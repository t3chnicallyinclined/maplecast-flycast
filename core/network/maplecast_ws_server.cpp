/*
	MapleCast WebSocket Server — binary mirror broadcast + JSON lobby on port 7200.

	On new client connect: sends full VRAM + PVR regs as initial sync, plus lobby status.
	Binary frames: delta TA commands broadcast to all clients.
	Text frames: JSON lobby (join, status) + binary gamepad input forwarding to UDP 7100.
*/
#include "maplecast_ws_server.h"
#include "maplecast_compress.h"
#include "maplecast_input_server.h"
#include "maplecast_gamestate.h"
#include "maplecast_mirror.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include "json/json.hpp"
#include <cstdio>
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
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <strings.h>   // strcasecmp
#include <unistd.h>
#endif

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

// Lobby: connection → slot mapping, queue tracking
static std::map<void*, int> _connSlot;

// Control-only connections — browsers that connect directly to flycast for
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

struct QueueEntry {
	void* key;
	std::string name;
	ConnHdl conn;
};

// ==================== P2P Spectator Relay Tree ====================
// Server feeds binary TA frames to 2-3 "seed" spectators.
// Seeds relay to children via WebRTC DataChannels (browser-side JS).
// Server manages topology, signals parent/child assignments via JSON.

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
static std::map<void*, std::string> _connToPeerId;  // connKey → peerId
static std::vector<QueueEntry> _queue;

// Loss detection state
static bool _matchActive = false;
static bool _matchEndHandled = false;
static int64_t _matchEndTime = 0; // when match ended (for delay before kick)
static int _pendingKickSlot = -1; // loser slot to evict if client doesn't self-disconnect

// Idle-kick threshold: a connected player who hasn't sent a button-state
// CHANGE in this many microseconds gets evicted from their slot. The clock
// is seeded fresh on join/reconnect so a slow joiner has the full window.
static constexpr int64_t IDLE_KICK_THRESHOLD_US = 30LL * 1000000LL;

// Forward declaration — defined further down, used by checkMatchEnd().
static void broadcastStatus();

// Server-side eviction primitive used by both the loser-kick path and the
// idle-kick path. Cleans up _connSlot, calls input::disconnectPlayer, and
// pushes {kicked, reason} + {assigned, slot:-1} to the evicted client so its
// UI resets without waiting for an onClose. Returns true if a slot was kicked.
//
// `slot`   — which player slot to evict (0 or 1)
// `reason` — short token sent to the client ("match_lost", "idle", ...)
static bool kickSlot(int slot, const char* reason)
{
	if (slot < 0 || slot > 1) return false;
	const auto& p = maplecast_input::getPlayer(slot);
	if (!p.connected) return false;

	printf("[maplecast-ws] SERVER KICK: P%d (%s) — reason=%s\n",
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
		return {
			{"id", std::string(p.id).substr(0, 8)},
			{"name", std::string(p.name)},
			{"device", std::string(p.device)},
			{"connected", true},
			{"type", typeStr},
			{"pps", p.packetsPerSec},
			{"cps", p.changesPerSec},
			{"src_ip", srcIpStr},
			{"src_port", srcPortVal}
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
	status["dirty"] = t.dirtyPages;
	status["registering"] = maplecast_input::isRegistering();
	status["web_registering"] = maplecast_input::isWebRegistering();
	status["web_registering_user"] = maplecast_input::isWebRegistering() ?
		maplecast_input::webRegisteringUsername() : "";
	status["sticks"] = maplecast_input::registeredStickCount();
	status["relay_seeds"] = seedCount;
	status["relay_nodes"] = treeSize;

	// Phase A — per-slot input latch telemetry. Sourced from the
	// LatchStatsAccum ring buffer that ggpo::getLocalInput() writes to
	// once per vblank for slots 0/1. Frontend renders these as a
	// histogram + counter set in the diagnostics overlay (A.6) so
	// players can see how their input timing relates to the latch
	// boundary.
	//   delta_us avg/p99/min/max — distribution of (t_latch - t_packet_arrival)
	//                              over the last ~256 latches (~4.3 s @ 60 Hz)
	//   total_latches             — every CMD9 vblank since boot
	//   latches_with_data         — vblanks where the network thread had
	//                              touched the slot since the previous latch
	//                              (= the slot saw a fresh packet this frame)
	//   last_seq, last_frame      — for live drift / diagnostics
	auto latchInfoJson = [](int slot) -> json {
		auto s = maplecast_input::getLatchStats(slot);
		return {
			{"total_latches",     (int64_t)s.totalLatches},
			{"latches_with_data", (int64_t)s.latchesWithData},
			{"avg_delta_us",      s.avgDeltaUs},
			{"p99_delta_us",      s.p99DeltaUs},
			{"min_delta_us",      s.minDeltaUs},
			{"max_delta_us",      s.maxDeltaUs},
			{"last_packet_seq",   (int64_t)s.lastPacketSeq},
			{"last_frame",        (int64_t)s.lastFrameNum},
		};
	};
	json latchStats;
	latchStats["p1"] = latchInfoJson(0);
	latchStats["p2"] = latchInfoJson(1);
	status["latch_stats"] = latchStats;

	// Phase B — frame phase publication. Tells the browser-side gamepad
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
		// Phase B guard window in microseconds — exposed so the browser
		// can shift its sends to land just OUTSIDE the guard window
		// (avoiding the deferred-by-one-frame penalty under
		// ConsistencyFirst).
		fp["guard_us"] = (int64_t)maplecast_input::getGuardUs();
		status["frame_phase"] = fp;
	}

	// Phase B — per-slot latch policy (latency / consistency). Lets the
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

// Idle-kick — evict any player who hasn't pressed a button in 30 seconds.
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
		// every status broadcast — which is exactly the loop bug that
		// produced 115 ghost match rows in 30 seconds on 2026-04-06.
		if (!_matchActive) {
			_matchActive = true;
			_matchEndHandled = false;
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
		// Client gets 3s grace via match_end → leaveGame() self-disconnect.
		// Server kicks at 5s as a safety net (closed tab, killed JS, malicious client).
		int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::high_resolution_clock::now().time_since_epoch()).count();

		if (now - _matchEndTime > 5000) {
			_matchActive = false;

			// King-of-the-hill: only evict loser if someone is waiting in queue.
			// Empty queue → winner stays on, loser keeps slot for rematch.
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

	// Orphan children — reassign them
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
	printf("[maplecast-ws] client connected (%d total)\n", _clientCount.load());

	// Always send SYNC on connect — backward compat for clients without relay.js
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
			_queue.erase(std::remove_if(_queue.begin(), _queue.end(),
				[key](const QueueEntry& e) { return e.key == key; }), _queue.end());
		} catch (...) {}
		_clientCount--;
	}

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

static void onMessage(ConnHdl hdl, WsServer::message_ptr msg)
{
	if (msg->get_opcode() == websocketpp::frame::opcode::binary)
	{
		// Binary = gamepad input (4 bytes: LT, RT, btnHi, btnLo)
		const auto& data = msg->get_payload();
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
				// Unassigned player — send raw 4-byte
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
				// Browser → flycast direct connection that does NOT want the
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
							// Drop the stale conn→slot mapping AND notify the
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

				// Register connection → slot mapping
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
					// the PLAYER across slot reassignments — the preference
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
				// Phase B + per-user gate — live per-slot latch policy switch.
				// Players choose between LatencyFirst (today's behavior) and
				// ConsistencyFirst (accumulator + edge preservation + guard
				// window). The policy follows the PLAYER, not the chair —
				// it's stored client-side in localStorage and re-pushed via
				// the join handshake whenever they (re)take a slot.
				//
				// Server-side gate: a connection can only set the policy for
				// the slot IT owns. Spectators and the other player are
				// rejected. This is the load-bearing security check; the UI
				// only HIDES the other slot's button, but server enforces.
				int slot = ctrl.value("slot", -1);
				std::string policyStr = ctrl.value("policy", "");

				// Identity check — what slot does THIS connection actually own?
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
				// whatever we have in RAM (well — installs, doesn't wipe;
				// see installStickBindings). Trusted source, so no auth check
				// here yet — relies on collector being on the same WS:7200
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
							// No room in tree — make another seed
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
				// Child reports parent DataChannel died — reassign
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
				// Health report from relay node — log for now
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
		// The relay→home and browser→relay paths benefit by 0-40ms p99.
		_ws.set_socket_init_handler([](websocketpp::connection_hdl,
		                               websocketpp::lib::asio::ip::tcp::socket& s) {
			websocketpp::lib::asio::error_code ec;
			s.set_option(websocketpp::lib::asio::ip::tcp::no_delay(true), ec);
		});

		_ws.set_open_handler(&onOpen);
		_ws.set_close_handler(&onClose);
		_ws.set_message_handler(&onMessage);

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
		close(_udpSock);
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

	std::lock_guard<std::mutex> lock(_connMutex);

	// Build set of connections that should NOT receive binary
	// (non-seed relay nodes that get frames via WebRTC instead)
	std::set<void*> relaySkip;
	for (auto& [key, peerId] : _connToPeerId) {
		auto it = _relayTree.find(peerId);
		if (it != _relayTree.end() && !it->second.isSeed && !it->second.isPlayer)
			relaySkip.insert(key);
	}

	for (auto& conn : _connections) {
		try {
			void* key = (void*)_ws.get_con_from_hdl(conn).get();
			if (relaySkip.count(key)) continue;          // gets frames via WebRTC relay
			if (_controlOnlyConns.count(key)) continue;  // browser direct control WS — gets frames via VPS relay
			_ws.send(conn, data, size, websocketpp::frame::opcode::binary);
		} catch (...) {}
	}
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

	// Compress with zstd (ZCST magic) — same path as onOpen
	MirrorCompressor syncComp;
	syncComp.init(syncSize + 128);
	size_t compSyncSize = 0;
	uint64_t compUs = 0;
	const uint8_t* compSync = syncComp.compress(syncBuf.data(), (uint32_t)syncSize, compSyncSize, compUs, 3);

	// Broadcast to ALL clients — even relay children get this directly. The
	// relay tree's delta-only path is fine for normal frames but a scene
	// transition needs to invalidate everyone's state at once.
	// EXCEPT control-only connections — they get SYNC via the VPS relay path,
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

	printf("[maplecast-ws] SCENE-CHANGE SYNC: %.1f MB -> %.1f MB (%.1fx) in %lums to %d clients\n",
		syncSize / (1024.0 * 1024.0), compSyncSize / (1024.0 * 1024.0),
		(double)syncSize / compSyncSize, compUs / 1000, _clientCount.load());
}

void updateTelemetry(const Telemetry& t)
{
	std::lock_guard<std::mutex> lock(_telemetryMutex);
	_telemetry = t;
}

// Stub: declared in the header, called by maplecast_mirror.cpp's
// doForcedSaveStateBroadcast(). The full implementation existed as
// uncommitted working-tree code earlier in the session and got reverted
// during a cleanup pass. doForcedSaveStateBroadcast() only fires on
// SIGUSR1 / explicit reset, so this no-op is harmless under normal
// operation. Restore the real impl if you need SAVE blob broadcasts.
void broadcastSaveStateBytes(const void* /*data*/, size_t /*size*/)
{
	printf("[maplecast-ws] broadcastSaveStateBytes — STUB (not implemented)\n");
}

}  // namespace maplecast_ws
