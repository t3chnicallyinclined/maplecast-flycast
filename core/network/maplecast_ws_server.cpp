/*
	MapleCast WebSocket Server â€” binary mirror broadcast + JSON lobby on port 7200.

	On new client connect: sends full VRAM + PVR regs as initial sync, plus lobby status.
	Binary frames: delta TA commands broadcast to all clients.
	Text frames: JSON lobby (join, status) + binary gamepad input forwarding to UDP 7100.
*/
#include "maplecast_ws_server.h"
#include "maplecast_compress.h"
#include "maplecast_input_server.h"
#include "maplecast_gamestate.h"
#include "maplecast_mirror.h"
#include "replay_writer.h"
#include "emulator.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"
// live state migration (drainMigration): the proven state-sync JOIN recipe
#include "serialize.h"
#include "hw/pvr/Renderer_if.h"   // rend_start_rollback / rend_resync_after_rollback
#include "hw/pvr/spg.h"           // spg_getNextInterrupt
#include "hw/sh4/sh4_sched.h"     // sh4_sched_request / sh4_sched_is_scheduled
#include <climits>                // LONG_MAX (migration memory guard)
extern int vblank_schid;          // defined in hw/pvr/spg.cpp (same pattern as state_sync)
namespace gdrom { bool maplecast_gdrom_busy(); }   // disc-quiet capture guard (gdromv3.cpp)

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include "json/json.hpp"
#include <cstdio>

// Palette functions defined in maplecast_control_ws.cpp
void maplecast_palette_write(int startIdx, const std::vector<u16>& colors, bool persist);
void maplecast_palette_clear();
#include <cstring>
#include <mutex>
#include <set>
#include <map>
#include <algorithm>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>  // getaddrinfo (migration push)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <strings.h>   // strcasecmp
#include <unistd.h>
#include <sys/statvfs.h>  // disk usage (Tele-0.6)
#include <netdb.h>     // getaddrinfo (migration push)
#endif
#include "maplecast_compat.h"

using WsServer = websocketpp::server<websocketpp::config::asio>;
using ConnHdl = websocketpp::connection_hdl;
using json = nlohmann::json;

namespace maplecast_ws
{

static WsServer _ws;
static std::thread _wsThread;
static std::thread _statusThread;
static std::set<ConnHdl, std::owner_less<ConnHdl>> _connections;
static std::mutex _connMutex;
static std::atomic<int> _clientCount{0};
static bool _active = false;

// Lobby: connection â†’ slot mapping, queue tracking
static std::map<void*, int> _connSlot;

// Tele-0.3: per-connection RTT, populated by the pong handler. Cleared
// on close. Reader (broadcastStatus) holds _connMutex, same as the
// pong handler.
static std::map<void*, int32_t> _connRttUs;

// Tele-0.5: per-connection client-side render stats, populated by the
// client's stats reporter thread (1Hz). Cleared on close.
struct ClientStats {
	int32_t render_us_avg;
	int32_t render_us_max;
	int32_t arrival_ema_us;
	uint64_t packets;
};
static std::map<void*, ClientStats> _connClientStats;

// Control-only connections â€” browsers that connect directly to flycast for
// JSON control + 4-byte gamepad input but get the heavy TA frame downstream
// from the relay. These are flagged via a `{type:"control_only"}` JSON message
// sent immediately after WS open by the browser. broadcastBinary() skips them
// so we don't send 4 Mbps of TA frames out the home upstream just to have the
// browser drop them (the browser already has them via the relay's downstream).
//
// This is the bandwidth-saving half of the "direct upstream WS" architecture.
// The slot-collision-fixing half is that each browser now has its own dedicated
// flycast connection, so _connSlot[hdl] keys uniquely per browser instead of
// colliding on the relay's single multiplexed upstream.
static std::set<void*> _controlOnlyConns;
// TDW-subscribed connections ({"type":"subscribe","mode":"tdw"}): native TDW
// clients on a server that still runs the legacy legs for browsers. They get
// TDW1/TDWS + SYNC keyframes only — the multi-Mbps ZCST deltas / ZCS2 / GSTA /
// PALF side channels are skipped per-connection (the client would just count
// the bytes and drop them).
static std::set<void*> _tdwOnlyConns;

struct QueueEntry {
	void* key;
	std::string name;
	ConnHdl conn;
};

// ==================== P2P Spectator Relay Tree ====================
// Server feeds binary TA frames to 2-3 "seed" spectators.
// Seeds relay to children via WebRTC DataChannels (browser-side JS).
// Server manages topology, signals parent/child assignments via JSON.

// Tele-0.6 forward-decls -- defined near getFrameWorkWindow at the
// bottom of the file. Reads /proc on Linux, returns zeros on Windows
// (the mirror-client build doesn't run a server).
struct ServerOpStats {
	int64_t  uptime_s;          // process uptime in seconds
	int32_t  cpu_pct_x100;      // CPU % * 100 (e.g., 1234 = 12.34%) over the last sample interval
	int64_t  rss_mb;            // resident set size, MB
	int32_t  disk_used_pct;     // recordings dir filesystem used%
	int64_t  recordings_bytes;  // total .mcrec bytes in recordings dir
};
static ServerOpStats getServerOpStats(const std::string& recordings_dir);

// Tele-0.2 forward-decls -- ring + getter live near updateTelemetry().
struct FrameWorkWindow {
	uint32_t n;
	uint32_t publish_avg_us;
	uint32_t publish_p99_us;
	uint32_t publish_max_us;
	uint32_t compress_avg_us;
	uint32_t compress_max_us;
	// Tele-0.7: dirty pages distribution within the window.
	uint32_t dirty_avg;
	uint32_t dirty_p50;
	uint32_t dirty_p99;
	uint32_t dirty_max;
};
static FrameWorkWindow getFrameWorkWindow(int64_t windowUs);

// Tele-0.8: SCENE-CHANGE SYNC counters. Incremented in serverPublish
// when a full VRAM resnap is broadcast. Surfaced cumulative + rate.
extern std::atomic<uint64_t> _sceneChangeSyncCount;
extern std::atomic<uint64_t> _sceneChangeSyncBytes;

// Tele-0.11: connection lifecycle counters. _connectsTotal /
// _disconnectsTotal are monotonic since boot; the broadcast computes
// per-minute rates by diffing against a previous snapshot.
extern std::atomic<uint64_t> _connectsTotal;
extern std::atomic<uint64_t> _disconnectsTotal;

static const int MAX_SEEDS = 3;
static const int MAX_CHILDREN = 3;
static int _nextPeerId = 1;

struct RelayNode {
	std::string peerId;
	ConnHdl conn;
	void* connKey = nullptr;
	bool isPlayer = false;      // players get direct stream, never relay
	bool isSeed = false;        // seeds receive binary from server
	bool canRelay = true;       // client reported relay capability
	std::string parentId;       // empty for seeds/players
	std::vector<std::string> children;
	int64_t connectedAt = 0;
};

static std::map<std::string, RelayNode> _relayTree;
static std::vector<std::string> _seedPeers;
static std::map<void*, std::string> _connToPeerId;  // connKey â†’ peerId
static std::vector<QueueEntry> _queue;

// Loss detection state (forward-declared so buildMcsvCache can read it).
static bool _matchActive = false;

// Mid-match join: MCSV frame pre-built at match-start and cached here.
// onOpen sends it immediately on connect — same synchronous path as SYNC,
// no 1Hz status-thread delay. Cleared when the match ends.
static std::mutex              _mcsvMtx;
static std::vector<uint8_t>   _cachedMcsvFrame;
// Delay MCSV build by N frames after match-start so the round intro and
// any disc-asset loads complete before the savestate is captured. A client
// booting from a match-start savestate hits GD-ROM reads that fail without
// a ROM file ("Failed to locate bootfile"). 300 frames = 5 s at 60 fps.
static int _mcsvBuildCountdown = 0;

// Set by checkMatchEnd (status thread) when the countdown fires; drained by
// drainMcsvCapture() on the emu thread so dc_serialize runs at a clean SH4
// frame boundary (SR.BL=0) instead of mid-interrupt.
static std::atomic<bool> _mcsvCaptureRequested{false};

static void buildMcsvCache()
{
	size_t stateSize = 0;
	uint8_t* stateData = maplecast_mirror::buildFullSaveState(stateSize);
	if (!stateData || stateSize == 0) {
		printf("[maplecast-ws] buildMcsvCache: buildFullSaveState failed\n");
		free(stateData);
		return;
	}
	// Compress: "MCSV"(4) + uint32_t(uncompSize) + ZCST_blob
	MirrorCompressor comp;
	comp.init(stateSize + 128);
	size_t compSize = 0; uint64_t compUs = 0;
	const uint8_t* compData = comp.compress(stateData, (uint32_t)stateSize, compSize, compUs, 3);
	free(stateData);
	std::vector<uint8_t> frame(8 + compSize);
	memcpy(frame.data(), "MCSV", 4);
	uint32_t ss = (uint32_t)stateSize;
	memcpy(frame.data() + 4, &ss, 4);
	memcpy(frame.data() + 8, compData, compSize);
	comp.destroy();
	{
		std::lock_guard<std::mutex> lk(_mcsvMtx);
		_cachedMcsvFrame = std::move(frame);
	}
	printf("[maplecast-ws] MCSV cached: %.1f MB raw -> %.1f MB compressed\n",
	       stateSize / (1024.0*1024.0), compSize / (1024.0*1024.0));
}

// Emu-thread entry: called once per frame from serverPublish. Runs the actual
// dc_serialize capture ONLY when checkMatchEnd has requested it — at this point
// the SH4 is between frames (SR.BL=0), so the savestate is clean and replica
// clients can resume it without "SH4 exception when blocked".
void drainMcsvCapture()
{
	if (_mcsvCaptureRequested.exchange(false, std::memory_order_acquire)) {
		buildMcsvCache();
		printf("[maplecast-ws] MCSV built on emu thread (clean SH4 boundary, SR.BL=0)\n");
	}
}

// Loss detection state (continued — _matchActive forward-declared above)
static bool _matchEndHandled = false;
static int64_t _matchEndTime = 0; // when match ended (for delay before kick)
static int _pendingKickSlot = -1; // loser slot to evict if client doesn't self-disconnect

// Idle-kick threshold: a connected player who hasn't sent a button-state
// CHANGE in this many microseconds gets evicted from their slot. The clock
// is seeded fresh on join/reconnect so a slow joiner has the full window.
static constexpr int64_t IDLE_KICK_THRESHOLD_US = 300LL * 1000000LL; // 5 minutes

// Forward declaration â€” defined further down, used by checkMatchEnd().
static void broadcastStatus();

// Server-side eviction primitive used by both the loser-kick path and the
// idle-kick path. Cleans up _connSlot, calls input::disconnectPlayer, and
// pushes {kicked, reason} + {assigned, slot:-1} to the evicted client so its
// UI resets without waiting for an onClose. Returns true if a slot was kicked.
//
// `slot`   â€” which player slot to evict (0 or 1)
// `reason` â€” short token sent to the client ("match_lost", "idle", ...)
static bool kickSlot(int slot, const char* reason)
{
	if (slot < 0 || slot > 1) return false;
	const auto& p = maplecast_input::getPlayer(slot);
	if (!p.connected) return false;

	printf("[maplecast-ws] SERVER KICK: P%d (%s) â€” reason=%s\n",
		slot + 1, p.name, reason);

	// Walk _connSlot looking for the connection bound to this slot, drop the
	// mapping, and resolve a ConnHdl so we can send a kicked message.
	ConnHdl evictedConn;
	bool foundConn = false;
	{
		std::lock_guard<std::mutex> lock(_connMutex);
		for (auto it = _connSlot.begin(); it != _connSlot.end(); ) {
			if (it->second == slot) {
				for (const auto& c : _connections) {
					try {
						if ((void*)_ws.get_con_from_hdl(c).get() == it->first) {
							evictedConn = c;
							foundConn = true;
							break;
						}
					} catch (...) {}
				}
				it = _connSlot.erase(it);
			} else {
				++it;
			}
		}
	}

	maplecast_input::disconnectPlayer(slot);
	maplecast_gamestate::restorePlayerNames();

	if (foundConn) {
		json kickMsg = {{"type", "kicked"}, {"reason", reason}};
		try { _ws.send(evictedConn, kickMsg.dump(), websocketpp::frame::opcode::text); } catch (...) {}
		json reset = {{"type", "assigned"}, {"slot", -1}};
		try { _ws.send(evictedConn, reset.dump(), websocketpp::frame::opcode::text); } catch (...) {}
	}
	return true;
}

// Telemetry from mirror publish
static Telemetry _telemetry{};
static std::mutex _telemetryMutex;

// UDP socket for forwarding browser input to input server
static int _udpSock = -1;
static struct sockaddr_in _udpDest;

static int getSlotForConn(ConnHdl hdl)
{
	try {
		void* key = (void*)_ws.get_con_from_hdl(hdl).get();
		auto it = _connSlot.find(key);
		if (it != _connSlot.end()) return it->second;
	} catch (...) {}
	return -1;
}

static json getStatus()
{
	auto slotInfo = [](int i) -> json {
		const auto& p = maplecast_input::getPlayer(i);
		if (!p.connected)
			return nullptr;
		const char* typeStr = (p.type == maplecast_input::InputType::NobdUDP) ? "hardware" : "browser";
		// Render the input source's IP/port for forensics. srcIP/srcPort
		// are network byte order; for slots that have never received a
		// non-loopback packet they're zero.
		std::string srcIpStr;
		int srcPortVal = 0;
		if (p.srcIP != 0) {
			struct in_addr ia; ia.s_addr = p.srcIP;
			srcIpStr = inet_ntoa(ia);
			srcPortVal = ntohs(p.srcPort);
		}
		// Tele-0.3: surface RTT for whichever connection is bound to this
		// slot. -1 means "no pong received yet" (just-joined window).
		// Tele-0.5: same lookup also gets the client's self-reported
		// render stats. -1 means "no client_stats received yet".
		int32_t rttUs = -1;
		int32_t cliRenderAvg = -1;
		int32_t cliRenderMax = -1;
		int32_t cliArrivalEma = -1;
		{
			std::lock_guard<std::mutex> lock(_connMutex);
			for (const auto& kv : _connSlot) {
				if (kv.second == i) {
					auto rIt = _connRttUs.find(kv.first);
					if (rIt != _connRttUs.end()) rttUs = rIt->second;
					auto cIt = _connClientStats.find(kv.first);
					if (cIt != _connClientStats.end()) {
						cliRenderAvg  = cIt->second.render_us_avg;
						cliRenderMax  = cIt->second.render_us_max;
						cliArrivalEma = cIt->second.arrival_ema_us;
					}
					break;
				}
			}
		}
		return {
			{"id", std::string(p.id).substr(0, 8)},
			{"name", std::string(p.name)},
			{"device", std::string(p.device)},
			{"connected", true},
			{"type", typeStr},
			{"pps", p.packetsPerSec},
			{"cps", p.changesPerSec},
			{"src_ip", srcIpStr},
			{"src_port", srcPortVal},
			{"rtt_us", rttUs},
			{"client_render_us_avg",  cliRenderAvg},
			{"client_render_us_max",  cliRenderMax},
			{"client_arrival_ema_us", cliArrivalEma},
		};
	};
	int players = (maplecast_input::getPlayer(0).connected ? 1 : 0)
	            + (maplecast_input::getPlayer(1).connected ? 1 : 0);
	// Each browser tab opens 2 WebSocket connections (parent page + iframe mirror)
	int viewers = (_clientCount.load() - players) / 2;
	if (viewers < 0) viewers = 0;

	// Snapshot queue + relay tree under _connMutex (caller may not hold it).
	json queueList = json::array();
	int seedCount = 0;
	int treeSize = 0;
	{
		std::lock_guard<std::mutex> lock(_connMutex);
		for (const auto& q : _queue)
			queueList.push_back(q.name);
		seedCount = (int)_seedPeers.size();
		treeSize = (int)_relayTree.size();
	}

	Telemetry t;
	{
		std::lock_guard<std::mutex> lock(_telemetryMutex);
		t = _telemetry;
	}
	json status;
	status["type"] = "status";
	status["p1"] = slotInfo(0);
	status["p2"] = slotInfo(1);
	status["spectators"] = viewers;
	status["queue"] = queueList;
	status["frame"] = t.frameNum;
	status["stream_kbps"] = (int64_t)(t.compressedSize * t.fps * 8 / 1024);
	status["raw_kbps"] = (int64_t)(t.deltaSize * t.fps * 8 / 1024);
	status["publish_us"] = (int64_t)t.publishUs;
	status["compress_us"] = (int64_t)t.compressUs;
	status["compression_ratio"] = t.compressedSize > 0 ? (double)t.deltaSize / t.compressedSize : 1.0;
	status["fps"] = (int64_t)t.fps;

	// Tele-0.2: server-side frame-work windows. publish_us / compress_us
	// above are the most-recent sample only -- these expose the worst
	// case + average over the last 1s and 30s so spikes are visible.
	// frame_budget_pct = max publish over the window relative to the
	// 16.67ms 60Hz budget. >50% means we're at risk of stalling under
	// load.
	// Tele-0.6: server operational stats.
	{
		const char* recDir = std::getenv("MAPLECAST_RECORDINGS_DIR");
		ServerOpStats op = getServerOpStats(recDir ? recDir : "recordings");
		status["server_ops"] = {
			{"uptime_s",         op.uptime_s},
			{"cpu_pct_x100",     op.cpu_pct_x100},
			{"rss_mb",           op.rss_mb},
			{"disk_used_pct",    op.disk_used_pct},
		};
	}

	auto fw1  = getFrameWorkWindow( 1'000'000);
	auto fw30 = getFrameWorkWindow(30'000'000);
	auto fwJson = [](const FrameWorkWindow& w) -> json {
		return {
			{"n",                (int64_t)w.n},
			{"publish_avg_us",   (int64_t)w.publish_avg_us},
			{"publish_p99_us",   (int64_t)w.publish_p99_us},
			{"publish_max_us",   (int64_t)w.publish_max_us},
			{"publish_max_pct",  (int64_t)((w.publish_max_us * 100) / 16667)},
			{"compress_avg_us",  (int64_t)w.compress_avg_us},
			{"compress_max_us",  (int64_t)w.compress_max_us},
			// Tele-0.7 dirty page distribution within the same window
			{"dirty_avg",        (int64_t)w.dirty_avg},
			{"dirty_p50",        (int64_t)w.dirty_p50},
			{"dirty_p99",        (int64_t)w.dirty_p99},
			{"dirty_max",        (int64_t)w.dirty_max},
		};
	};
	status["frame_work_1s"]  = fwJson(fw1);
	status["frame_work_30s"] = fwJson(fw30);

	// Tele-0.8: SCENE-CHANGE SYNC counters (cumulative since boot).
	status["sync_bursts"] = {
		{"count_total",        (int64_t)_sceneChangeSyncCount.load(std::memory_order_relaxed)},
		{"bytes_total",        (int64_t)_sceneChangeSyncBytes.load(std::memory_order_relaxed)},
	};

	// Tele-0.11: connection lifecycle counters since boot.
	status["conn_lifecycle"] = {
		{"connects_total",     (int64_t)_connectsTotal.load(std::memory_order_relaxed)},
		{"disconnects_total",  (int64_t)_disconnectsTotal.load(std::memory_order_relaxed)},
		{"current",            _clientCount.load(std::memory_order_relaxed)},
	};

	// Tele-0.12: derived anomaly flags. Cheap booleans the dashboard
	// can show as warning lights / log to a separate alerts stream.
	{
		json anomalies = json::array();
		// Frame budget overrun: any publish_us in the last 30s window
		// exceeded the 16.67ms 60Hz budget. Sustained = bad.
		if (fw30.publish_max_us > 16667)
			anomalies.push_back("frame_overrun_30s");
		// Latch p99 over 2 frames in either slot's last-1s window.
		if (auto p1ls = maplecast_input::getLatchStatsWindow(0, 1'000'000); p1ls.p99DeltaUs > 33333)
			anomalies.push_back("p1_latch_spike_1s");
		if (auto p2ls = maplecast_input::getLatchStatsWindow(1, 1'000'000); p2ls.p99DeltaUs > 33333)
			anomalies.push_back("p2_latch_spike_1s");
		// Compression ratio drop -- a TA-stream-shape regression looks
		// like ratio plummeting because the new bytes don't compress.
		// Threshold 4x is below typical 6-15x range; sustained <4x for
		// the 30s window means something's wrong (would have flagged
		// the Arcade-defaults bug).
		if (t.compressedSize > 0 && t.deltaSize > 0
		    && (double)t.deltaSize / (double)t.compressedSize < 4.0)
			anomalies.push_back("low_compression_ratio");
		status["anomalies"] = anomalies;
	}
	status["dirty"] = t.dirtyPages;
	status["registering"] = maplecast_input::isRegistering();
	status["web_registering"] = maplecast_input::isWebRegistering();
	status["web_registering_user"] = maplecast_input::isWebRegistering() ?
		maplecast_input::webRegisteringUsername() : "";
	status["sticks"] = maplecast_input::registeredStickCount();
	status["relay_seeds"] = seedCount;
	status["relay_nodes"] = treeSize;

	// Phase A â€” per-slot input latch telemetry. Sourced from the
	// LatchStatsAccum ring buffer that ggpo::getLocalInput() writes to
	// once per vblank for slots 0/1. Frontend renders these as a
	// histogram + counter set in the diagnostics overlay (A.6) so
	// players can see how their input timing relates to the latch
	// boundary.
	//   delta_us avg/p99/min/max â€” distribution of (t_latch - t_packet_arrival)
	//                              over the last ~256 latches (~4.3 s @ 60 Hz)
	//   total_latches             â€” every CMD9 vblank since boot
	//   latches_with_data         â€” vblanks where the network thread had
	//                              touched the slot since the previous latch
	//                              (= the slot saw a fresh packet this frame)
	//   last_seq, last_frame      â€” for live drift / diagnostics
	// Tele-0.1: surface time-windowed buckets (last 1s + last 30s)
	// alongside the existing 256-sample window. The cumulative stats
	// carry the long-term aggregate; the windowed ones expose recent
	// spikes that get smoothed out during low-input periods.
	auto latchInfoJson = [](int slot) -> json {
		auto s   = maplecast_input::getLatchStats(slot);
		auto w1  = maplecast_input::getLatchStatsWindow(slot,  1'000'000); // 1s
		auto w30 = maplecast_input::getLatchStatsWindow(slot, 30'000'000); // 30s
		return {
			{"total_latches",     (int64_t)s.totalLatches},
			{"latches_with_data", (int64_t)s.latchesWithData},
			{"avg_delta_us",      s.avgDeltaUs},
			{"p99_delta_us",      s.p99DeltaUs},
			{"min_delta_us",      s.minDeltaUs},
			{"max_delta_us",      s.maxDeltaUs},
			{"last_packet_seq",   (int64_t)s.lastPacketSeq},
			{"last_frame",        (int64_t)s.lastFrameNum},
			{"window_1s", {
				{"n",            (int64_t)w1.nSamples},
				{"avg_delta_us", w1.avgDeltaUs},
				{"p99_delta_us", w1.p99DeltaUs},
				{"max_delta_us", w1.maxDeltaUs},
			}},
			{"window_30s", {
				{"n",            (int64_t)w30.nSamples},
				{"avg_delta_us", w30.avgDeltaUs},
				{"p99_delta_us", w30.p99DeltaUs},
				{"max_delta_us", w30.maxDeltaUs},
			}},
		};
	};
	json latchStats;
	latchStats["p1"] = latchInfoJson(0);
	latchStats["p2"] = latchInfoJson(1);
	status["latch_stats"] = latchStats;

	// Phase B â€” frame phase publication. Tells the browser-side gamepad
	// scheduler when the most recent vblank latch fired and how long the
	// vblank interval is, so it can phase-align its send pattern to land
	// 2-4 ms before the next latch (instead of the random ~8 ms phase
	// jitter inherent to rAF-aligned sends). The biggest single latency
	// win for browser players because it cuts average input-to-latch lag
	// in half.
	//
	// All times in microseconds, monotonic clock since process start
	// (CLOCK_MONOTONIC). Browser maintains its own offset by sampling
	// `t_last_latch_us` against its local performance.now() at receive
	// time and tracking the rolling delta.
	{
		json fp;
		fp["frame"]            = (int64_t)maplecast_mirror::currentFrame();
		fp["t_last_latch_us"]  = (int64_t)maplecast_mirror::lastLatchTimeUs();
		fp["frame_period_us"]  = (int64_t)maplecast_mirror::framePeriodUs();
		// t_next_latch_us is the predicted next vblank time. The browser
		// could compute this itself, but pre-computing here keeps the
		// client logic simpler and gives the server a single source of
		// truth in case we ever change the prediction model.
		const int64_t period = maplecast_mirror::framePeriodUs();
		const int64_t lastLatch = maplecast_mirror::lastLatchTimeUs();
		fp["t_next_latch_us"]  = lastLatch + period;
		// Phase B guard window in microseconds â€” exposed so the browser
		// can shift its sends to land just OUTSIDE the guard window
		// (avoiding the deferred-by-one-frame penalty under
		// ConsistencyFirst).
		fp["guard_us"] = (int64_t)maplecast_input::getGuardUs();
		status["frame_phase"] = fp;
	}

	// Phase B â€” per-slot latch policy (latency / consistency). Lets the
	// browser show the current policy in the diagnostics overlay and offer
	// the live A/B toggle button (B.9).
	{
		json policy;
		auto policyName = [](maplecast_input::LatchPolicy p) {
			return (p == maplecast_input::LatchPolicy::ConsistencyFirst) ? "consistency" : "latency";
		};
		policy["p1"] = policyName(maplecast_input::getLatchPolicy(0));
		policy["p2"] = policyName(maplecast_input::getLatchPolicy(1));
		status["latch_policy"] = policy;
	}

	// Game state for leaderboard/stats
	maplecast_gamestate::GameState gs;
	maplecast_gamestate::readGameState(gs);
	if (gs.in_match) {
		json game;
		game["in_match"] = true;
		game["timer"] = gs.game_timer;
		game["stage"] = gs.stage_id;
		game["p1_combo"] = gs.p1_combo;
		game["p2_combo"] = gs.p2_combo;
		game["p1_meter"] = gs.p1_meter_level;
		game["p2_meter"] = gs.p2_meter_level;
		// Character health: 3 per player
		json p1hp = json::array();
		json p2hp = json::array();
		json p1chars = json::array();
		json p2chars = json::array();
		for (int i = 0; i < 3; i++) {
			p1hp.push_back(gs.chars[i*2].health);
			p2hp.push_back(gs.chars[i*2+1].health);
			p1chars.push_back(gs.chars[i*2].character_id);
			p2chars.push_back(gs.chars[i*2+1].character_id);
		}
		game["p1_hp"] = p1hp;
		game["p2_hp"] = p2hp;
		game["p1_chars"] = p1chars;
		game["p2_chars"] = p2chars;
		status["game"] = game;
	}
	return status;
}

// Idle-kick â€” evict any player who hasn't pressed a button in 30 seconds.
// Runs from the status thread at 1Hz alongside checkMatchEnd. Decision is
// based on `lastChangeUs` (button-state changes), not raw packet rate, so a
// player whose stick polls at 250Hz but never presses anything still counts
// as idle. Promotes the next queue head into the freed slot.
static void checkIdleKick()
{
	int slot = maplecast_input::findIdlePlayer(IDLE_KICK_THRESHOLD_US);
	if (slot < 0) return;

	bool kicked = kickSlot(slot, "idle");
	if (!kicked) return;

	// Promote queue head if there is one (mirrors the post-match loop).
	std::string nextName;
	{
		std::lock_guard<std::mutex> lock(_connMutex);
		if (!_queue.empty()) {
			auto& next = _queue.front();
			nextName = next.name;
			json yourTurn;
			yourTurn["type"] = "your_turn";
			yourTurn["msg"] = "It's your turn! The cabinet is open.";
			try { _ws.send(next.conn, yourTurn.dump(), websocketpp::frame::opcode::text); } catch (...) {}
		}
	}
	if (!nextName.empty())
		printf("[maplecast-ws] Notified %s: idle slot freed up\n", nextName.c_str());

	broadcastStatus();
}

static void checkMatchEnd()
{
	maplecast_gamestate::GameState gs;
	maplecast_gamestate::readGameState(gs);

	if (gs.in_match) {
		// Latch _matchActive on the rising edge only. Resetting it every
		// tick would also reset _matchEndHandled and re-fire match_end on
		// every status broadcast â€” which is exactly the loop bug that
		// produced 115 ghost match rows in 30 seconds on 2026-04-06.
		if (!_matchActive) {
			_matchActive = true;
			_matchEndHandled = false;
			// Auto-save match-start state so replica clients can download it
			// and boot directly into this match without going through char select.
			// Slot 98 = replica client sync slot (leaves slot 0 = user autoload).
			try {
				dc_savestate(98, nullptr, 0);
				printf("[maplecast-ws] match-start savestate written to slot 98\n");
			} catch (const std::exception& e) {
				printf("[maplecast-ws] match-start savestate FAILED: %s\n", e.what());
			} catch (...) {
				printf("[maplecast-ws] match-start savestate FAILED (unknown)\n");
			}
			// Delay MCSV build: the round intro triggers GD-ROM disc reads for
			// ~5s after in_match. A savestate captured mid-read causes
			// "Failed to locate bootfile" on no-ROM replica clients.
			// NOTE: this countdown is decremented in checkMatchEnd, which runs on
			// the 1 Hz status thread — so the unit is SECONDS, not frames. The old
			// value of 300 meant 5 MINUTES (the "300 frames = 5s" comment assumed a
			// 60 Hz tick that doesn't exist here), so the MCSV never built in a
			// fresh match. 6 = ~6 s, clearing the intro disc I/O.
			_mcsvBuildCountdown = 6;
			printf("[maplecast-ws] MCSV build scheduled in 6s (round-intro disc I/O guard)\n");
		}

		// Deferred MCSV build (counts down from match-start). We DON'T capture
		// here — this runs on the 1Hz status thread, and dc_serialize reads live
		// SH4 registers; capturing mid-execution can snapshot SR.BL=1 (the CPU
		// inside the vblank interrupt handler), which crashes replica clients on
		// load with "SH4 exception when blocked". Instead request the capture and
		// let serverPublish (emu thread, between frames, SR.BL=0) run it.
		if (_mcsvBuildCountdown > 0) {
			if (--_mcsvBuildCountdown == 0) {
				_mcsvCaptureRequested.store(true, std::memory_order_release);
				printf("[maplecast-ws] MCSV capture requested — emu thread will build at next frame boundary\n");
			}
		}

		// Periodic MCSV broadcast: DISABLED by default (MAPLECAST_MCSV_PERIODIC=1 re-enables). It ships a
		// ~6.5MB full save-state to ALL clients every ~60s so mid-match-joining RELAY/REPLICA (native SH4)
		// clients catch up — but BROWSER render clients RECEIVE IT AND DROP IT (no SH4), so it was pure
		// wasted bandwidth (the periodic ~6.5MB "super spike" red herring). We are not running native
		// clients now; onOpen still seeds any that connect, and replica clients only apply the FIRST MCSV.
		static const bool _mcsvPeriodic = [](){ const char* e = std::getenv("MAPLECAST_MCSV_PERIODIC");
			return e && e[0] && e[0] != '0'; }();
		if (_mcsvPeriodic) {
			static int _mcsvBroadcastTick = 0;
			if (++_mcsvBroadcastTick >= 60) {
				_mcsvBroadcastTick = 0;
				std::vector<uint8_t> mcsvCopy;
				{ std::lock_guard<std::mutex> lk(_mcsvMtx); mcsvCopy = _cachedMcsvFrame; }
				if (!mcsvCopy.empty()) {
					std::lock_guard<std::mutex> lock(_connMutex);
					for (auto& conn : _connections) {
						try { _ws.send(conn, mcsvCopy.data(), mcsvCopy.size(),
						               websocketpp::frame::opcode::binary); }
						catch (...) {}
					}
					printf("[maplecast-ws] MCSV periodic broadcast: %.1f MB to %zu clients\n",
					       mcsvCopy.size() / (1024.0*1024.0), _connections.size());
				}
			}
		}

		// Check if all 3 chars on one side are dead
		bool p1dead = (gs.chars[0].health == 0 && gs.chars[2].health == 0 && gs.chars[4].health == 0);
		bool p2dead = (gs.chars[1].health == 0 && gs.chars[3].health == 0 && gs.chars[5].health == 0);

		if ((p1dead || p2dead) && !_matchEndHandled) {
			_matchEndHandled = true;
			_matchEndTime = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::high_resolution_clock::now().time_since_epoch()).count();

			int loserSlot = p1dead ? 0 : 1;
			int winnerSlot = p1dead ? 1 : 0;
			_pendingKickSlot = loserSlot;
			const auto& loser = maplecast_input::getPlayer(loserSlot);
			const auto& winner = maplecast_input::getPlayer(winnerSlot);
			printf("[maplecast-ws] MATCH END: P%d (%s) wins! P%d (%s) eliminated.\n",
				winnerSlot+1, winner.name, loserSlot+1, loser.name);

			// Notify all clients
			json endMsg;
			endMsg["type"] = "match_end";
			endMsg["winner"] = winnerSlot;
			endMsg["winner_name"] = std::string(winner.name);
			endMsg["loser"] = loserSlot;
			endMsg["loser_name"] = std::string(loser.name);
			std::string endStr = endMsg.dump();
			{
				std::lock_guard<std::mutex> lock(_connMutex);
				for (auto& conn : _connections)
					try { _ws.send(conn, endStr, websocketpp::frame::opcode::text); } catch (...) {}
			}
		}
	}
	else if (_matchActive && _matchEndHandled)
	{
		// Match ended and game returned to non-match state (character select, etc.)
		// Client gets 3s grace via match_end â†’ leaveGame() self-disconnect.
		// Server kicks at 5s as a safety net (closed tab, killed JS, malicious client).
		int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::high_resolution_clock::now().time_since_epoch()).count();

		if (now - _matchEndTime > 5000) {
			_matchActive = false;
			_mcsvBuildCountdown = 0;
			// Clear MCSV cache — char-select state is useless to send to new clients.
			{ std::lock_guard<std::mutex> lk(_mcsvMtx); _cachedMcsvFrame.clear(); }

			// King-of-the-hill: only evict loser if someone is waiting in queue.
			// Empty queue â†’ winner stays on, loser keeps slot for rematch.
			bool kicked = false;
			if (!_queue.empty() && _pendingKickSlot >= 0) {
				kicked = kickSlot(_pendingKickSlot, "match_lost");
			}
			_pendingKickSlot = -1;

			// Notify the first person in queue it's their turn (slot is now free if we kicked).
			if (!_queue.empty()) {
				auto next = _queue.front();
				json yourTurn;
				yourTurn["type"] = "your_turn";
				yourTurn["msg"] = "It's your turn! Press Start to play!";
				try {
					_ws.send(next.conn, yourTurn.dump(), websocketpp::frame::opcode::text);
				} catch (...) {}
				printf("[maplecast-ws] Notified %s: it's your turn!%s\n",
					next.name.c_str(), kicked ? " (loser kicked)" : "");
			}

			if (kicked)
				broadcastStatus();
		}
	}
}

// ==================== Relay Topology Helpers ====================

static std::string generatePeerId()
{
	return "p" + std::to_string(_nextPeerId++);
}

static void sendJson(ConnHdl hdl, const json& msg)
{
	try { _ws.send(hdl, msg.dump(), websocketpp::frame::opcode::text); }
	catch (...) {}
}

// Find the shallowest relay node with available child slots (BFS)
static std::string findBestParent()
{
	// BFS from seeds outward
	std::vector<std::string> queue;
	for (auto& sid : _seedPeers) queue.push_back(sid);

	size_t idx = 0;
	while (idx < queue.size()) {
		const std::string& id = queue[idx++];
		auto it = _relayTree.find(id);
		if (it == _relayTree.end()) continue;
		if ((int)it->second.children.size() < MAX_CHILDREN && it->second.canRelay)
			return id;
		for (auto& child : it->second.children)
			queue.push_back(child);
	}
	return "";  // tree is full
}

static void makeSeed(const std::string& peerId)
{
	auto it = _relayTree.find(peerId);
	if (it == _relayTree.end()) return;
	it->second.isSeed = true;
	it->second.parentId.clear();
	_seedPeers.push_back(peerId);

	// Send SYNC to new seed
	size_t syncSize = 4 + 4 + VRAM_SIZE + 4 + pvr_RegSize;
	std::vector<uint8_t> syncBuf(syncSize);
	uint8_t* dst = syncBuf.data();
	memcpy(dst, "SYNC", 4); dst += 4;
	uint32_t vs = VRAM_SIZE;
	memcpy(dst, &vs, 4); dst += 4;
	memcpy(dst, &vram[0], VRAM_SIZE); dst += VRAM_SIZE;
	uint32_t ps = pvr_RegSize;
	memcpy(dst, &ps, 4); dst += 4;
	memcpy(dst, pvr_regs, pvr_RegSize);
	try { _ws.send(it->second.conn, syncBuf.data(), syncSize, websocketpp::frame::opcode::binary); }
	catch (...) {}

	sendJson(it->second.conn, {{"type", "relay_role"}, {"role", "seed"}, {"peerId", peerId}});
	printf("[relay] %s promoted to SEED (%d seeds)\n", peerId.c_str(), (int)_seedPeers.size());
}

static void assignChild(const std::string& childId, const std::string& parentId)
{
	auto childIt = _relayTree.find(childId);
	auto parentIt = _relayTree.find(parentId);
	if (childIt == _relayTree.end() || parentIt == _relayTree.end()) return;

	childIt->second.parentId = parentId;
	parentIt->second.children.push_back(childId);

	std::string role = childIt->second.canRelay ? "relay" : "leaf";
	sendJson(childIt->second.conn, {{"type", "relay_role"}, {"role", role}, {"peerId", childId}});
	sendJson(childIt->second.conn, {{"type", "relay_assign_parent"}, {"parentId", parentId}});
	sendJson(parentIt->second.conn, {{"type", "relay_assign_child"}, {"childId", childId}});

	printf("[relay] %s assigned to parent %s (role=%s)\n", childId.c_str(), parentId.c_str(), role.c_str());
}

static void removeFromTree(const std::string& peerId)
{
	auto it = _relayTree.find(peerId);
	if (it == _relayTree.end()) return;

	// Remove from parent's children list
	if (!it->second.parentId.empty()) {
		auto parentIt = _relayTree.find(it->second.parentId);
		if (parentIt != _relayTree.end()) {
			auto& pc = parentIt->second.children;
			pc.erase(std::remove(pc.begin(), pc.end(), peerId), pc.end());
			sendJson(parentIt->second.conn, {{"type", "relay_remove_child"}, {"childId", peerId}});
		}
	}

	// Remove from seed list
	if (it->second.isSeed)
		_seedPeers.erase(std::remove(_seedPeers.begin(), _seedPeers.end(), peerId), _seedPeers.end());

	// Orphan children â€” reassign them
	std::vector<std::string> orphans = it->second.children;
	_relayTree.erase(it);

	for (auto& orphanId : orphans) {
		auto orphanIt = _relayTree.find(orphanId);
		if (orphanIt == _relayTree.end()) continue;
		orphanIt->second.parentId.clear();
		sendJson(orphanIt->second.conn, {{"type", "relay_orphaned"}});

		// Try to find a new parent
		if ((int)_seedPeers.size() < MAX_SEEDS && orphanIt->second.canRelay) {
			makeSeed(orphanId);
		} else {
			std::string newParent = findBestParent();
			if (!newParent.empty())
				assignChild(orphanId, newParent);
			else
				makeSeed(orphanId);  // no room, make it a seed
		}
	}

	printf("[relay] %s removed from tree, %d orphans reassigned\n", peerId.c_str(), (int)orphans.size());
}

static const char* stickEventKindStr(maplecast_input::StickEventKind k)
{
	using K = maplecast_input::StickEventKind;
	switch (k) {
		case K::Register:   return "register";
		case K::Unregister: return "unregister";
		case K::Online:     return "online";
		case K::Offline:    return "offline";
	}
	return "unknown";
}

static void broadcastStickEvents()
{
	auto events = maplecast_input::drainStickEvents();
	if (events.empty()) return;

	json msg;
	msg["type"] = "stick_event";
	msg["events"] = json::array();
	for (const auto& ev : events) {
		struct in_addr ia; ia.s_addr = ev.srcIP;
		msg["events"].push_back({
			{"kind",     stickEventKindStr(ev.kind)},
			{"username", ev.username},
			{"ip",       inet_ntoa(ia)},
			{"port",     ntohs(ev.srcPort)},
			{"ts",       ev.ts},
		});
	}
	std::string s = msg.dump();
	std::lock_guard<std::mutex> lock(_connMutex);
	for (auto& conn : _connections)
	{
		try { _ws.send(conn, s, websocketpp::frame::opcode::text); }
		catch (...) {}
	}
}

// Tele-0.3: send a WS PING to each open connection with a microsecond
// timestamp payload. Native + browser clients echo it back as PONG;
// the pong_handler computes RTT = now - ts. Called once per status
// broadcast (~1Hz). websocketpp::ping is non-blocking -- it queues the
// frame on the connection's send strand.
static void pingAllForRtt()
{
	const int64_t nowUs = (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	const std::string payload = std::to_string(nowUs);
	std::vector<ConnHdl> snapshot;
	{
		std::lock_guard<std::mutex> lock(_connMutex);
		snapshot.assign(_connections.begin(), _connections.end());
	}
	for (auto& hdl : snapshot) {
		try {
			websocketpp::lib::error_code ec;
			_ws.ping(hdl, payload, ec);
		} catch (...) {}
	}
}

static void broadcastStatus()
{
	// Drain stick events first so reload-detect / DB persistence reach
	// listeners (collector + browsers) on every status tick.
	broadcastStickEvents();

	std::string status = getStatus().dump();
	std::lock_guard<std::mutex> lock(_connMutex);
	for (auto& conn : _connections)
	{
		try { _ws.send(conn, status, websocketpp::frame::opcode::text); }
		catch (...) {}
	}
}

static void onOpen(ConnHdl hdl)
{
	{
		std::lock_guard<std::mutex> lock(_connMutex);
		_connections.insert(hdl);
		_clientCount++;
	}
	_connectsTotal.fetch_add(1, std::memory_order_relaxed);  // Tele-0.11
	printf("[maplecast-ws] client connected (%d total)\n", _clientCount.load());

	// Always send SYNC on connect â€” backward compat for clients without relay.js
	// (test-renderer.html, standalone clients, etc.)
	// The bandwidth savings come from delta frames (broadcastBinary), not SYNC.
	// Non-seed relay clients will also get SYNC from their parent via WebRTC,
	// but getting it from the server first means they can start rendering immediately.
	size_t syncSize = 4 + 4 + VRAM_SIZE + 4 + pvr_RegSize;
	std::vector<uint8_t> syncBuf(syncSize);
	uint8_t* dst = syncBuf.data();

	memcpy(dst, "SYNC", 4); dst += 4;
	uint32_t vs = VRAM_SIZE;
	memcpy(dst, &vs, 4); dst += 4;
	memcpy(dst, &vram[0], VRAM_SIZE); dst += VRAM_SIZE;
	uint32_t ps = pvr_RegSize;
	memcpy(dst, &ps, 4); dst += 4;
	memcpy(dst, pvr_regs, pvr_RegSize);

	try {
		MirrorCompressor syncComp;
		syncComp.init(syncSize + 128);
		size_t compSyncSize = 0;
		uint64_t compUs = 0;
		const uint8_t* compSync = syncComp.compress(syncBuf.data(), (uint32_t)syncSize, compSyncSize, compUs, 3);
		_ws.send(hdl, compSync, compSyncSize, websocketpp::frame::opcode::binary);
		syncComp.destroy();
		printf("[maplecast-ws] sent compressed sync: %.1f MB -> %.1f MB (%.1fx) in %lums\n",
			syncSize / (1024.0 * 1024.0), compSyncSize / (1024.0 * 1024.0),
			(double)syncSize / compSyncSize, compUs / 1000);
	} catch (...) {
		printf("[maplecast-ws] failed to send initial sync\n");
	}

	// If a match is live, send the pre-built MCSV savestate immediately — same
	// synchronous path as the SYNC frame above. No queue, no 1Hz delay.
	if (_matchActive) {
		std::lock_guard<std::mutex> lk(_mcsvMtx);
		if (!_cachedMcsvFrame.empty()) {
			try {
				_ws.send(hdl, _cachedMcsvFrame.data(), _cachedMcsvFrame.size(),
				         websocketpp::frame::opcode::binary);
				printf("[maplecast-ws] MCSV sent immediately on connect (%.1f MB compressed)\n",
				       _cachedMcsvFrame.size() / (1024.0 * 1024.0));
			} catch (...) {
				printf("[maplecast-ws] MCSV send failed on connect\n");
			}
		}
	}

	// Send lobby status
	try {
		_ws.send(hdl, getStatus().dump(), websocketpp::frame::opcode::text);
	} catch (...) {}
}

static void onClose(ConnHdl hdl)
{
	// Clean up slot assignment
	int slot = getSlotForConn(hdl);
	if (slot >= 0) {
		maplecast_input::disconnectPlayer(slot);
	}

	void* key = nullptr;
	{
		std::lock_guard<std::mutex> lock(_connMutex);
		_connections.erase(hdl);
		try {
			key = (void*)_ws.get_con_from_hdl(hdl).get();
			_connSlot.erase(key);
			_controlOnlyConns.erase(key);
			_tdwOnlyConns.erase(key);
			_connRttUs.erase(key);       // Tele-0.3
			_connClientStats.erase(key); // Tele-0.5
			_queue.erase(std::remove_if(_queue.begin(), _queue.end(),
				[key](const QueueEntry& e) { return e.key == key; }), _queue.end());
		} catch (...) {}
		_clientCount--;
	}
	_disconnectsTotal.fetch_add(1, std::memory_order_relaxed);  // Tele-0.11

	// Remove from relay tree (handles orphan reassignment)
	if (key) {
		auto peerIt = _connToPeerId.find(key);
		if (peerIt != _connToPeerId.end()) {
			removeFromTree(peerIt->second);
			_connToPeerId.erase(peerIt);
		}
	}

	printf("[maplecast-ws] client disconnected (%d total, %d seeds, %d relay nodes)\n",
		_clientCount.load(), (int)_seedPeers.size(), (int)_relayTree.size());

	// Notify remaining clients of updated status
	broadcastStatus();
}

// live state migration (implemented at the bottom of this file)
static void migHandleRequest(ConnHdl hdl, const nlohmann::json& ctrl);
static void migHandleIncomingPush(ConnHdl hdl, const std::string& data);
static void migEagerInit();

static void onMessage(ConnHdl hdl, WsServer::message_ptr msg)
{
	if (msg->get_opcode() == websocketpp::frame::opcode::binary)
	{
		// Binary = gamepad input (4 bytes: LT, RT, btnHi, btnLo)
		const auto& data = msg->get_payload();
		// Migration STPU push from another fleet node (docs/STATE-HANDOFF-PLAN.md)
		if (data.size() >= 12 && memcmp(data.data(), "STPU", 4) == 0)
		{
			migHandleIncomingPush(hdl, data);
			return;
		}
		if (data.size() == 4)
		{
			// Init UDP socket if needed
			if (_udpSock < 0)
			{
				_udpSock = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
				memset(&_udpDest, 0, sizeof(_udpDest));
				_udpDest.sin_family = AF_INET;
				_udpDest.sin_port = htons(7100);
				_udpDest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			}

			int slot = getSlotForConn(hdl);
			if (slot >= 0 && slot <= 1)
			{
				// Tagged 5-byte packet: slot + W3 data
				char tagged[5];
				tagged[0] = (char)slot;
				memcpy(tagged + 1, data.c_str(), 4);
				sendto(_udpSock, tagged, 5, 0, (struct sockaddr*)&_udpDest, sizeof(_udpDest));
			}
			else
			{
				// Unassigned player â€” send raw 4-byte
				sendto(_udpSock, data.c_str(), 4, 0, (struct sockaddr*)&_udpDest, sizeof(_udpDest));
			}
		}
		else if (data.size() == 8)
		{
			// E2E probe (2026-07-12): 8-byte browser input = [LT][RT][btnHi][btnLo][seq:u32 LE].
			// Wrap as the EXISTING 11-byte "PC" packet so the input server's seq/arrival-time
			// bookkeeping (and the E2EP latch attribution) works unchanged. ?e2e=1 clients only.
			if (_udpSock < 0)
			{
				_udpSock = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
				memset(&_udpDest, 0, sizeof(_udpDest));
				_udpDest.sin_family = AF_INET;
				_udpDest.sin_port = htons(7100);
				_udpDest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			}
			int slot = getSlotForConn(hdl);
			if (slot >= 0 && slot <= 1)
			{
				char pc[11];
				pc[0] = 'P'; pc[1] = 'C'; pc[2] = (char)slot;
				memcpy(pc + 3, data.c_str() + 4, 4);   // seq u32 LE
				memcpy(pc + 7, data.c_str(), 4);       // W3 [LT][RT][btnHi][btnLo]
				sendto(_udpSock, pc, 11, 0, (struct sockaddr*)&_udpDest, sizeof(_udpDest));
			}
			else
			{
				// Unassigned: mirror the 4-byte path's raw fallback (strip the seq) — WITHOUT
				// this, ?e2e=1 inputs were SILENTLY DROPPED pre-assignment = "inputs not working".
				sendto(_udpSock, data.c_str(), 4, 0, (struct sockaddr*)&_udpDest, sizeof(_udpDest));
			}
		}
	}
	else if (msg->get_opcode() == websocketpp::frame::opcode::text)
	{
		// Text = JSON control
		try {
			auto ctrl = json::parse(msg->get_payload());
			if (ctrl["type"] == "control_only")
			{
				// Browser â†’ flycast direct connection that does NOT want the
				// 4 Mbps TA frame downstream (it gets that from the relay).
				// Add to the skip-binary set so broadcastBinary() leaves it
				// alone. Idempotent.
				try {
					void* key = (void*)_ws.get_con_from_hdl(hdl).get();
					std::lock_guard<std::mutex> lock(_connMutex);
					_controlOnlyConns.insert(key);
				} catch (...) {}
				return;
			}
			// wire-v2 VCACHE: a viewer asks for a fresh SYNC broadcast. Sent on
			// join (the relay serves its CACHED SYNC without telling the server,
			// so the join-driven VCACHE reseed never fires otherwise) and on a
			// VCACHE ref-miss (a content-ref for a hash the viewer never received
			// -- its page stays silently stale until resync). The fresh SYNC also
			// resets the ZCS2 stream epoch and reseeds the VCACHE sent-set (both
			// flags are set at the broadcastFreshSync drain site in serverPublish).
			// Rate-limited: a SYNC is a multi-MB broadcast to every client.
			// Live state migration: a player's client asks THIS server to hand
			// its game to another fleet node (docs/STATE-HANDOFF-PLAN.md v1).
			if (ctrl["type"] == "migrate")
			{
				migHandleRequest(hdl, ctrl);
				return;
			}
			// Per-connection leg subscription: native TDW clients opt out of
			// the legacy broadcast legs (ZCST deltas/ZCS2/GSTA/PALF/...) that
			// browsers still need — they keep TDW1/TDWS + SYNC keyframes.
			if (ctrl["type"] == "subscribe")
			{
				try {
					bool tdw = ctrl.value("mode", "") == "tdw";
					{
						void* key = (void*)_ws.get_con_from_hdl(hdl).get();
						std::lock_guard<std::mutex> lock(_connMutex);
						if (tdw) {
							_tdwOnlyConns.insert(key);
							printf("[maplecast-ws] client subscribed: tdw-only (legacy legs shed)\n");
						} else {
							_tdwOnlyConns.erase(key);
							printf("[maplecast-ws] client subscribed: all legs\n");
						}
						fflush(stdout);
					}
					// TDW joiners need the dictionary NOW — served directly, never
					// via the legacy SYNC broadcast (kill switches / rate limits).
					if (tdw)
						maplecast_mirror::requestTdwSnapshot();
				} catch (...) {}
				return;
			}
			if (ctrl["type"] == "request_sync")
			{
				static std::atomic<int64_t> _lastSyncReqMs{0};
				int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
				int64_t prev = _lastSyncReqMs.load(std::memory_order_relaxed);
				if (now - prev >= 2000 && _lastSyncReqMs.compare_exchange_strong(prev, now)) {
					printf("[maplecast-ws] request_sync from viewer -> fresh SYNC broadcast\n");
					maplecast_mirror::requestSyncBroadcast();
				}
				return;
			}
			// Skin system â€” palette_write, palette_clear, match_info use "cmd"
			// instead of "type". Handle them before the type-based chain so they
			// work even when the message has no "type" field (e.g. from king.html
			// skin picker).
			if (ctrl.contains("cmd") && ctrl["cmd"] == "palette_write")
			{
				printf("[maplecast-ws] palette_write received: index=%d\n", ctrl.value("index", -1));
				int startIdx = ctrl.value("index", 0);
				bool persist = ctrl.value("persist", false);
				if (ctrl.contains("colors") && ctrl["colors"].is_array()) {
					auto& colors = ctrl["colors"];
					int count = (int)colors.size();
					if (startIdx >= 0 && startIdx + count <= 1024) {
						std::vector<u16> colorVec;
						for (auto& c : colors) colorVec.push_back(c.get<int>() & 0xFFFF);
						::maplecast_palette_write(startIdx, colorVec, persist);
					}
				}
				return;
			}
			if (ctrl.contains("cmd") && ctrl["cmd"] == "palette_clear")
			{
				::maplecast_palette_clear();
				return;
			}
			if (ctrl.contains("cmd") && ctrl["cmd"] == "match_info")
			{
				// Read character structs from DC RAM for the skin picker
				static const u32 bases[] = { 0x268340, 0x2688E4, 0x268E88, 0x26942C, 0x2699D0, 0x269F74 };
				static const char* slotNames[] = { "P1C1","P2C1","P1C2","P2C2","P1C3","P2C3" };
				static const char* charNames[] = {
					"Ryu","Zangief","Guile","Morrigan","Anakaris","Strider",
					"Cyclops","Wolverine","Psylocke","Iceman","Rogue","Captain America",
					"Spider-Man","Hulk","Venom","Doctor Doom","Tron Bonne","Jill",
					"Hayato","Ruby Heart","SonSon","Amingo","Marrow","Cable",
					"Abyss1","Abyss2","Abyss3","Chun-Li","Mega Man","Roll",
					"Akuma","BB Hood","Felicia","Charlie","Sakura","Dan",
					"Cammy","Dhalsim","M.Bison","Ken","Gambit","Juggernaut",
					"Storm","Sabretooth","Magneto","Shuma-Gorath","War Machine",
					"Silver Samurai","Omega Red","Spiral","Colossus","Iron Man",
					"Sentinel","Blackheart","Thanos","Jin","Captain Commando",
					"Bone Wolverine","Servbot"
				};
				json chars = json::array();
				for (int i = 0; i < 6; i++) {
					u8 active = ::mem_b[bases[i]];
					u8 charId = ::mem_b[bases[i] + 1];
					u8 palette = ::mem_b[bases[i] + 0x52D];
					u8 health = ::mem_b[bases[i] + 0x420];
					const char* name = (charId < 59) ? charNames[charId] : "Unknown";
					chars.push_back({{"slot",slotNames[i]},{"charId",charId},{"name",name},
						{"active",(bool)active},{"palette",palette},{"health",health}});
				}
				std::string rid = ctrl.value("reply_id", "");
				json resp = {{"ok",true},{"cmd","match_info"},{"reply_id",rid},
					{"data",{{"characters",chars}}}};
				try { _ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text); } catch(...) {}
				return;
			}
			// record_start / record_stop -- runtime trigger from F9 hotkey or
			// any other client. Same logic as the legacy control-WS endpoint
			// (port 7211); routed here so the mirror client can drive it
			// over the same WS connection it already has open at port 7200.
			if (ctrl.contains("cmd") && ctrl["cmd"] == "record_start")
			{
				std::string rid = ctrl.value("reply_id", "");
				std::string path = ctrl.value("path", "");
				if (path.empty()) {
					json resp = {{"ok",false},{"cmd","record_start"},{"reply_id",rid},
						{"error","missing 'path'"}};
					try { _ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text); } catch(...) {}
					return;
				}
				if (maplecast_replay::active()) {
					json resp = {{"ok",false},{"cmd","record_start"},{"reply_id",rid},
						{"error","already recording -- call record_stop first"}};
					try { _ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text); } catch(...) {}
					return;
				}
				printf("[mirror-ws] record_start: %s\n", path.c_str());
				fflush(stdout);
				maplecast_replay::StartParams p;
				p.out_path = path;
				p.p1_name  = ctrl.value("p1_name", "");
				p.p2_name  = ctrl.value("p2_name", "");
				const bool ok = maplecast_replay::startInteractive(p);
				json resp = ok
					? json{{"ok",true},{"cmd","record_start"},{"reply_id",rid},
						{"data",{{"path",path},{"state","recording"}}}}
					: json{{"ok",false},{"cmd","record_start"},{"reply_id",rid},
						{"error","writer::start rejected -- see flycast log"}};
				try { _ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text); } catch(...) {}
				return;
			}
			// Tele-0.5: client_stats reporter -- native client pushes
			// its own render-time stats every 1s so we can surface E2E
			// in the status broadcast.
			if (ctrl.contains("type") && ctrl["type"] == "client_stats")
			{
				try {
					void* key = (void*)_ws.get_con_from_hdl(hdl).get();
					ClientStats cs{};
					cs.render_us_avg  = ctrl.value("render_us_avg",  0);
					cs.render_us_max  = ctrl.value("render_us_max",  0);
					cs.arrival_ema_us = ctrl.value("arrival_ema_us", 0);
					cs.packets        = (uint64_t)ctrl.value("packets", 0);
					std::lock_guard<std::mutex> lock(_connMutex);
					_connClientStats[key] = cs;
				} catch (...) {}
				return;
			}

			if (ctrl.contains("cmd") && ctrl["cmd"] == "record_stop")
			{
				std::string rid = ctrl.value("reply_id", "");
				if (!maplecast_replay::active()) {
					json resp = {{"ok",false},{"cmd","record_stop"},{"reply_id",rid},
						{"error","not recording"}};
					try { _ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text); } catch(...) {}
					return;
				}
				const uint64_t entries = maplecast_replay::entryCount();
				maplecast_replay::stop(0xFF);
				printf("[mirror-ws] record_stop: %llu entries\n",
				       (unsigned long long)entries);
				fflush(stdout);
				json resp = {{"ok",true},{"cmd","record_stop"},{"reply_id",rid},
					{"data",{{"entries",entries}}}};
				try { _ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text); } catch(...) {}
				return;
			}
			if (ctrl["type"] == "join")
			{
				std::string playerId = ctrl.value("id", "");
				std::string name = ctrl.value("name", "Player");
				std::string device = ctrl.value("device", "Browser");

				// Ghost-slot eviction: if a slot is currently held by a player
				// with the same display name (case-insensitive), this is almost
				// certainly the same human reconnecting from a new tab. Free
				// the old slot first so they get their seat back instead of
				// being double-booked into the *other* slot. Skip when name is
				// the generic fallback.
				//
				// We notify the evicted hdl with a `kicked` JSON message so
				// the old tab's UI cleans up (closes its controlWs, stops
				// gamepad polling, surfaces a "taken over by another tab"
				// message). Without this, the old tab sits in limbo: WS open,
				// mySlot still set, polling still firing, but flycast drops
				// every input frame because the slot mapping is gone.
				if (!name.empty() && name != "Player") {
					for (int i = 0; i < 2; i++) {
						const auto& p = maplecast_input::getPlayer(i);
						if (p.connected && strcasecmp(p.name, name.c_str()) == 0) {
							printf("[maplecast-ws] Ghost-slot eviction: P%d (%s) freed for reconnect\n",
								i + 1, p.name);
							maplecast_input::disconnectPlayer(i);
							// Drop the stale connâ†’slot mapping AND notify the
							// evicted hdl so its browser cleans up.
							for (auto it = _connSlot.begin(); it != _connSlot.end(); ) {
								if (it->second == i) {
									try {
										json kicked = {{"type", "kicked"}, {"reason", "ghost"}};
										auto evictedHdlPtr = it->first;
										// Walk _connections to find the matching hdl by raw pointer.
										for (auto& chdl : _connections) {
											try {
												void* ckey = (void*)_ws.get_con_from_hdl(chdl).get();
												if (ckey == evictedHdlPtr) {
													_ws.send(chdl, kicked.dump(), websocketpp::frame::opcode::text);
													break;
												}
											} catch (...) {}
										}
									} catch (...) {}
									it = _connSlot.erase(it);
								} else {
									++it;
								}
							}
							break;
						}
					}
				}

				int slot = maplecast_input::registerPlayer(
					playerId.c_str(), name.c_str(), device.c_str(),
					maplecast_input::InputType::BrowserWS);

				// Register connection â†’ slot mapping
				if (slot >= 0) {
					try {
						void* key = (void*)_ws.get_con_from_hdl(hdl).get();
						_connSlot[key] = slot;

						// Mark as player in relay tree (gets direct stream, never relays)
						auto peerIt = _connToPeerId.find(key);
						if (peerIt != _connToPeerId.end()) {
							auto nodeIt = _relayTree.find(peerIt->second);
							if (nodeIt != _relayTree.end())
								nodeIt->second.isPlayer = true;
						}
					} catch (...) {}

					// Per-user latch policy: if the join handshake carries a
					// latch_policy preference, push it to the slot the player
					// just got assigned. This is what makes the policy follow
					// the PLAYER across slot reassignments â€” the preference
					// lives in the browser's localStorage and is transmitted
					// on every (re)join, so a returning player gets their
					// chosen mode regardless of which slot opens up.
					//
					// When absent, the slot keeps whatever policy it had
					// (which on a fresh boot is LatencyFirst, the default).
					std::string latchPref = ctrl.value("latch_policy", "");
					if (latchPref == "latency") {
						maplecast_input::setLatchPolicy(slot, maplecast_input::LatchPolicy::LatencyFirst);
					} else if (latchPref == "consistency") {
						maplecast_input::setLatchPolicy(slot, maplecast_input::LatchPolicy::ConsistencyFirst);
					}
					// Anything else (empty/unknown) leaves the slot at its
					// current policy. Future stick-memory work can layer in
					// a stick-binding lookup here as a second source.
				}

				json resp = {{"type", "assigned"}, {"slot", slot}, {"id", playerId.substr(0,8)}, {"name", name}};
				_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);

				// Patch in-game player name
				if (slot >= 0)
					maplecast_gamestate::setPlayerName(slot, name.c_str());

				// Broadcast updated status to all
				broadcastStatus();
			}
			else if (ctrl["type"] == "leave")
			{
				int slot = getSlotForConn(hdl);
				if (slot >= 0) {
					maplecast_input::disconnectPlayer(slot);
					maplecast_gamestate::restorePlayerNames();
					try {
						void* key = (void*)_ws.get_con_from_hdl(hdl).get();
						_connSlot.erase(key);
					} catch (...) {}
				}

				json resp = {{"type", "assigned"}, {"slot", -1}};
				_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);

				broadcastStatus();
			}
			else if (ctrl["type"] == "queue_join")
			{
				std::string name = ctrl.value("name", "Anon");
				try {
					void* key = (void*)_ws.get_con_from_hdl(hdl).get();
					// Don't double-add
					bool found = false;
					for (const auto& q : _queue) { if (q.key == key) { found = true; break; } }
					if (!found)
						_queue.push_back({key, name, hdl});
				} catch (...) {}
				broadcastStatus();
			}
			else if (ctrl["type"] == "register_stick")
			{
				std::string browserId = ctrl.value("id", "");
				if (!browserId.empty()) {
					maplecast_input::startStickRegistration(browserId.c_str());
					json resp;
					resp["type"] = "register_started";
					resp["msg"] = "Tap any button 5 times, pause, then 5 times again";
					_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
				}
			}
			else if (ctrl["type"] == "unregister_stick")
			{
				std::string browserId = ctrl.value("id", "");
				if (!browserId.empty()) {
					maplecast_input::unregisterStick(browserId.c_str());
					json resp;
					resp["type"] = "stick_unregistered";
					_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
					broadcastStatus();
				}
			}
			else if (ctrl["type"] == "register_stick_web")
			{
				// Web-based registration: username + "press any button"
				std::string username = ctrl.value("username", "");
				if (!maplecast_input::isValidUsername(username.c_str())) {
					json resp = {{"type", "register_error"}, {"msg", "Invalid username. 4-12 chars, letters/numbers/underscore only."}};
					_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
				} else {
					// Check if username is already registered
					auto info = maplecast_input::getStickInfo(username.c_str());
					if (info.registered) {
						json resp = {{"type", "register_error"}, {"msg", "Username already registered. Unregister first."}};
						_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
					} else {
						maplecast_input::startWebRegistration(username.c_str());
						json resp = {{"type", "register_waiting"}, {"username", username}, {"msg", "Press any button on your stick..."}};
						_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
					}
				}
			}
			else if (ctrl["type"] == "cancel_register_web")
			{
				maplecast_input::cancelWebRegistration();
				json resp = {{"type", "register_cancelled"}};
				_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
			}
			else if (ctrl["type"] == "set_latch_policy")
			{
				// Phase B + per-user gate â€” live per-slot latch policy switch.
				// Players choose between LatencyFirst (today's behavior) and
				// ConsistencyFirst (accumulator + edge preservation + guard
				// window). The policy follows the PLAYER, not the chair â€”
				// it's stored client-side in localStorage and re-pushed via
				// the join handshake whenever they (re)take a slot.
				//
				// Server-side gate: a connection can only set the policy for
				// the slot IT owns. Spectators and the other player are
				// rejected. This is the load-bearing security check; the UI
				// only HIDES the other slot's button, but server enforces.
				int slot = ctrl.value("slot", -1);
				std::string policyStr = ctrl.value("policy", "");

				// Identity check â€” what slot does THIS connection actually own?
				int ownerSlot = getSlotForConn(hdl);
				if (slot != ownerSlot || ownerSlot < 0) {
					json resp = {{"type", "set_latch_policy_error"},
					             {"msg", "you can only change your own slot's latch policy"}};
					_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
				}
				else if (slot < 0 || slot > 1) {
					json resp = {{"type", "set_latch_policy_error"},
					             {"msg", "slot must be 0 or 1"}};
					_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
				} else if (policyStr == "latency") {
					maplecast_input::setLatchPolicy(slot, maplecast_input::LatchPolicy::LatencyFirst);
					json resp = {{"type", "latch_policy_changed"},
					             {"slot", slot}, {"policy", "latency"}};
					_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
					broadcastStatus();
				} else if (policyStr == "consistency") {
					maplecast_input::setLatchPolicy(slot, maplecast_input::LatchPolicy::ConsistencyFirst);
					json resp = {{"type", "latch_policy_changed"},
					             {"slot", slot}, {"policy", "consistency"}};
					_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
					broadcastStatus();
				} else {
					json resp = {{"type", "set_latch_policy_error"},
					             {"msg", "policy must be 'latency' or 'consistency'"}};
					_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
				}
			}
			else if (ctrl["type"] == "check_stick")
			{
				// Reports: do you own a stick, is it online, and are you
				// already in a slot. The slot field is the missing piece
				// that lets a reloaded browser tab resync to its existing
				// player without clicking I GOT NEXT (which would otherwise
				// double-book the user into the other slot).
				std::string username = ctrl.value("username", "");
				auto info = maplecast_input::getStickInfo(username.c_str());
				int currentSlot = -1;
				for (int i = 0; i < 2; i++) {
					const auto& p = maplecast_input::getPlayer(i);
					if (p.connected && strcasecmp(p.name, username.c_str()) == 0) {
						currentSlot = i;
						break;
					}
				}
				json resp = {
					{"type", "stick_status"},
					{"username", username},
					{"registered", info.registered},
					{"online", info.online},
					{"slot", currentSlot}
				};
				_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
			}
			else if (ctrl["type"] == "stick_load")
			{
				// Collector boot-time push: authoritative DB state replaces
				// whatever we have in RAM (well â€” installs, doesn't wipe;
				// see installStickBindings). Trusted source, so no auth check
				// here yet â€” relies on collector being on the same WS:7200
				// link that already requires being on-host or going through
				// our nginx auth.
				if (ctrl.contains("bindings") && ctrl["bindings"].is_array()) {
					std::vector<maplecast_input::StickSnapshot> snaps;
					for (const auto& b : ctrl["bindings"]) {
						maplecast_input::StickSnapshot s = {};
						std::string user = b.value("username", "");
						std::string ip   = b.value("ip", "");
						int port         = b.value("port", 0);
						strncpy(s.username, user.c_str(), sizeof(s.username) - 1);
						s.srcIP = inet_addr(ip.c_str());
						s.srcPort = htons((uint16_t)port);
						s.lastInputUs = 0;
						if (s.username[0] && s.srcIP)
							snaps.push_back(s);
					}
					maplecast_input::installStickBindings(snaps);
					maplecast_input::saveStickCache();
					printf("[maplecast-ws] stick_load applied: %zu binding(s)\n", snaps.size());
				}
			}
			else if (ctrl["type"] == "patch_name")
			{
				int slot = ctrl.value("slot", 0);
				std::string name = ctrl.value("name", "");
				printf("[maplecast-ws] PATCH_NAME received: slot=%d name='%s'\n", slot, name.c_str());
				if (!name.empty()) {
					maplecast_gamestate::setPlayerName(slot, name.c_str());
					// Verify the write took effect
					uint8_t check = addrspace::read8(0x8CBBC31E);
					printf("[maplecast-ws] Verify: 0x8CBBC31E = 0x%02X ('%c')\n", check, check >= 32 ? check : '.');
				}
			}
			// ==================== P2P Relay Messages ====================
			else if (ctrl["type"] == "relay_ready")
			{
				bool canRelay = ctrl.value("canRelay", true);
				try {
					void* key = (void*)_ws.get_con_from_hdl(hdl).get();
					std::string peerId = generatePeerId();

					RelayNode node;
					node.peerId = peerId;
					node.conn = hdl;
					node.connKey = key;
					node.canRelay = canRelay;
					node.connectedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::high_resolution_clock::now().time_since_epoch()).count();

					_relayTree[peerId] = node;
					_connToPeerId[key] = peerId;

					// Assign role: seed if we need more, otherwise find a parent
					if ((int)_seedPeers.size() < MAX_SEEDS && canRelay) {
						makeSeed(peerId);
					} else {
						std::string parentId = findBestParent();
						if (!parentId.empty()) {
							assignChild(peerId, parentId);
						} else {
							// No room in tree â€” make another seed
							makeSeed(peerId);
						}
					}
				} catch (...) {}
			}
			else if (ctrl["type"] == "relay_signal")
			{
				// Forward WebRTC signaling between peers
				std::string toPeerId = ctrl.value("toPeerId", "");
				auto toIt = _relayTree.find(toPeerId);
				if (toIt != _relayTree.end()) {
					// Find sender's peerId
					std::string fromPeerId;
					try {
						void* key = (void*)_ws.get_con_from_hdl(hdl).get();
						auto fromIt = _connToPeerId.find(key);
						if (fromIt != _connToPeerId.end()) fromPeerId = fromIt->second;
					} catch (...) {}

					if (!fromPeerId.empty()) {
						json fwd;
						fwd["type"] = "relay_signal";
						fwd["fromPeerId"] = fromPeerId;
						fwd["signal"] = ctrl["signal"];
						sendJson(toIt->second.conn, fwd);
					}
				}
			}
			else if (ctrl["type"] == "relay_parent_lost")
			{
				// Child reports parent DataChannel died â€” reassign
				try {
					void* key = (void*)_ws.get_con_from_hdl(hdl).get();
					auto peerIt = _connToPeerId.find(key);
					if (peerIt != _connToPeerId.end()) {
						std::string peerId = peerIt->second;
						auto nodeIt = _relayTree.find(peerId);
						if (nodeIt != _relayTree.end()) {
							// Clear parent
							nodeIt->second.parentId.clear();
							// Find new parent
							if ((int)_seedPeers.size() < MAX_SEEDS && nodeIt->second.canRelay) {
								makeSeed(peerId);
							} else {
								std::string newParent = findBestParent();
								if (!newParent.empty())
									assignChild(peerId, newParent);
								else
									makeSeed(peerId);
							}
						}
					}
				} catch (...) {}
			}
			else if (ctrl["type"] == "relay_stats")
			{
				// Health report from relay node â€” log for now
			}
			else if (ctrl["type"] == "cancel_register")
			{
				maplecast_input::cancelStickRegistration();
			}
			else if (ctrl["type"] == "ping")
			{
				// Echo back with server timestamp for RTT measurement
				json resp;
				resp["type"] = "pong";
				resp["t"] = ctrl.value("t", 0.0);
				_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
			}
			else if (ctrl["type"] == "queue_leave")
			{
				try {
					void* key = (void*)_ws.get_con_from_hdl(hdl).get();
					_queue.erase(std::remove_if(_queue.begin(), _queue.end(),
						[key](const QueueEntry& e) { return e.key == key; }), _queue.end());
				} catch (...) {}
				broadcastStatus();
			}
			// (palette_write, palette_clear, match_info handled above the type chain)
		} catch (...) {}
	}
}

bool init(int port)
{
	try {
		_ws.clear_access_channels(websocketpp::log::alevel::all);
		_ws.clear_error_channels(websocketpp::log::elevel::all);
		_ws.init_asio();
		_ws.set_reuse_addr(true);

		// CRITICAL: disable Nagle's algorithm on every accepted socket.
		// Without this, TCP buffers small writes (status JSON, ping echoes,
		// 4-byte input forwards) for up to 40ms hoping to coalesce them with
		// the next write. With it, every send hits the wire immediately.
		// The relayâ†’home and browserâ†’relay paths benefit by 0-40ms p99.
		_ws.set_socket_init_handler([](websocketpp::connection_hdl,
		                               websocketpp::lib::asio::ip::tcp::socket& s) {
			websocketpp::lib::asio::error_code ec;
			s.set_option(websocketpp::lib::asio::ip::tcp::no_delay(true), ec);
		});

		_ws.set_open_handler(&onOpen);
		_ws.set_close_handler(&onClose);
		_ws.set_message_handler(&onMessage);
		migEagerInit();   // migration arenas at boot, not on the hot path (edge OOM fix)
		// Tele-0.3: PONG handler. We periodically ping each client with
		// a microsecond timestamp as the payload; on pong arrival we
		// decode the timestamp, compute RTT = now - ts_in_ping, and
		// stash it in _connRttUs[conn-key] for the next status broadcast.
		_ws.set_pong_handler([](ConnHdl hdl, std::string payload) {
			try {
				const int64_t nowUs = (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
				const int64_t pingUs = std::stoll(payload);
				const int64_t rttUs  = nowUs - pingUs;
				if (rttUs > 0 && rttUs < 5'000'000) {
					void* key = (void*)_ws.get_con_from_hdl(hdl).get();
					std::lock_guard<std::mutex> lock(_connMutex);
					_connRttUs[key] = (int32_t)rttUs;
				}
			} catch (...) {}
		});

		_ws.listen(port);
		_ws.start_accept();

		// TODO: multi-threaded io_context. Several global structures
		// (_queue, _relayTree, _seedPeers) are read without _connMutex in
		// status broadcast and relay-tree helpers. Migrating to multi-threaded
		// asio requires auditing every access path first. For now, single
		// thread + the relay echoing pings locally keeps the hot path clean.
		_wsThread = std::thread([&]() { _ws.run(); });

		// Periodic status broadcast (every 1 second)
		_active = true;
		_statusThread = std::thread([]() {
			while (_active) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				if (_active && _clientCount.load() > 0) {
					checkMatchEnd();
					checkIdleKick();
					// pingAllForRtt() disabled -- triggered black-screen on
					// the native client (binary-frame read loop fell over
					// somehow when WS PING control frames started flowing).
					// Server-side PING infrastructure still compiles in case
					// we want to re-enable for browser-only RTT later via a
					// gate, but for now we leave RTT measurement to clients.
					broadcastStatus();
				}
			}
		});

		printf("[maplecast-ws] WebSocket server on ws://0.0.0.0:%d (mirror + lobby)\n", port);
		return true;
	} catch (const std::exception& e) {
		printf("[maplecast-ws] Init failed: %s\n", e.what());
		return false;
	}
}

void shutdown()
{
	_active = false;
	try { _ws.stop(); } catch (...) {}
	if (_wsThread.joinable()) _wsThread.join();
	if (_statusThread.joinable()) _statusThread.join();
	if (_udpSock >= 0) {
#ifdef _WIN32
		closesocket(_udpSock);
#else
		mc_closesocket(_udpSock);
#endif
		_udpSock = -1;
	}
}

bool active()
{
	return _active;
}

void broadcastBinary(const void* data, size_t size)
{
	if (!_active || _clientCount.load(std::memory_order_relaxed) == 0) return;

	// Post the entire broadcast (snapshot + send loop) onto asio's
	// io_service thread (_wsThread). Caller returns immediately after
	// the post() â€” no locks held, no socket work, no contention with
	// any other thread.
	//
	// Why this matters: the TA mirror publish runs on Flycast-rend, the
	// audio sender runs on its own thread, and both used to call
	// _ws.send() inline. asio's per-connection write queue is
	// thread-safe but not lock-free, and having two threads submitting
	// dense traffic (60 video/sec + 86 audio/sec) caused measurable
	// stalls that showed up as video lag correlated with audio being
	// enabled. Centralizing all WS writes on _wsThread removes the
	// cross-thread contention entirely; audio and video never block
	// each other because neither ever touches the socket directly.
	//
	// The data is copied once into a std::string (asio's native buffer
	// type for websocketpp send) and moved into the lambda. On _wsThread,
	// the send loop runs to completion without any other thread waiting
	// on it.
	// Classify once: does a TDW-subscribed client want this message? TDW1/TDWS
	// always; a ZCST whose uncompressed size exceeds 1MB is a SYNC keyframe
	// (the render-worker heuristic) — TDW clients still seed/heal from those.
	// Everything else (ZCST deltas, ZCS2, GSTA/PALF/OBJS/EFCT/TXTR) is legacy.
	bool tdwWanted = false;
	if (size >= 4) {
		const uint8_t* b = reinterpret_cast<const uint8_t*>(data);
		if (memcmp(b, "TDW1", 4) == 0 || memcmp(b, "TDWS", 4) == 0)
			tdwWanted = true;
		else if (memcmp(b, "ZCST", 4) == 0 && size >= 8) {
			uint32_t usize;
			memcpy(&usize, b + 4, 4);
			tdwWanted = usize > 1024 * 1024;   // SYNC keyframe
		}
	}
	std::string payload(reinterpret_cast<const char*>(data), size);
	_ws.get_io_service().post([payload = std::move(payload), tdwWanted]() mutable {
		std::vector<ConnHdl> targets;
		{
			std::lock_guard<std::mutex> lock(_connMutex);
			targets.reserve(_connections.size());
			std::set<void*> relaySkip;
			for (auto& [key, peerId] : _connToPeerId) {
				auto it = _relayTree.find(peerId);
				if (it != _relayTree.end() && !it->second.isSeed && !it->second.isPlayer)
					relaySkip.insert(key);
			}
			for (auto& conn : _connections) {
				try {
					void* key = (void*)_ws.get_con_from_hdl(conn).get();
					if (relaySkip.count(key)) continue;
					if (_controlOnlyConns.count(key)) continue;
					if (!tdwWanted && _tdwOnlyConns.count(key)) continue;
					targets.push_back(conn);
				} catch (...) {}
			}
		}
		for (auto& conn : targets) {
			try {
				_ws.send(conn, payload.data(), payload.size(),
					websocketpp::frame::opcode::binary);
			} catch (...) {}
		}
	});
}

void broadcastFreshSync()
{
	if (!_active || _clientCount.load(std::memory_order_relaxed) == 0) return;

	// Build SYNC packet from current VRAM + PVR regs
	size_t syncSize = 4 + 4 + VRAM_SIZE + 4 + pvr_RegSize;
	std::vector<uint8_t> syncBuf(syncSize);
	uint8_t* dst = syncBuf.data();
	memcpy(dst, "SYNC", 4); dst += 4;
	uint32_t vs = VRAM_SIZE;
	memcpy(dst, &vs, 4); dst += 4;
	memcpy(dst, &vram[0], VRAM_SIZE); dst += VRAM_SIZE;
	uint32_t ps = pvr_RegSize;
	memcpy(dst, &ps, 4); dst += 4;
	memcpy(dst, pvr_regs, pvr_RegSize);

	// Compress with zstd (ZCST magic) â€” same path as onOpen
	MirrorCompressor syncComp;
	syncComp.init(syncSize + 128);
	size_t compSyncSize = 0;
	uint64_t compUs = 0;
	const uint8_t* compSync = syncComp.compress(syncBuf.data(), (uint32_t)syncSize, compSyncSize, compUs, 3);

	// Broadcast to ALL clients â€” even relay children get this directly. The
	// relay tree's delta-only path is fine for normal frames but a scene
	// transition needs to invalidate everyone's state at once.
	// EXCEPT control-only connections â€” they get SYNC via the VPS relay path,
	// not over the home upstream.
	{
		std::lock_guard<std::mutex> lock(_connMutex);
		for (auto& conn : _connections) {
			try {
				void* key = (void*)_ws.get_con_from_hdl(conn).get();
				if (_controlOnlyConns.count(key)) continue;
				_ws.send(conn, compSync, compSyncSize, websocketpp::frame::opcode::binary);
			} catch (...) {}
		}
	}
	syncComp.destroy();

	// Tele-0.8: SCENE-CHANGE SYNC counters.
	_sceneChangeSyncCount.fetch_add(1, std::memory_order_relaxed);
	_sceneChangeSyncBytes.fetch_add((uint64_t)compSyncSize * (uint64_t)_clientCount.load(),
	                                 std::memory_order_relaxed);

	printf("[maplecast-ws] SCENE-CHANGE SYNC: %.1f MB -> %.1f MB (%.1fx) in %lums to %d clients\n",
		syncSize / (1024.0 * 1024.0), compSyncSize / (1024.0 * 1024.0),
		(double)syncSize / compSyncSize, compUs / 1000, _clientCount.load());
}

// Tele-0.2: time-windowed ring of (ts_us, publish_us, compress_us)
// triples so the status broadcast can surface "worst-case publish over
// the last 1s / 30s" -- the most direct signal of "is the server
// keeping up with frame budget?". 1024 entries @ 60Hz = ~17s of
// history; combined with the 30s window the ring naturally caps the
// upper-bound query.
struct FrameWorkSample {
	int64_t  ts_us;
	uint32_t publish_us;
	uint32_t compress_us;
	uint32_t dirty_pages;   // Tele-0.7
};

// Tele-0.8 storage
std::atomic<uint64_t> _sceneChangeSyncCount{0};
std::atomic<uint64_t> _sceneChangeSyncBytes{0};

// Tele-0.11 storage
std::atomic<uint64_t> _connectsTotal{0};
std::atomic<uint64_t> _disconnectsTotal{0};
static constexpr int FRAMEWORK_RING_SIZE = 2048;       // ~34s @ 60Hz
static FrameWorkSample          _frameWork[FRAMEWORK_RING_SIZE];
static std::atomic<uint32_t>    _frameWorkWriteIdx{0};

static void recordFrameWork(uint32_t publishUs, uint32_t compressUs, uint32_t dirtyPages)
{
	const int64_t nowUs = (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	uint32_t idx = _frameWorkWriteIdx.fetch_add(1, std::memory_order_relaxed);
	const uint32_t pos = idx % FRAMEWORK_RING_SIZE;
	_frameWork[pos] = { nowUs, publishUs, compressUs, dirtyPages };
}

// Tele-0.6: server operational stats (uptime, CPU%, RSS, disk%).
// Linux only -- the mirror-client build compiles this file but never
// calls getServerOpStats (broadcastStatus only runs on the headless
// server). On Windows we return zeros.
static std::chrono::steady_clock::time_point _opStartTime = std::chrono::steady_clock::now();

static ServerOpStats getServerOpStats(const std::string& recordings_dir)
{
	ServerOpStats out{};
	out.uptime_s = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::steady_clock::now() - _opStartTime).count();

#ifdef __linux__
	// CPU% from /proc/self/stat utime+stime jiffies, diffed against
	// the previous sample. *100 for percent-with-2-decimal-places
	// resolution (e.g., 1234 = 12.34%).
	static int64_t lastJiffies = -1;
	static auto lastSample = std::chrono::steady_clock::now();
	int64_t curJiffies = 0;
	if (FILE* f = fopen("/proc/self/stat", "r")) {
		char buf[2048];
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[n] = 0;
		// Skip past comm: it's wrapped in parens and may itself contain
		// parens or spaces, so find the LAST ')'.
		const char* p = strrchr(buf, ')');
		if (p) {
			p++;
			int field = 2;  // just passed comm (field 2)
			while (*p && field < 14) {
				if (*p == ' ') field++;
				p++;
			}
			char* endp = nullptr;
			long long u = strtoll(p, &endp, 10);
			long long s = strtoll(endp, nullptr, 10);
			curJiffies = u + s;
		}
	}
	auto now = std::chrono::steady_clock::now();
	if (lastJiffies >= 0) {
		const long ticksPerSec = sysconf(_SC_CLK_TCK);
		if (ticksPerSec > 0) {
			const int64_t deltaJiffies = curJiffies - lastJiffies;
			const int64_t deltaUs = std::chrono::duration_cast<std::chrono::microseconds>(
				now - lastSample).count();
			if (deltaUs > 0) {
				const double pct = (double)deltaJiffies * 1e6 * 100.0 /
				                   ((double)ticksPerSec * (double)deltaUs);
				out.cpu_pct_x100 = (int32_t)(pct * 100.0);
			}
		}
	}
	lastJiffies = curJiffies;
	lastSample  = now;

	// RSS in MB from /proc/self/status VmRSS (kB).
	if (FILE* f = fopen("/proc/self/status", "r")) {
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			if (strncmp(line, "VmRSS:", 6) == 0) {
				long kB = 0;
				if (sscanf(line + 6, "%ld", &kB) == 1)
					out.rss_mb = kB / 1024;
				break;
			}
		}
		fclose(f);
	}

	// Disk usage % on the recordings dir filesystem.
	if (!recordings_dir.empty()) {
		struct statvfs vfs;
		if (statvfs(recordings_dir.c_str(), &vfs) == 0) {
			const uint64_t total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
			const uint64_t free_  = (uint64_t)vfs.f_bavail * vfs.f_frsize;
			if (total > 0)
				out.disk_used_pct = (int32_t)(100 - (free_ * 100 / total));
		}
	}
#else
	(void)recordings_dir;
#endif
	return out;
}

static FrameWorkWindow getFrameWorkWindow(int64_t windowUs)
{
	FrameWorkWindow out{};
	const int64_t nowUs = (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	const int64_t cutoff = nowUs - windowUs;

	uint32_t writeIdx = _frameWorkWriteIdx.load(std::memory_order_relaxed);
	uint32_t validCount = (writeIdx < FRAMEWORK_RING_SIZE) ? writeIdx : FRAMEWORK_RING_SIZE;
	if (validCount == 0) return out;

	uint32_t pubSnap[FRAMEWORK_RING_SIZE];
	uint32_t dirtySnap[FRAMEWORK_RING_SIZE];
	uint64_t pubSum = 0, compSum = 0, dirtySum = 0;
	uint32_t compMax = 0;
	uint32_t n = 0;
	for (uint32_t i = 0; i < validCount; ++i) {
		if (_frameWork[i].ts_us < cutoff) continue;
		pubSnap[n]     = _frameWork[i].publish_us;
		dirtySnap[n]   = _frameWork[i].dirty_pages;
		n++;
		pubSum   += _frameWork[i].publish_us;
		compSum  += _frameWork[i].compress_us;
		dirtySum += _frameWork[i].dirty_pages;
		if (_frameWork[i].compress_us > compMax) compMax = _frameWork[i].compress_us;
	}
	if (n == 0) return out;
	std::sort(pubSnap, pubSnap + n);
	out.n               = n;
	out.publish_avg_us  = (uint32_t)(pubSum / n);
	out.publish_max_us  = pubSnap[n - 1];
	uint32_t p99Idx = (n * 99) / 100; if (p99Idx >= n) p99Idx = n - 1;
	out.publish_p99_us  = pubSnap[p99Idx];
	out.compress_avg_us = (uint32_t)(compSum / n);
	out.compress_max_us = compMax;
	// Tele-0.7 dirty stats
	std::sort(dirtySnap, dirtySnap + n);
	out.dirty_avg = (uint32_t)(dirtySum / n);
	out.dirty_p50 = dirtySnap[n / 2];
	out.dirty_p99 = dirtySnap[p99Idx];
	out.dirty_max = dirtySnap[n - 1];
	return out;
}

void updateTelemetry(const Telemetry& t)
{
	std::lock_guard<std::mutex> lock(_telemetryMutex);
	_telemetry = t;
	recordFrameWork((uint32_t)t.publishUs, (uint32_t)t.compressUs, (uint32_t)t.dirtyPages);
}

// Tele-0.9: match-end event broadcast. Triggered from
// maplecast_mirror's in_match 1->0 hook. Sends a self-contained JSON
// summary to all WS clients. Foundation for the match-data-platform
// vision -- once we have NATS, the same payload publishes there.
void broadcastMatchEnd(int64_t start_us, int64_t end_us,
                       const maplecast_gamestate::GameState& start_gs,
                       const maplecast_gamestate::GameState& end_gs)
{
	const int64_t duration_us = (end_us > start_us) ? (end_us - start_us) : 0;

	auto charsJson = [](const maplecast_gamestate::GameState& gs, int base) -> json {
		json arr = json::array();
		for (int i = 0; i < 3; i++) {
			arr.push_back({
				{"char_id",    gs.chars[base + i].character_id},
				{"hp",         gs.chars[base + i].health},
				{"red_hp",     gs.chars[base + i].red_health},
				{"palette",    gs.chars[base + i].palette_id},
				{"active",     (bool)gs.chars[base + i].active},
			});
		}
		return arr;
	};

	// Winner inferred from total HP across all 3 characters per side.
	auto totalHp = [](const maplecast_gamestate::GameState& gs, int base) -> int {
		return (int)gs.chars[base].health + (int)gs.chars[base + 1].health
		     + (int)gs.chars[base + 2].health;
	};
	const int p1Final = totalHp(end_gs, 0);
	const int p2Final = totalHp(end_gs, 3);
	const char* winner = (p1Final == p2Final) ? "draw"
	                   : (p1Final >  p2Final) ? "p1" : "p2";

	json msg = {
		{"type",        "match_end"},
		{"start_us",    start_us},
		{"end_us",      end_us},
		{"duration_us", duration_us},
		{"stage",       end_gs.stage_id},
		{"timer",       end_gs.game_timer},
		{"winner",      winner},
		{"p1_final_hp", p1Final},
		{"p2_final_hp", p2Final},
		{"p1_chars",    charsJson(start_gs, 0)},
		{"p2_chars",    charsJson(start_gs, 3)},
		{"p1_combo_max", end_gs.p1_combo},   // current combo at end -- approximation
		{"p2_combo_max", end_gs.p2_combo},
		{"frame_counter_end", end_gs.frame_counter},
	};
	const std::string payload = msg.dump();

	std::vector<ConnHdl> snapshot;
	{
		std::lock_guard<std::mutex> lock(_connMutex);
		snapshot.assign(_connections.begin(), _connections.end());
	}
	for (auto& hdl : snapshot) {
		try { _ws.send(hdl, payload, websocketpp::frame::opcode::text); } catch (...) {}
	}

	printf("[match-end] stage=%d duration=%lldus winner=%s p1_hp=%d p2_hp=%d (broadcast to %zu clients)\n",
	       end_gs.stage_id, (long long)duration_us, winner, p1Final, p2Final, snapshot.size());
	fflush(stdout);
}

Telemetry getLastTelemetry()
{
	std::lock_guard<std::mutex> lock(_telemetryMutex);
	return _telemetry;
}

int clientCount()
{
	return _clientCount.load(std::memory_order_relaxed);
}

// Stub: declared in the header, called by maplecast_mirror.cpp's
// doForcedSaveStateBroadcast(). The full implementation existed as
// uncommitted working-tree code earlier in the session and got reverted
// during a cleanup pass. doForcedSaveStateBroadcast() only fires on
// SIGUSR1 / explicit reset, so this no-op is harmless under normal
// operation. Restore the real impl if you need SAVE blob broadcasts.
void broadcastSaveStateBytes(const void* /*data*/, size_t /*size*/)
{
	printf("[maplecast-ws] broadcastSaveStateBytes â€” STUB (not implemented)\n");
}

// ==================== live state MIGRATION (server transfer) ====================
//
// docs/STATE-HANDOFF-PLAN.md v1. A player's client asks THIS server to hand its
// game to another fleet node; the game continues there byte-identically
// (dc_serialize round-trip is byte-perfect, fleet binaries are md5-identical).
//
//   client --text--> {"type":"migrate","dest":"host[:port]","key":K}   (:7200)
//   emu thread: drainMigration() -> buildFullSaveState() (frame boundary,
//               SR.BL=0 -- same rule as drainMcsvCapture)
//   worker thread: zstd (MirrorCompressor) -> raw WS client push to dest :7200
//               "STPU"(4) keyLen(u32) key rawSize(u32) zcstBlob
//   dest onMessage: validate MAPLECAST_FLEET_KEY -> stash -> text stpu_ack
//   dest emu thread: drainMigration() applies via the PROVEN state-sync JOIN
//               recipe (rend_start_rollback -> emu.loadstate -> rend_resync ->
//               vblank re-arm) then requestSyncBroadcast() so mirror shadows
//               realign and the forced TDWS snapshot fires for TDW clients.
//   worker: on ack -> requester gets {"type":"migrated","dest":...} and its
//               client switches wire+input to dest.
//
// NEVER route this through Emulator::loadstate from the WS/render thread --
// that's the exact maplecast_control_ws.cpp:314 crash. Both the capture and
// the apply run ONLY inside drainMigration() on the emu thread.
//
// Gate: MAPLECAST_FLEET_KEY must be set AND match on both ends; otherwise
// migrate requests and STPU pushes are rejected outright.

static std::mutex _migMu;
static std::string _migDest;           // pending outbound: dest host[:port]
static ConnHdl    _migRequester;       // who asked (gets migrated/migrate_failed)
static bool       _migSendPending = false;
static std::vector<uint8_t> _migInBlob;   // received STPU payload (ZCST-compressed)
static uint32_t   _migInRawSize = 0;
static bool       _migApplyPending = false;
static MirrorDecompressor _migDecomp;     // boot-reserved (migEagerInit)
static bool       _migDecompInit = false;

static const char* fleetKey() { return std::getenv("MAPLECAST_FLEET_KEY"); }

// MemAvailable in MB (Linux; ~unlimited elsewhere). The 955MB edges sit at
// ~50-70MB available with flycast ~755MB resident — an apply OR a capture
// there OOM-KILLS the node (proven three times on sea, 2026-07-15, despite
// 3GB free swap: the burst outruns reclaim). A node that can't afford the
// work must REFUSE it — the player stays put with a clear error instead of
// the destination dying and rebooting into its own autoload.
static long memAvailableMB()
{
#ifdef __linux__
	FILE* f = fopen("/proc/meminfo", "r");
	if (!f) return LONG_MAX;
	char line[128];
	long kb = -1;
	while (fgets(line, sizeof line, f))
		if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1)
			break;
	fclose(f);
	return kb < 0 ? LONG_MAX : kb / 1024;
#else
	return LONG_MAX;
#endif
}
static const long MIG_RECV_MIN_MB = 150;   // blob + arenas already reserved + loadstate churn
static const long MIG_SEND_MIN_MB = 250;   // 28MB state malloc + 42MB compressor + 10MB frame

// Reserve the migration arenas at BOOT, not on the hot path. Edge nodes run
// ~50MB from the ceiling; the receive+apply allocation burst (10MB blob +
// 32MB decompress buffer) outran kernel reclaim TWICE on sea (OOM-KILL,
// 2026-07-15) even with 3GB of free swap. Boot-time allocation succeeds while
// memory is calm and the pages stay swappable; the hot path then allocates
// ~nothing beyond websocketpp's own payload buffer.
static void migEagerInit()
{
	const char* key = fleetKey();
	if (!key || !*key)
		return;
	_migDecomp.init(32 * 1024 * 1024);
	_migDecompInit = true;
	_migInBlob.reserve(16 * 1024 * 1024);
	printf("[migrate] armed (MAPLECAST_FLEET_KEY set) -- 48MB arenas reserved at boot\n");
	fflush(stdout);
}

static void migNotifyRequester(ConnHdl hdl, const json& msg)
{
	try { _ws.send(hdl, msg.dump(), websocketpp::frame::opcode::text); } catch (...) {}
}

// Raw RFC 6455 WS client: connect to host:port, opt out of the binary
// broadcast (control_only), push ONE binary frame, wait for the stpu_ack /
// stpu_err text reply. Returns true on acked. Runs on a detached worker.
static bool migPushToDest(const std::string& hostPort, const std::vector<uint8_t>& frame,
                          std::string& err)
{
	std::string host = hostPort;
	int port = 7200;
	auto colon = hostPort.rfind(':');
	if (colon != std::string::npos) {
		host = hostPort.substr(0, colon);
		port = std::atoi(hostPort.c_str() + colon + 1);
		if (port <= 0) port = 7200;
	}

	struct addrinfo hints = {}, *res = nullptr;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	char portStr[16];
	snprintf(portStr, sizeof portStr, "%d", port);
	if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) {
		err = "DNS resolve failed for " + host;
		return false;
	}
	auto sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (connect(sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
		freeaddrinfo(res);
		mc_closesocket(sock);
		err = "connect failed to " + hostPort;
		return false;
	}
	freeaddrinfo(res);
#ifdef _WIN32
	DWORD tmo = 20000;
	mc_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
#else
	struct timeval tv = {20, 0};
	mc_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif

	auto sendAll = [&](const void* p, size_t n) -> bool {
		const char* c = (const char*)p;
		while (n) {
			int w = mc_send(sock, c, n, 0);
			if (w <= 0) return false;
			c += w; n -= (size_t)w;
		}
		return true;
	};

	// HTTP upgrade
	char req[512];
	snprintf(req, sizeof req,
		"GET / HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
		"Sec-WebSocket-Key: bWFwbGVjYXN0LW1pZ3JhdGU=\r\nSec-WebSocket-Version: 13\r\n\r\n",
		host.c_str(), port);
	if (!sendAll(req, strlen(req))) { mc_closesocket(sock); err = "handshake send failed"; return false; }
	std::string resp;
	while (resp.find("\r\n\r\n") == std::string::npos && resp.size() < 8192) {
		char buf[1024];
		int r = mc_recv(sock, buf, sizeof buf, 0);
		if (r <= 0) { mc_closesocket(sock); err = "handshake recv failed"; return false; }
		resp.append(buf, r);
	}
	if (resp.find(" 101 ") == std::string::npos) {
		mc_closesocket(sock);
		err = "upgrade rejected: " + resp.substr(0, resp.find("\r\n"));
		return false;
	}

	// Client frames MUST be masked; an all-zero mask key is valid and keeps
	// the multi-MB payload zero-copy (XOR with 0 = identity).
	auto sendFrame = [&](uint8_t opcode, const void* p, size_t n) -> bool {
		uint8_t hdr[14];
		size_t h = 0;
		hdr[h++] = 0x80 | opcode;
		if (n < 126) hdr[h++] = 0x80 | (uint8_t)n;
		else if (n < 65536) {
			hdr[h++] = 0x80 | 126;
			hdr[h++] = (uint8_t)(n >> 8); hdr[h++] = (uint8_t)n;
		} else {
			hdr[h++] = 0x80 | 127;
			for (int i = 7; i >= 0; i--) hdr[h++] = (uint8_t)((uint64_t)n >> (8 * i));
		}
		hdr[h++] = 0; hdr[h++] = 0; hdr[h++] = 0; hdr[h++] = 0;   // mask key = 0
		return sendAll(hdr, h) && sendAll(p, n);
	};

	const char* optOut = "{\"type\":\"control_only\"}";
	if (!sendFrame(0x1, optOut, strlen(optOut)) || !sendFrame(0x2, frame.data(), frame.size())) {
		mc_closesocket(sock);
		err = "push send failed";
		return false;
	}

	// Read server frames; skip anything that isn't our text ack (the dest may
	// still ship its initial SYNC before control_only lands).
	std::vector<uint8_t> rbuf;
	auto needBytes = [&](size_t n) -> bool {
		while (rbuf.size() < n) {
			char buf[65536];
			int r = mc_recv(sock, buf, sizeof buf, 0);
			if (r <= 0) return false;
			rbuf.insert(rbuf.end(), buf, buf + r);
		}
		return true;
	};
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
	while (std::chrono::steady_clock::now() < deadline) {
		if (!needBytes(2)) { err = "ack recv failed/timeout"; mc_closesocket(sock); return false; }
		uint8_t op = rbuf[0] & 0x0F;
		uint64_t len = rbuf[1] & 0x7F;
		size_t off = 2;
		if (len == 126) { if (!needBytes(4)) break; len = ((uint64_t)rbuf[2] << 8) | rbuf[3]; off = 4; }
		else if (len == 127) {
			if (!needBytes(10)) break;
			len = 0;
			for (int i = 0; i < 8; i++) len = (len << 8) | rbuf[2 + i];
			off = 10;
		}
		if (!needBytes(off + len)) break;
		if (op == 0x1) {
			std::string txt((const char*)rbuf.data() + off, (size_t)len);
			rbuf.erase(rbuf.begin(), rbuf.begin() + (long)(off + len));
			try {
				auto j = json::parse(txt);
				if (j.value("type", "") == "stpu_ack") { mc_closesocket(sock); return true; }
				if (j.value("type", "") == "stpu_err") {
					err = j.value("error", "dest rejected push");
					mc_closesocket(sock);
					return false;
				}
			} catch (...) {}
			continue;   // other lobby JSON -- keep waiting
		}
		rbuf.erase(rbuf.begin(), rbuf.begin() + (long)(off + len));   // skip binary
	}
	mc_closesocket(sock);
	err = "no ack from dest within 25s";
	return false;
}

// Text-side entry: {"type":"migrate","dest":...,"key":...}. Runs on the asio
// handler thread -- only validates + stashes; the emu thread does the capture.
static void migHandleRequest(ConnHdl hdl, const json& ctrl)
{
	const char* key = fleetKey();
	if (!key || !*key) {
		migNotifyRequester(hdl, {{"type","migrate_failed"},{"error","migration disabled: MAPLECAST_FLEET_KEY not set on this server"}});
		return;
	}
	if (ctrl.value("key", "") != key) {
		migNotifyRequester(hdl, {{"type","migrate_failed"},{"error","bad fleet key"}});
		return;
	}
	std::string dest = ctrl.value("dest", "");
	if (dest.empty() || dest.size() > 128) {
		migNotifyRequester(hdl, {{"type","migrate_failed"},{"error","missing dest"}});
		return;
	}
	{
		long availMB = memAvailableMB();
		if (availMB < MIG_SEND_MIN_MB) {
			migNotifyRequester(hdl, {{"type","migrate_failed"},
				{"error","source low on memory (" + std::to_string(availMB) + "MB available)"}});
			return;
		}
	}
	static std::atomic<int64_t> lastMs{0};
	int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	int64_t prev = lastMs.load(std::memory_order_relaxed);
	if (now - prev < 10000 || !lastMs.compare_exchange_strong(prev, now)) {
		migNotifyRequester(hdl, {{"type","migrate_failed"},{"error","migration rate-limited (10s)"}});
		return;
	}
	{
		std::lock_guard<std::mutex> lock(_migMu);
		_migDest = dest;
		_migRequester = hdl;
		_migSendPending = true;
	}
	printf("[migrate] request -> %s (capture on next frame boundary)\n", dest.c_str());
	fflush(stdout);
}

// Binary-side entry: an incoming STPU push from another fleet node. Validate +
// stash + receipt-ack; the emu thread applies at the next frame boundary.
static void migHandleIncomingPush(ConnHdl hdl, const std::string& data)
{
	const char* key = fleetKey();
	auto reject = [&](const char* why) {
		printf("[migrate] STPU rejected: %s\n", why);
		fflush(stdout);
		migNotifyRequester(hdl, {{"type","stpu_err"},{"error",why}});
	};
	if (!key || !*key) { reject("MAPLECAST_FLEET_KEY not set on receiver"); return; }
	{
		long availMB = memAvailableMB();
		if (availMB < MIG_RECV_MIN_MB) {
			char why[96];
			snprintf(why, sizeof why,
			         "receiver low on memory (%ldMB available, need %ldMB)", availMB, MIG_RECV_MIN_MB);
			reject(why);
			return;
		}
	}
	if (data.size() < 12) { reject("short frame"); return; }
	uint32_t keyLen, rawSize;
	memcpy(&keyLen, data.data() + 4, 4);
	if (keyLen > 256 || data.size() < 12 + keyLen) { reject("bad key length"); return; }
	if (std::string(data.data() + 8, keyLen) != key) { reject("bad fleet key"); return; }
	memcpy(&rawSize, data.data() + 8 + keyLen, 4);
	if (rawSize < 1024 * 1024 || rawSize > 128u * 1024 * 1024) { reject("implausible raw size"); return; }
	{
		std::lock_guard<std::mutex> lock(_migMu);
		_migInBlob.assign(data.begin() + 12 + keyLen, data.end());
		_migInRawSize = rawSize;
		_migApplyPending = true;
	}
	// Pop the emu thread out of runInternal() at the next TRUE frame boundary
	// (emulator.cpp:2171 consumes this and Stop()s the SH4) — WITHOUT this,
	// runInternal never returns on a plain server and the apply site is never
	// reached (proven by the round-3 gate: hook heartbeat fired once at boot).
	maplecast_mirror::raArmStepStop();
	printf("[migrate] STPU received: %zu B compressed (raw %u B) -- apply on next frame boundary\n",
	       data.size() - 12 - keyLen, rawSize);
	fflush(stdout);
	migNotifyRequester(hdl, {{"type","stpu_ack"},{"raw",rawSize}});
}

// EMU THREAD, frame boundary, SH4 halted — called from Emulator::run() BEFORE
// the frame executes (the state_replica::frameInject precedent). The first
// live gate (2026-07-15) proved the serverPublish site is the RENDER thread:
// applying there swapped RAM under a RUNNING SH4 -> verify() abort 0x80000003
// moments after "APPLIED". Only this site is safe for the apply.
bool applyPendingMigration()
{
	// Hold _migMu through the whole apply: the only contender is the WS
	// thread stashing a second push, which SHOULD wait out an in-flight
	// load. Decompressing straight out of the boot-reserved _migInBlob
	// arena (clear() keeps its capacity) means zero hot-path allocation —
	// the OOM-kill fix, see migEagerInit().
	std::lock_guard<std::mutex> lock(_migMu);
	if (!_migApplyPending)
		return false;
	_migApplyPending = false;
	const uint32_t rawSize = _migInRawSize;
	if (!_migDecompInit) { _migDecomp.init(32 * 1024 * 1024); _migDecompInit = true; }
	size_t gotRaw = 0;
	const uint8_t* raw = _migDecomp.decompress(_migInBlob.data(), _migInBlob.size(), gotRaw);
	_migInBlob.clear();   // keep the reserved capacity
	if (!raw || gotRaw != rawSize) {
		printf("[migrate] apply ABORTED: decompress got %zu B, expected %u B\n", gotRaw, rawSize);
		fflush(stdout);
		return true;   // work was pending: caller must Start()+continue either way
	}
	try {
		// Suppress publishes while the state swaps (run-ahead's hidden-leg
		// tool): a viewer-connected node's render thread would otherwise
		// diff/ship RAM+VRAM mid-memcpy — a torn frame at best.
		maplecast_mirror::setSuppressPublish(true);
		// Render guard = the A2 run-ahead recipe (emulator.cpp:1860): DRAIN
		// the render queue, then loadstate + rend_resync. NOT the lockstep
		// client's rend_start_rollback() -- that waits on a vramRollback
		// signal an idle headless render thread never fires (hang, first
		// gate attempt 2026-07-15). Full (non-lightweight) loadstate: this
		// is FOREIGN state, the JIT cache/textures must flush.
		rend_wait_render_idle();
		Deserializer deser(raw, gotRaw, /*rollback=*/false);
		emu.loadstate(deser);
		rend_resync_after_rollback();
		if (!sh4_sched_is_scheduled(vblank_schid)) {
			const int re_sch = spg_getNextInterrupt();
			sh4_sched_request(vblank_schid, re_sch);
			printf("[migrate] re-armed vblank_schid at +%d cycles\n", re_sch);
		}
		maplecast_mirror::setSuppressPublish(false);
	} catch (const std::exception& e) {
		maplecast_mirror::setSuppressPublish(false);
		printf("[migrate] emu.loadstate threw: %s -- state unchanged\n", e.what());
		fflush(stdout);
		return true;
	}
	// Realign every mirror consumer: fresh SYNC for legacy clients, forced
	// TDWS snapshot for TDW clients (ARCHITECTURE.md bug #8 discipline).
	maplecast_mirror::requestSyncBroadcast();
	printf("[migrate] APPLIED %u B state -- game continues from the source node\n", rawSize);
	fflush(stdout);
	return true;
}

// serverPublish site (render thread): the CAPTURE side only. dc_serialize from
// here is the same proven path as drainMcsvCapture / control-WS savestate_save.
void drainMigration()
{
	// -- capture + hand off (this node is the SOURCE) --
	std::string dest;
	ConnHdl requester;
	{
		std::lock_guard<std::mutex> lock(_migMu);
		if (!_migSendPending) return;
		// DISC-QUIET GUARD: a state captured mid-read (char-select -> match
		// load streams from GD-ROM) carries in-flight drive state that
		// reproducibly wedges the DESTINATION at match-load ("freezes then
		// back to character select", 2026-07-15). Defer up to ~5s for a
		// quiet frame; loads finish in well under that.
		static int deferred = 0;
		if (gdrom::maplecast_gdrom_busy() && deferred < 300) {
			if (deferred++ == 0) {
				printf("[migrate] disc busy at capture request -- deferring to a quiet frame\n");
				fflush(stdout);
			}
			return;   // _migSendPending stays set; retry next frame
		}
		if (deferred >= 300)
			printf("[migrate] disc still busy after 5s -- capturing anyway (bounded)\n");
		deferred = 0;
		dest = _migDest;
		requester = _migRequester;
		_migSendPending = false;
	}
	size_t rawSz = 0;
	uint8_t* rawState = maplecast_mirror::buildFullSaveState(rawSz);
	if (!rawState) {
		migNotifyRequester(requester, {{"type","migrate_failed"},{"error","state capture failed"}});
		return;
	}
	printf("[migrate] captured %zu B state -- compress+push to %s off-thread\n", rawSz, dest.c_str());
	fflush(stdout);
	std::thread([dest, requester, rawState, rawSz]() {
		MirrorCompressor comp;
		comp.init(rawSz + rawSz / 2 + 1024);
		size_t compSize = 0;
		uint64_t compUs = 0;
		const uint8_t* compPtr = comp.compress(rawState, (uint32_t)rawSz, compSize, compUs);
		const char* key = fleetKey();   // validated non-null at request time
		uint32_t keyLen = (uint32_t)strlen(key), raw32 = (uint32_t)rawSz;
		std::vector<uint8_t> frame;
		frame.reserve(12 + keyLen + compSize);
		frame.insert(frame.end(), {'S','T','P','U'});
		frame.insert(frame.end(), (uint8_t*)&keyLen, (uint8_t*)&keyLen + 4);
		frame.insert(frame.end(), key, key + keyLen);
		frame.insert(frame.end(), (uint8_t*)&raw32, (uint8_t*)&raw32 + 4);
		frame.insert(frame.end(), compPtr, compPtr + compSize);
		free(rawState);
		std::string err;
		if (migPushToDest(dest, frame, err)) {
			printf("[migrate] dest %s ACKED (%zu B wire) -- redirecting client\n", dest.c_str(), frame.size());
			fflush(stdout);
			migNotifyRequester(requester, {{"type","migrated"},{"dest",dest}});
		} else {
			printf("[migrate] push to %s FAILED: %s\n", dest.c_str(), err.c_str());
			fflush(stdout);
			migNotifyRequester(requester, {{"type","migrate_failed"},{"error",err}});
		}
	}).detach();
}

}  // namespace maplecast_ws
