/*
	================================================================
	SHELVED 2026-04-09 â€” superseded by GGPO peer mode
	================================================================
	This file (and maplecast_state_sync.cpp) was a hand-rolled
	replay client: bespoke UDP tape subscriber + bespoke TCP state-
	sync + a stall-only frameGate() with no rollback. It boots and
	runs against nobd.net but desyncs from the authoritative server
	because it lacks save-state ring + rollback + fast-forward.

	The correct architecture â€” which this branch is now moving to â€”
	is to reuse flycast's existing GGPO integration in
	core/network/ggpo.cpp (1067 lines, fully wired: ggpo_start_session,
	ggpo_synchronize_input, ggpo_advance_frame, save_game_state /
	load_game_state callbacks via dc_serialize/dc_deserialize, the
	whole rollback + fast-forward machinery). The headless server
	becomes one GGPO peer, the native client becomes the other.
	GGPO's own save-state ring covers replay correctness. No
	frameGate, no bespoke tape, no bespoke state-sync.

	WHY NOT DELETED YET
	  - Still compiled into the build, still exported via
	    maplecast_player::init() / frameGate() â€” emulator.cpp:1006-
	    1010 and :1090 / :1155 call into it.
	  - Held as a fallback diagnostic until the GGPO peer mode is
	    proven end-to-end against nobd.net.
	  - Once GGPO peer mode is green, this file + maplecast_state_sync.*
	    + the publishFrameTick / tape ring code in
	    maplecast_input_server.cpp can all be removed in one commit.

	DO NOT add features here. Anything new goes into the GGPO path.
	================================================================

	MapleCast Player Client â€” lockstep tape subscriber.

	See maplecast_player.h for the big-picture design. This file implements:

	  1. A UDP subscriber on an ephemeral port that:
	     - sends HELO to the server's tape port every ~1 second
	     - receives INPT datagrams, parses TapeEntry records, and hands
	       them to a per-slot pending queue
	  2. frameGate(localFrame) which, before each emu frame, drains the
	     queue up to localFrame and writes kcode[]/lt[]/rt[] for slots 0-1

	PHASE 2 SCOPE
	  - Assumes client and server are booted from the same ROM at the same
	    cold-boot state. No savestate sync yet (that's Phase 3).
	  - Slots 0-1 only (matches maplecast_input's authoritative scope).
	  - Local gamepads are NOT sent to the server from inside this module;
	    that's the responsibility of the existing input send path
	    (browser WS client / NOBD-style UDP sender). Phase 2 just proves
	    the lockstep replay works.
*/
#include "types.h"
#include "maplecast_player.h"
#include "maplecast_input_server.h"   // TapeEntry, unpackSeqSlot, kTapePort implicit
#include "maplecast_state_sync.h"
#include "maplecast_lockstep.h"       // lockstep checksum layer (env-gated, OFF by default)
#include "maplecast_predict.h"        // client-side predict/rollback (env-gated, OFF by default)
#include "maplecast_rollback.h"       // gameStateRegionHash (predict-live confirmed-hash gate)

#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <ctime>

#include "net_platform.h"
#include "maplecast_compat.h"
#include "cfg/option.h"          // config::ThreadedRendering override for lockstep
#include "hw/pvr/Renderer_if.h"  // rend_enable_renderer() — lag-driven render-skip

// kcode[]/lt[]/rt[] globals â€” same globals the server writes in updateSlot()
extern u32 kcode[4];
extern u16 rt[4], lt[4];

namespace maplecast_player
{

// Local-input redirect: the raw local stick, kept OUT of the sim's kcode[] so
// the sim is tape-only (see maplecast_player.h). Active-low buttons / triggers.
uint32_t g_localKcode[4] = { ~0u, ~0u, ~0u, ~0u };
uint16_t g_localLt[4]    = { 0, 0, 0, 0 };
uint16_t g_localRt[4]    = { 0, 0, 0, 0 };
static std::atomic<bool> _redirectLocal{false};
bool localInputRedirectActive() { return _redirectLocal.load(std::memory_order_relaxed); }

static std::atomic<bool>      _active{false};
static std::atomic<bool>      _connected{false};
static std::thread            _rxThread;
static int                    _sock = -1;
static sockaddr_in            _serverAddr{};     // tape server endpoint
static std::string            _serverHost;       // bare host (no :port) â€” state-sync connects to kStatePort
static std::atomic<uint8_t>   _stallPolicy{(uint8_t)StallPolicy::Hard};

// True once maplecast_state_sync::clientApplyPending() has successfully
// applied at least one STAT envelope. Until then we don't know what
// server frame the SH4 is at, so frameGate MUST stall â€” we can't trust
// the internal _localFrame counter against the tape queue.
static std::atomic<bool>      _initialSynced{false};

// Per-slot pending queue of tape entries, ordered by frame. The receive
// thread pushes back; frameGate pops front as entries mature.
//
// Ordering: the server emits entries in push order per slot, and UDP is
// unordered but single-host. We sort-insert on arrival to tolerate out-
// of-order packets. Capped length â€” if the client is catastrophically
// behind, we drop old entries rather than run out of memory.
static constexpr size_t kMaxQueueLen = 2048;

struct PendingQueue {
	std::deque<maplecast_input::TapeEntry> entries;
	std::mutex                              mu;
};
static PendingQueue _queues[2];

// ── CAPSTONE — live predict-drive state (MAPLECAST_PREDICT_LIVE=1) ───────────
// The predicted head runs ahead of confirmed; per-frame we record the predicted
// input (to detect mispredicts) and buffer authoritative tape entries (to
// re-apply on rollback). PRING > ring DEPTH so the lead is always covered.
static constexpr int LP_RING = 128;
struct LpInput { uint16_t btn = 0xFFFF; uint8_t lt = 0, rt = 0; };
static LpInput   _lpPred[2][LP_RING];        // predicted input we applied, per slot
static LpInput   _lpAuth[2][LP_RING];        // authoritative input, per slot
static uint64_t  _lpAuthFrame[2][LP_RING];   // which frame that auth slot holds (validate)
static uint64_t  _lpConfirmed = 0;           // last fully-authoritative frame run
static bool      _lpInited    = false;
static LpInput   _lpLastConfRemote;          // remote's last confirmed input (repeat-predict)
// APPLY-PHASE FIX (STEP 2): persists the last logical head input across drive
// calls so the head's 1-frame input pipeline is continuous in steady state
// (target==P, zero catch-up frames) — the rendered frame consumes the previous
// tick's input, matching the server's Emulator::run phase.
static LpInput   _lpHeadPipe;
static uint64_t  _lpRollbacks = 0, _lpMaxDepth = 0, _lpReJoins = 0;
// ROLLBACK CLASSIFIER (diagnostic, purely local): after each re-sim, compare the head's
// state hash pre- vs post-rollback. SPURIOUS = re-sim produced an identical head state
// (the input mispredict changed nothing on screen -> the rollback + its ~4ms/frame SH-4
// cost was wasted). REAL = the head state actually changed (a necessary correction). The
// spurious:real ratio decides whether the jitter is skippable or inherent.
static uint64_t  _lpRbSpurious = 0, _lpRbReal = 0;
// Local input at its EFFECT frame: the client samples the stick at tick H and the
// input takes effect at frame H+INPUT_DELAY, on BOTH the client's predicted head
// AND the server (frame-stamped) — so the predicted local input == the
// authoritative echo => self-mispredict = 0 (stage c invariant, applied live).
// Headless input-injection test (MAPLECAST_PREDICT_LIVE_INJECT): drive a held
// local input for a window WITHOUT a stick, to validate the input path.
static uint64_t  _lpInjectStart = 0;
static uint64_t  _lpRbAtInjectStart = 0;
static uint64_t  _lpInjEchoed = 0, _lpInjMatched = 0, _lpInjConfirmed = 0;
static uint64_t  _lpArmHead = 0, _lpGapMax = 0;
// RUN-AHEAD: serverLive estimate anchors (lockstep serverFrame extrapolated by
// head-ticks) + newest tape frame seen. The head is driven to serverLive+INPUT_DELAY.
static uint64_t  _lpSrvFrame = 0, _lpSrvAnchorHead = 0, _lpTapeNewest = 0;
static uint64_t  _lpLeadMin = UINT64_MAX, _lpLeadMax = 0;
// CONFIRMED-HASH gate: the head's game-state hash per frame; when a frame confirms
// (no rollback), compared to the server's authoritative hash for that frame.
static uint64_t  _lpLiveHash[LP_RING];
static uint64_t  _lpLiveHashFrame[LP_RING];
static bool      _lpLiveHashInit = false;
static uint64_t  _lpConfMatch = 0, _lpConfMismatch = 0;
static uint64_t  _lpConfM[3] = {0,0,0}, _lpConfMM[3] = {0,0,0};   // offsets N-1,N,N+1
static uint64_t  _lpLiveSub[LP_RING][5];                          // per-frame sub-hashes
static int       _lpSubLogged = 0;                                // limit mismatch logs

// INJECTION TEST (MAPLECAST_PREDICT_LIVE_INJECT): drive a SUSTAINED + CHANGING
// local input for a long window WITHOUT hardware, set at the TOP of frameGate so
// forwardLocalInput() (which reads g_localKcode) sees it — same timing the real
// gamepad thread has. Hold 60, mash 300, release. Summary logged at window end.
static void lpInjectTick(uint64_t P, int ls)
{
	if (!std::getenv("MAPLECAST_PREDICT_LIVE_INJECT")) return;
	if (_lpArmHead == 0) return;   // wait until the drive has armed
	if (_lpInjectStart == 0 && P > _lpArmHead + 200) { _lpInjectStart = P + 10; _lpRbAtInjectStart = _lpRollbacks; }
	const uint64_t s = _lpInjectStart;
	if (s && P >= s && P < s + 60) {
		g_localKcode[ls] = 0xFFFF0000u | 0xFEFF; g_localLt[ls] = 0; g_localRt[ls] = 0;
	} else if (s && P >= s + 60 && P < s + 360) {
		static const uint16_t mash[4] = { 0xFFBF, 0xFF7F, 0xFEFF, 0xFFFF };
		g_localKcode[ls] = 0xFFFF0000u | mash[((P - s) / 4) % 4];
		g_localLt[ls] = 0; g_localRt[ls] = 0;
	} else if (s && P == s + 360) {
		g_localKcode[ls] = 0xFFFFFFFFu;
		printf("[predict-live] ===== SUSTAINED-INJECT RESULT (360-frame press): "
		       "confirmed=%llu echoed=%llu matched=%llu(want==confirmed) "
		       "rollbacks-during-press=%llu(want ~0) maxDepth=%llu maxGap=%llu(want flat) =====\n",
		       (unsigned long long)_lpInjConfirmed, (unsigned long long)_lpInjEchoed,
		       (unsigned long long)_lpInjMatched,
		       (unsigned long long)(_lpRollbacks - _lpRbAtInjectStart),
		       (unsigned long long)_lpMaxDepth, (unsigned long long)_lpGapMax);
		fflush(stdout);
	}
}

// Telemetry
static std::atomic<uint64_t> _packetsReceived{0};
static std::atomic<uint64_t> _entriesReceived{0};
static std::atomic<uint64_t> _entriesApplied{0};
static std::atomic<uint64_t> _entriesDroppedStale{0};
static std::atomic<uint64_t> _framesStalled{0};
static std::atomic<uint64_t> _framesSpeculated{0};
static std::atomic<uint64_t> _lastAppliedFrame{0};
static std::atomic<uint64_t> _serverLatestFrame{0};
static std::atomic<int64_t>  _lastPacketArrivalUs{0};

// Internal monotonic local frame counter. Advances by 1 each time
// frameGate() returns true. seedLocalFrame() can override it (used by
// Phase 3 savestate sync). Owned by the emu thread after init â€” the
// atomic is defensive in case other threads want to read it for UI.
static std::atomic<uint64_t> _localFrame{0};

// Phase 4-lite: local SDL gamepad input forwarding. We snapshot the
// locally-written kcode[0]/lt[0]/rt[0] at the top of frameGate() â€”
// BEFORE the tape overwrites them with authoritative server state â€”
// and if the state has changed since the last send, ship a 7-byte PC
// packet to the server's MapleCast input port (7100). The server
// auto-binds our source IP to slot 0 on first packet.
//
// This is a raw UDP send on the emu thread â€” no retry, no acks. The
// input server treats it as "latest wins" so a single lost packet is
// self-healing: the next state change re-sends the full kcode. Packet
// rate is bounded by the emu frame rate (~60 Hz) and only when state
// actually changes, so it's well under any reasonable rate limit.
static int            _fwdSock          = -1;
static sockaddr_in    _fwdInputAddr{};   // server MapleCast input port (7100)
static uint16_t       _fwdLastButtons   = 0xFFFF;   // active-low idle
static uint8_t        _fwdLastLt        = 0;
static uint8_t        _fwdLastRt        = 0;
static bool           _fwdHasFirst      = false;
static int            _fwdClaimedSlot   = 0;         // MAPLECAST_PLAYER_SLOT env var
static std::atomic<uint64_t> _fwdPacketsSent{0};

static inline int64_t nowUs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

// Parse "host" or "host:port" into _serverAddr. Port defaults to the
// tape publisher's port (7101) if omitted. Returns true on success.
static bool resolveServer(const char* spec)
{
	if (!spec || !*spec) return false;

	std::string s = spec;
	std::string host = s;
	int port = 7101;   // matches kTapePort in maplecast_input_server.cpp

	size_t colon = s.find_last_of(':');
	if (colon != std::string::npos) {
		// Ignore IPv6-style brackets for now â€” this project's deploy is v4.
		host = s.substr(0, colon);
		port = std::atoi(s.c_str() + colon + 1);
		if (port <= 0 || port > 65535) {
			printf("[player] bad port in MAPLECAST_PLAYER_CLIENT='%s'\n", spec);
			return false;
		}
	}

	_serverHost = host;

	struct addrinfo hints{};
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	struct addrinfo* res = nullptr;
	int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
	if (rc != 0 || !res) {
		printf("[player] getaddrinfo('%s') failed: %s\n",
		       host.c_str(), gai_strerror(rc));
		return false;
	}
	memcpy(&_serverAddr, res->ai_addr, sizeof(sockaddr_in));
	_serverAddr.sin_port = htons((uint16_t)port);
	freeaddrinfo(res);

	char ipstr[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &_serverAddr.sin_addr, ipstr, sizeof(ipstr));
	printf("[player] tape server resolved: %s:%d\n", ipstr, port);
	return true;
}

// Insert an entry into its slot's queue in frame-sorted order.
// Drops stale entries (frame older than the queue's front - 1) since
// they're useless â€” the only reason to keep older entries is if the emu
// is still catching up, which is handled by the front-pop drain in
// frameGate.
static void enqueueEntry(const maplecast_input::TapeEntry& e, uint8_t slot)
{
	if (slot > 1) return;
	PendingQueue& q = _queues[slot];
	std::lock_guard<std::mutex> lock(q.mu);

	// Hard cap â€” if someone's queue runaway, drop the oldest.
	while (q.entries.size() >= kMaxQueueLen) {
		q.entries.pop_front();
		_entriesDroppedStale.fetch_add(1, std::memory_order_relaxed);
	}

	// Fast path: entry is newer than tail â†’ append.
	if (q.entries.empty() || e.frame >= q.entries.back().frame) {
		q.entries.push_back(e);
		return;
	}

	// Slow path: out-of-order insert. Rare (single-host UDP).
	for (auto it = q.entries.rbegin(); it != q.entries.rend(); ++it) {
		if (it->frame <= e.frame) {
			q.entries.insert(it.base(), e);
			return;
		}
	}
	q.entries.push_front(e);
}

static void rxLoop()
{
	printf("[player] rx thread started\n");

	// Bind to an ephemeral port on any local interface so the server
	// can reply to HELOs. We connect() the socket to the server so
	// mc_recv() only delivers datagrams from that source (light filtering)
	// and mc_send() can go without a destination.
	int bsock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (bsock < 0) {
		printf("[player] socket() failed: %s\n", strerror(errno));
		return;
	}
	sockaddr_in local{};
	local.sin_family      = AF_INET;
	local.sin_addr.s_addr = INADDR_ANY;
	local.sin_port        = 0;
	if (bind(bsock, (sockaddr*)&local, sizeof(local)) < 0) {
		printf("[player] bind() failed: %s\n", strerror(errno));
		mc_closesocket(bsock);
		return;
	}
	if (connect(bsock, (sockaddr*)&_serverAddr, sizeof(_serverAddr)) < 0) {
		// Not fatal â€” just means we do explicit mc_sendto() fallbacks below.
		// But on a healthy setup connect() to a UDP socket always succeeds,
		// so log and continue.
		printf("[player] connect() warning: %s\n", strerror(errno));
	}
	_sock = bsock;

	// 100ms receive timeout so we can interleave HELO keepalives and
	// still shut down promptly.
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 100000;
	mc_setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	int64_t lastHeloUs = 0;
	uint8_t buf[2048];

	while (_active.load(std::memory_order_relaxed))
	{
		// Send HELO every ~1 second (subscriber TTL on the server is 5s).
		int64_t now = nowUs();
		if (now - lastHeloUs > 900000) {
			const char helo[4] = { 'H', 'E', 'L', 'O' };
			ssize_t sent = mc_send(_sock, helo, 4, 0);
			if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
				// Non-fatal â€” server may be momentarily unreachable.
				static int64_t lastLogUs = 0;
				if (now - lastLogUs > 5000000) {
					printf("[player] HELO send failed: %s\n", strerror(errno));
					lastLogUs = now;
				}
			}
			lastHeloUs = now;
		}

		ssize_t n = mc_recv(_sock, buf, sizeof(buf), 0);
		if (n < 0) {
			// Recv timeout (SO_RCVTIMEO, now correctly honored on Windows) or a
			// transient error. errno is NOT a reliable Winsock indicator — a
			// timeout leaves errno=0 ("No error"), which previously fell through
			// to break and KILLED the tape rx thread on the first idle 100ms
			// window (before any tape subscription established). Just retry; the
			// _active flag + HELO pacing bound this loop, and shutdown()
			// ::shutdown()s the socket so we exit promptly.
			continue;
		}
		if (n < 8) continue;

		// Parse INPT envelope
		if (!(buf[0] == 'I' && buf[1] == 'N' && buf[2] == 'P' && buf[3] == 'T')) continue;
		uint8_t version = buf[4];
		uint8_t count   = buf[5];
		if (version != 1) continue;

		const size_t expected = 8 + (size_t)count * sizeof(maplecast_input::TapeEntry);
		if ((size_t)n < expected) continue;

		_packetsReceived.fetch_add(1, std::memory_order_relaxed);
		_lastPacketArrivalUs.store(now, std::memory_order_relaxed);
		_connected.store(true, std::memory_order_relaxed);

		const maplecast_input::TapeEntry* entries =
			reinterpret_cast<const maplecast_input::TapeEntry*>(buf + 8);

		uint64_t highestFrame = _serverLatestFrame.load(std::memory_order_relaxed);
		for (uint8_t i = 0; i < count; i++) {
			const maplecast_input::TapeEntry& e = entries[i];
			uint32_t seq;
			uint8_t  slot;
			maplecast_input::unpackSeqSlot(e.seqAndSlot, seq, slot);
			if (slot > 1) continue;
			enqueueEntry(e, slot);
			_entriesReceived.fetch_add(1, std::memory_order_relaxed);
			if (e.frame > highestFrame) highestFrame = e.frame;
		}
		_serverLatestFrame.store(highestFrame, std::memory_order_relaxed);
	}

	mc_closesocket(_sock);
	_sock = -1;
	printf("[player] rx thread stopped\n");
}

// Snapshot the locally-written SDL gamepad state and send it to the
// server as a 7-byte PC packet if anything changed since the last send.
// MUST be called BEFORE the tape overwrites kcode[0] in frameGate(),
// otherwise we'd be sending the server's own state back to it in a loop.
//
// kcode[] on this side is active-low in its lower 16 bits with the
// upper 16 bits masked to 1s by the SDL path. We strip the high mask
// before sending (matching the server's wire format, which only ships
// the low 16 bits).
static void forwardLocalInput()
{
	if (_fwdSock < 0) return;

	// Read the RAW LOCAL STICK from the redirect buffers, NOT kcode[0]. kcode[0]
	// is the SIM's input = the tape injection; reading it here would forward the
	// server's own echo back (a loop) and — critically — the local stick must
	// NOT be in kcode[0] at all (that was the double-application bug). The
	// gamepad path writes the live stick into g_localKcode[] when the redirect
	// is active; when it's not (shouldn't happen for a player client), fall back
	// to kcode so behaviour is unchanged.
	const bool redir = localInputRedirectActive();
	const uint32_t rawKcode = redir ? g_localKcode[0] : kcode[0];
	const uint16_t buttons  = (uint16_t)(rawKcode & 0xFFFF);
	const uint8_t  ltVal    = (uint8_t)((redir ? g_localLt[0] : lt[0]) >> 8);
	const uint8_t  rtVal    = (uint8_t)((redir ? g_localRt[0] : rt[0]) >> 8);

	if (_fwdHasFirst && buttons == _fwdLastButtons
	                 && ltVal   == _fwdLastLt
	                 && rtVal   == _fwdLastRt)
		return;   // no change, nothing to forward

	_fwdLastButtons = buttons;
	_fwdLastLt      = ltVal;
	_fwdLastRt      = rtVal;
	_fwdHasFirst    = true;

	// STAGE c — frame-stamped input: when the predict loop is running, stamp the
	// forwarded input with the target landing frame F = predictedFrame + INPUT_DELAY
	// so the server applies it AT F (the same frame the client predicted it at) —
	// eliminating self-mispredict. 15-byte "PC" variant:
	//   "PC"[slot][LT][RT][btn_hi][btn_lo][frame:u64_LE]
	// Falls back to the 7-byte packet when predict is off (default client untouched).
	const uint64_t predF = maplecast_predict::active() ? maplecast_predict::predictedFrame() : 0;
	if (predF != 0) {
		uint8_t pkt[15];
		pkt[0] = 'P'; pkt[1] = 'C'; pkt[2] = (uint8_t)_fwdClaimedSlot;
		pkt[3] = ltVal; pkt[4] = rtVal;
		pkt[5] = (uint8_t)(buttons >> 8); pkt[6] = (uint8_t)(buttons & 0xFF);
		const uint64_t F = predF + maplecast_predict::INPUT_DELAY;
		for (int i = 0; i < 8; i++) pkt[7 + i] = (uint8_t)((F >> (8 * i)) & 0xFF);   // LE
		ssize_t sent = mc_sendto(_fwdSock, pkt, sizeof(pkt), 0,
		                      (const sockaddr*)&_fwdInputAddr, sizeof(_fwdInputAddr));
		if (sent == (ssize_t)sizeof(pkt))
			_fwdPacketsSent.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	// 7-byte PC packet: "PC"[slot][LT][RT][btn_hi][btn_lo]
	uint8_t pkt[7];
	pkt[0] = 'P';
	pkt[1] = 'C';
	pkt[2] = (uint8_t)_fwdClaimedSlot;
	pkt[3] = ltVal;
	pkt[4] = rtVal;
	pkt[5] = (uint8_t)(buttons >> 8);
	pkt[6] = (uint8_t)(buttons & 0xFF);

	ssize_t sent = mc_sendto(_fwdSock, pkt, sizeof(pkt), 0,
	                      (const sockaddr*)&_fwdInputAddr, sizeof(_fwdInputAddr));
	if (sent == (ssize_t)sizeof(pkt))
		_fwdPacketsSent.fetch_add(1, std::memory_order_relaxed);
}

// Apply a single entry to the kcode[]/lt[]/rt[] globals for its slot.
static void applyEntry(const maplecast_input::TapeEntry& e, uint8_t slot)
{
	if (slot > 1) return;
	kcode[slot] = e.buttons | 0xFFFF0000u;   // active-low, upper 16 bits set
	lt[slot]    = (uint16_t)e.lt << 8;
	rt[slot]    = (uint16_t)e.rt << 8;
	_entriesApplied.fetch_add(1, std::memory_order_relaxed);
	if (e.frame > _lastAppliedFrame.load(std::memory_order_relaxed))
		_lastAppliedFrame.store(e.frame, std::memory_order_relaxed);
}

bool init()
{
	if (_active.load()) return true;

	const char* spec = std::getenv("MAPLECAST_PLAYER_CLIENT");
	if (!spec || !*spec) return false;

	// Make stdout LINE-buffered for the whole lockstep client so PLAYOUT /
	// underrun / checksum telemetry is visible LIVE in a redirected log. A
	// redirected stdout is block-buffered by default (stays 0 bytes until a 4 KB
	// buffer fills or the process exits) — which blinds all live diagnosis. Do
	// this unconditionally when lockstep is active (not just under a DEBUG env),
	// since the user's real launcher may not set it. Line-buffered flushes on
	// every '\n' with far less overhead than fully-unbuffered.
	if (maplecast_lockstep::active())
		setvbuf(stdout, nullptr, _IONBF, 0);

	// RATE DECOUPLING for the RENDERING lockstep client. The server is a 60fps
	// norend authority; the real GUI client does SH4 + native GL + sdl2 audio
	// and CANNOT finish a frame in 16.67ms, so single-threaded it runs BELOW
	// 60fps, falls progressively behind, overruns the tape buffer and freezes.
	// The SH4 (sim) must advance at the STREAM rate regardless of render/display
	// speed — render is a passive, droppable downstream consumer. This is
	// determinism-safe: the client is NOT the mirror server (isServer=false), so
	// the render path never writes guest-visible state (no serverPublish, no
	// framebuffer writeback) — verified by the offset-locked checksum staying
	// matched with threaded render ON.
	//   - ThreadedRendering OFF: threaded render deadlocks at the post-loadstate
	//     resume (emu+render thread wedge on the pvr queue — MEASURED: client
	//     froze at the JOIN frame, fps=0). Single-threaded resumes cleanly, so
	//     the SH4 runs synchronously with the render. Rate is instead held by
	//     the lag-driven render-skip below.
	//   - EmulateFramebuffer OFF: never write the rendered framebuffer back into
	//     guest VRAM (the norend server never does).
	// RATE FIX (frameGate): when the client lags the live tape, DROP the draw
	// (rend_enable_renderer(false)) so the SH4 runs at full norend speed and
	// catches up; re-enable near the live edge. scheduleRenderDone fires before
	// the skip (Renderer_if.cpp:600) so SH4 timing is identical — sim stays
	// deterministic, only pixels drop. This keeps the sim at the 60fps stream
	// rate with the render dropping frames when it can't keep up.
	if (maplecast_lockstep::active()) {
		// ThreadedRendering ON: the emu thread advances the SH4 at a steady 60fps
		// (playout-buffer paced) INDEPENDENT of render/display speed — render runs
		// on the main thread and presents async, dropping frames when it can't
		// keep up. Single-threaded chained the sim to the ~35fps render and lost
		// to the 60fps server (buffer drained). The post-loadstate resume race
		// that froze earlier threaded builds is fixed by rend_start_rollback()
		// around the JOIN load (maplecast_state_sync::clientApplyPending — GGPO's
		// proven threaded-loadstate sync). Determinism holds because the client
		// render is read-only (isServer=false) — re-verified by the offset-locked
		// checksum matching with threaded render ON.
		// ThreadedRendering ON: the emu thread advances the SH4 + input-tape
		// replay at a STEADY 60fps (playout-paced) INDEPENDENT of render/present
		// jitter; the render runs on the main (Flycast-rend) thread and presents
		// async. Single-threaded, the sim was chained to the render/present/audio
		// loop, so per-frame jitter hitched BOTH video (spikes) AND input replay
		// (stuck inputs). The post-loadstate resume race is handled by
		// rend_start_rollback() around the JOIN load (c_everSynced-guarded) in
		// maplecast_state_sync::clientApplyPending. Render is fast now (the 30ms
		// was rend.DumpTextures=yes, since fixed), so renderEnd.Wait() returns
		// quickly and the pipeline holds 60fps. Determinism holds (client render
		// is read-only, isServer=false) — verified by the offset-locked checksum.
		// Single-threaded (the threaded GUI build crashes at boot). With a FAST
		// render this holds 60fps: sim ~9ms/frame (SH4 + native-480p GL + no-vsync
		// present) runs faster than realtime, so the audio backend paces it to
		// 60fps (audiostream.cpp:63); catch-up mutes audio (fastForwardMode) so
		// the dynarec runs ~110fps and drains a backlog.
		config::ThreadedRendering.override(false);
		config::EmulateFramebuffer.override(false);
		// FORCE render fast regardless of the loaded emu.cfg:
		//  - DumpTextures OFF: writing every texture to disk each frame was the
		//    ~30ms/frame (=> sim <60fps => the client fell 170s behind). MUST be
		//    off or the sim can never keep the live edge.
		//  - VSync OFF: present() must not block on the display vblank.
		//  - Native 480p: don't upscale on a client that must keep pace.
		config::DumpTextures.override(false);
		config::VSync.override(false);
		config::RenderResolution.override(480);
		// Steady-state 60fps pacing comes from the real audio backend (push()
		// blocks to wall-clock 44.1kHz, audiostream.cpp:63) with real sound; the
		// one-time post-JOIN catch-up mutes audio via fastForwardMode
		// (sgc_if.cpp:1599). frameGate holds the playout buffer.
		printf("[player] lockstep playout: ThreadedRendering=%d EmulateFramebuffer=%d "
		       "RenderResolution=%d Sh4Clock=%d (emu-thread 60fps sim, async render)\n",
		       (int)config::ThreadedRendering, (int)config::EmulateFramebuffer,
		       (int)config::RenderResolution, (int)config::Sh4Clock);
	}

	if (!resolveServer(spec)) return false;

	// Parse stall policy env var.
	if (const char* p = std::getenv("MAPLECAST_PLAYER_STALL_POLICY")) {
		if (!strcasecmp(p, "speculate"))
			_stallPolicy.store((uint8_t)StallPolicy::Speculate);
		else if (!strcasecmp(p, "hard"))
			_stallPolicy.store((uint8_t)StallPolicy::Hard);
		else
			printf("[player] unknown MAPLECAST_PLAYER_STALL_POLICY='%s' "
			       "(expected 'hard' or 'speculate')\n", p);
	}
	printf("[player] stall policy = %s\n",
	       _stallPolicy.load() == (uint8_t)StallPolicy::Hard ? "hard" : "speculate");

	// Phase 4-lite: set up the local-input forwarding socket. Reuse the
	// tape server's resolved address but override the port to 7100 (the
	// MapleCast input server port on the same host). The slot we claim
	// defaults to 0, overridable with MAPLECAST_PLAYER_SLOT.
	_fwdInputAddr      = _serverAddr;
	_fwdInputAddr.sin_port = htons(7100);
	if (const char* slotEnv = std::getenv("MAPLECAST_PLAYER_SLOT")) {
		int s = std::atoi(slotEnv);
		if (s < 0) s = 0;
		if (s > 1) s = 1;
		_fwdClaimedSlot = s;
	}
	_fwdSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (_fwdSock < 0) {
		printf("[player] warning: fwd socket failed: %s (local input disabled)\n",
		       strerror(errno));
	} else {
		printf("[player] local input forwarding -> port 7100 as slot %d\n",
		       _fwdClaimedSlot);
	}

	// Drive the SIM purely from the tape: redirect the local stick OUT of the
	// sim's kcode[] (into g_localKcode[]) so it can't leak/double-apply. Only
	// forwardLocalInput() reads the local stick, sending it to the server.
	_redirectLocal.store(true, std::memory_order_relaxed);

	_active.store(true);
	_rxThread = std::thread(rxLoop);

	// Start the state-sync TCP client. Reconnects automatically on
	// failure so it's safe to call before the server is necessarily
	// listening. Until the first STAT envelope arrives and is applied,
	// frameGate stalls the SH4 unconditionally (see _initialSynced).
	if (!maplecast_state_sync::clientStart(_serverHost.c_str())) {
		printf("[player] warning: state-sync clientStart failed â€” SH4 will stall "
		       "until a state arrives\n");
	}

	// Lockstep-mirror checksum layer (env-gated MAPLECAST_LOCKSTEP=1, default
	// OFF). Subscribes to the server's game-state-hash channel (UDP 7103) so
	// frameGate can verify per-frame parity + resync on divergence. No-op
	// unless enabled — the player client is unchanged when lockstep is off.
	if (maplecast_lockstep::active())
		maplecast_lockstep::clientInit(_serverHost.c_str());

	printf("[player] === PLAYER CLIENT MODE ENABLED ===\n");
	return true;
}

void shutdown()
{
	if (!_active.load()) return;
	_active.store(false);
	if (maplecast_lockstep::active())
		maplecast_lockstep::clientShutdown();
	maplecast_state_sync::clientStop();
	if (_fwdSock >= 0) {
		mc_closesocket(_fwdSock);
		_fwdSock = -1;
	}
	if (_sock >= 0) {
		// Kick the rx thread out of its mc_recv() via shutdown() â€” close()
		// happens inside the thread itself.
		::shutdown(_sock, SHUT_RDWR);
	}
	if (_rxThread.joinable()) _rxThread.join();
	for (int s = 0; s < 2; s++) {
		std::lock_guard<std::mutex> lock(_queues[s].mu);
		_queues[s].entries.clear();
	}
	printf("[player] shutdown\n");
}

bool active() { return _active.load(std::memory_order_relaxed); }

StallPolicy getStallPolicy()
{
	return (StallPolicy)_stallPolicy.load(std::memory_order_relaxed);
}

void setStallPolicy(StallPolicy p)
{
	_stallPolicy.store((uint8_t)p, std::memory_order_relaxed);
}

void seedLocalFrame(uint64_t frame)
{
	_localFrame.store(frame, std::memory_order_relaxed);
}

void requestResync()
{
	// Re-arm the one-shot initial-state apply so frameGate's !_initialSynced
	// block runs clientApplyPending again (which dc_deserialize's the fresh
	// state + reseeds _localFrame via seedLocalFrame). Bounce the state-sync
	// TCP so the server's accept path sets needsInitialSync=true and ships a
	// brand-new snapshot.
	_initialSynced.store(false, std::memory_order_relaxed);
	maplecast_state_sync::clientStop();
	maplecast_state_sync::clientStart(_serverHost.c_str());
	printf("[player] resync requested — bounced state-sync, awaiting fresh JOIN\n");
}


// Send a 15-byte frame-stamped local-input packet via the PROVEN socket/dest
// (same _fwdSock/_fwdInputAddr the 7-byte lockstep forward delivers through).
static void lpForwardStamped(const LpInput& in, int slot, uint64_t stampF)
{
	if (_fwdSock < 0) return;
	uint8_t pkt[15];
	pkt[0] = 'P'; pkt[1] = 'C'; pkt[2] = (uint8_t)slot;
	pkt[3] = in.lt; pkt[4] = in.rt;
	pkt[5] = (uint8_t)(in.btn >> 8); pkt[6] = (uint8_t)(in.btn & 0xFF);
	for (int i = 0; i < 8; i++) pkt[7 + i] = (uint8_t)((stampF >> (8 * i)) & 0xFF);
	mc_sendto(_fwdSock, pkt, sizeof(pkt), 0, (const sockaddr*)&_fwdInputAddr, sizeof(_fwdInputAddr));
}

// ── CAPSTONE — the live predict-drive tick (MAPLECAST_PREDICT_LIVE=1) ────────
// Returns true (advance the head one game frame — Emulator::run renders it) or
// false (transient: not ready). Ingests authoritative tape, reconciles (rollback
// on mispredict via the page-delta ring), then predicts the head with the local
// player's live input NOW + repeat-last-confirmed remote. The confirmed timeline
// stays server-authoritative; clientVerify (called above in frameGate) is the
// determinism gate. Only reached when maplecast_predict::liveActive().
static bool livePredictDrive(uint64_t localFrame)
{
	const int ls = _fwdClaimedSlot & 1;   // local slot
	const int rs = ls ^ 1;                // remote slot
	const uint64_t P = localFrame;        // predicted head (about to run frame P)

	// APPLY-PHASE FIX (STEP 2): 1-frame input pipeline depth. The client's
	// headless advance consumes the just-set kcode one game-frame earlier than the
	// server's Emulator::run; PH==1 delays the applied input by one logical frame
	// so the client head reacts on the SAME game-frame as the server. Env-tunable
	// (MAPLECAST_PREDICT_PHASE=0 disables, for A/B verification). Applied in BOTH
	// the rollback re-sim and the head catch-up so the two timelines agree
	// (rollbacks stay bounded).
	static int PH = -2;
	if (PH == -2) { const char* pe = std::getenv("MAPLECAST_PREDICT_PHASE"); PH = pe ? atoi(pe) : 0; }   // default 0: the runInternal boundary fix obsoleted the old phase-1 hack

	if (!_lpInited) {
		if (!maplecast_predict::liveInit()) return false;
		_lpConfirmed = (P == 0) ? 0 : P - 1;
		_lpLastConfRemote = LpInput{};
		_lpInited = true;
		_lpArmHead = P;
		printf("[predict-live] drive ARMED: localSlot=%d confirmed=%llu head=%llu\n",
		       ls, (unsigned long long)_lpConfirmed, (unsigned long long)P);
	}

	// Per-frame perf probe (MAPLECAST_PREDICT_PERF=1): time the whole predict drive (ingest +
	// rollback re-sim + head catch-up) and capture the re-sim depth this frame, so we can see
	// the rollback-burst compute vs the 16.6ms budget (the jitter).
	static const bool _perfLog = std::getenv("MAPLECAST_PREDICT_PERF") != nullptr;
	const auto _perfT0 = std::chrono::steady_clock::now();
	uint64_t _perfDepth = 0;

	// 1. INGEST authoritative tape (both slots) — ALL received frames < head P
	//    (the head leads the server, so many tape frames sit between confirmed and P).
	for (int slot = 0; slot < 2; slot++) {
		PendingQueue& q = _queues[slot];
		std::lock_guard<std::mutex> lock(q.mu);
		if (!q.entries.empty() && q.entries.back().frame > _lpTapeNewest)
			_lpTapeNewest = q.entries.back().frame;   // newest received (live-edge proxy)
		while (!q.entries.empty()) {
			const auto& e = q.entries.front();
			if (e.frame <= _lpConfirmed) { q.entries.pop_front(); continue; }  // stale
			if (e.frame >= P) break;                                          // not yet run by head
			const int idx = (int)(e.frame % LP_RING);
			_lpAuth[slot][idx] = LpInput{ e.buttons, e.lt, e.rt };
			_lpAuthFrame[slot][idx] = e.frame;
			q.entries.pop_front();
		}
	}

	// 1b. Capture the just-rendered head's per-frame hash for frame P-1. The
	//     confHash gate below only compares a frame that was NOT rolled back this
	//     tick (reSimFrom==UINT64_MAX && _lpLiveHashFrame==N): for such a frame the
	//     head's predicted input MATCHED the authoritative tape, so the head state
	//     == the confirmed (tape-replayed) state — a valid confirmed-vs-server
	//     sample. Rolled-back frames overwrite this with the re-sim hash below.
	//     Without this, the gate only sees re-simmed frames (sparse/absent once the
	//     phase fix suppresses rollbacks) — this keeps the gate densely populated.
	if (P > 0) {
		const uint64_t hf = P - 1;
		const int hi = (int)(hf % LP_RING);
		_lpLiveHash[hi] = maplecast_rollback::gameStateRegionHash();
		_lpLiveHashFrame[hi] = hf;
		if (std::getenv("MAPLECAST_SUBHASH_LOG"))
			maplecast_rollback::gameStateSubHashes(_lpLiveSub[hi]);
	}

	// 2. RECONCILE: advance confirmed while both slots' authoritative for N present;
	//    note the earliest mispredicted frame. (Bound by head P.)
	uint64_t reSimFrom = UINT64_MAX;
	for (uint64_t N = _lpConfirmed + 1; N < P; N++) {
		const int idx = (int)(N % LP_RING);
		if (_lpAuthFrame[ls][idx] != N || _lpAuthFrame[rs][idx] != N) break;   // not both yet
		// Reconcile BOTH slots against the authoritative tape so the confirmed
		// timeline is an EXACT replay of the server's inputs => confirmed==server
		// GUARANTEED. (Trust-own-input silently masked local-slot frame-stamp
		// misalignment: the server applied our input at a frame where we'd predicted
		// a different value, and trust-own never reconciled it -> permanent state
		// divergence under input. Reconciling the local slot too fires a small
		// rollback that corrects it; the head keeps our live input for instant feel.)
		// FULL input comparison (btn + lt + rt) — the sim applies analog triggers
		// too, so a trigger-only prediction miss must also trigger a rollback (was
		// btn-only: a latent wrong-state-no-rollback hole).
		auto inSame = [](const LpInput& a, const LpInput& b) {
			return a.btn == b.btn && a.lt == b.lt && a.rt == b.rt;
		};
		const bool matchL = inSame(_lpAuth[ls][idx], _lpPred[ls][idx]);
		const bool matchR = inSame(_lpAuth[rs][idx], _lpPred[rs][idx]);
		if (!(matchL && matchR) && reSimFrom == UINT64_MAX) {
			reSimFrom = N;
			// DIAGNOSTIC (log-only): pin whether the LOCAL (frame-stamped) or REMOTE
			// (repeat-last-confirmed) slot drove the mispredict, and by what input
			// delta — local miss => stamp timing/phase skew; remote miss => repeat-last
			// gap. Rate-limited so it never floods the frame loop.
			static uint64_t _misLog = 0;
			if ((_misLog++ % 30) == 0) {
				printf("[predict-mis] f=%llu Lmiss=%d Rmiss=%d | predL=%04x/%02x/%02x authL=%04x/%02x/%02x | predR=%04x authR=%04x\n",
				       (unsigned long long)N, (int)!matchL, (int)!matchR,
				       _lpPred[ls][idx].btn, _lpPred[ls][idx].lt, _lpPred[ls][idx].rt,
				       _lpAuth[ls][idx].btn, _lpAuth[ls][idx].lt, _lpAuth[ls][idx].rt,
				       _lpPred[rs][idx].btn, _lpAuth[rs][idx].btn);
				fflush(stdout);
			}
		}
		// INJECTION verify: did the server echo our (injected) local input, and did
		// our prediction match it? Proves the forward+stamp+echo path end-to-end.
		if (_lpInjectStart && N >= _lpInjectStart && N < _lpInjectStart + 180) {
			if (_lpAuth[ls][idx].btn == 0xFFBF) _lpInjEchoed++;
			if (_lpAuth[ls][idx].btn == _lpPred[ls][idx].btn) _lpInjMatched++;
			_lpInjConfirmed++;
		}
		_lpLastConfRemote = _lpAuth[rs][idx];
		_lpConfirmed = N;
		// CONFIRMED-HASH GATE: if this frame was NOT rolled back (its head hash is
		// still valid) compare our end-of-N state hash to the server's authoritative
		// hash. Test 3 label offsets (-1/0/+1) to pin the true alignment (idle masks
		// a 1-frame skew; injection exposes it). Whichever offset has mismatch==0 is
		// the correct one — that's the real determinism proof under input.
		if (reSimFrom == UINT64_MAX && _lpLiveHashFrame[idx] == N) {
			uint64_t sh;
			for (int k = 0; k < 3; k++) {
				const int64_t sfN = (int64_t)N + (k - 1);   // N-1, N, N+1
				if (sfN >= 0 && maplecast_lockstep::serverHashForClientFrame((uint64_t)sfN, &sh)) {
					if (_lpLiveHash[idx] == sh) _lpConfM[k]++; else _lpConfMM[k]++;
				}
			}
			// LATCH LOCALIZER: log the client head's raw controller latch for a WINDOW
			// of CONSECUTIVE confirmed frames near injection start, so it can be aligned
			// against [LATCH-SRV] f=N to pin the exact apply-phase off-by-one direction.
			if (std::getenv("MAPLECAST_SUBHASH_LOG") && _lpInjectStart &&
			    N >= _lpInjectStart && N < _lpInjectStart + 40) {
				const uint64_t* s = _lpLiveSub[idx];
				printf("[LATCH-CLI] f=%llu latch=%016llx chars=%016llx\n",
				       (unsigned long long)N,(unsigned long long)s[4],(unsigned long long)s[0]);
				fflush(stdout);
			}
		}
	}

	// 3. ROLLBACK + RE-SIM from the earliest mispredict to the head (invisible).
	if (reSimFrom != UINT64_MAX) {
		if (!maplecast_predict::ringHas(reSimFrom)) {
			// Beyond the ring — can't reproduce. Fall back to a full re-JOIN.
			_lpReJoins++;
			printf("[predict-live] mispredict@%llu beyond ring (oldest=%llu) -> re-JOIN\n",
			       (unsigned long long)reSimFrom, (unsigned long long)maplecast_predict::ringOldest());
			_lpInited = false;
			requestResync();
			return false;
		}
		const uint64_t depth = (P > reSimFrom) ? (P - reSimFrom) : 0;
		if (depth > _lpMaxDepth) _lpMaxDepth = depth;
		_perfDepth = depth;
		_lpRollbacks++;
		// ROLLBACK CLASSIFIER: snapshot the head's PRE-rollback (predicted) state hash.
		// Step 1b captured _lpLiveHash[P-1] at the top of this drive call, so this is the
		// predicted head state; after the re-sim we compare to decide spurious vs real.
		const int _rbHeadIdx = (int)((P - 1) % LP_RING);
		const bool _rbHaveHead = (P >= 1 && _lpLiveHashFrame[_rbHeadIdx] == P - 1);
		const uint64_t _rbOldHead = _rbHaveHead ? _lpLiveHash[_rbHeadIdx] : 0;
		maplecast_predict::ringRestore(reSimFrom);
		// APPLY-PHASE FIX (STEP 2): the client's headless advance
		// (runOneGameFrameHeadless, loops to the 0x3496B0 game-frame tick) consumes
		// the just-set kcode ONE game-frame EARLIER than the server's Emulator::run
		// (which runs to present()). This latent 1-frame latch->act skew is
		// phase-INVARIANT under a held input (why GATE 0 passed) but SEEDS a
		// compounding char divergence under CHANGING input (the 34/1065 confHash
		// mismatch). Reproduce the server's phase by applying the PREVIOUS logical
		// frame's input to kcode (an in-order 1-frame input pipeline). The loop
		// restarts fresh each drive call from the ring, so the pipeline is
		// rollback-safe. PH default 1 (delay); env-tunable for A/B verification.
		// Prime the pipeline from the logical input of frame (reSimFrom-1) — a
		// confirmed frame whose authoritative tape is still resident in the ring.
		LpInput pipeL, pipeR;
		{
			const uint64_t g = reSimFrom - 1;
			const int gi = (int)(g % LP_RING);
			pipeL = (_lpAuthFrame[ls][gi] == g) ? _lpAuth[ls][gi] : _lpPred[ls][gi];
			pipeR = (_lpAuthFrame[rs][gi] == g) ? _lpAuth[rs][gi] : _lpLastConfRemote;
		}
		for (uint64_t f = reSimFrom; f < P; f++) {
			const int idx = (int)(f % LP_RING);
			// Logical input for THIS frame (drives mispredict bookkeeping + the
			// next iteration's pipeline). AUTHORITATIVE tape for confirmed frames
			// (exact server replay); future frames use predicted local + repeat-
			// last-confirmed remote.
			LpInput logL, logR;
			if (_lpAuthFrame[ls][idx] == f) { logL = _lpAuth[ls][idx]; _lpPred[ls][idx] = logL; }
			else                              logL = _lpPred[ls][idx];
			if (_lpAuthFrame[rs][idx] == f) { logR = _lpAuth[rs][idx]; _lpPred[rs][idx] = logR; }
			else                            { logR = _lpLastConfRemote; _lpPred[rs][idx] = logR; }
			// APPLIED input = phase-delayed (previous logical frame) when PH==1, so
			// the headless sim reacts on the SAME game-frame the server did.
			const LpInput& appL = (PH == 1) ? pipeL : logL;
			const LpInput& appR = (PH == 1) ? pipeR : logR;
			kcode[ls] = (uint32_t)appL.btn | 0xFFFF0000u;
			lt[ls] = (uint16_t)appL.lt << 8; rt[ls] = (uint16_t)appL.rt << 8;
			kcode[rs] = (uint32_t)appR.btn | 0xFFFF0000u;
			lt[rs] = (uint16_t)appR.lt << 8; rt[rs] = (uint16_t)appR.rt << 8;
			pipeL = logL; pipeR = logR;
			maplecast_predict::advanceHeadlessOneFrame();
			maplecast_predict::ringSave(f + 1);
			// Update the per-frame hash to the RE-SIMMED (post-rollback) state so the
			// confirmed-hash gate compares the CONFIRMED state to the server — not a
			// stale speculative head hash. This makes confHash the decisive
			// cross-instance test (confirmed re-sim frame f vs server hash f).
			_lpLiveHash[idx] = maplecast_rollback::gameStateRegionHash();
			_lpLiveHashFrame[idx] = f;
			if (std::getenv("MAPLECAST_SUBHASH_LOG"))
				maplecast_rollback::gameStateSubHashes(_lpLiveSub[idx]);
		}
		// ROLLBACK CLASSIFIER: the re-sim rewrote _lpLiveHash[P-1] to the re-simmed head
		// state. Equal to the pre-rollback head hash => the rollback changed nothing on
		// screen (SPURIOUS, wasted ~4ms/frame); different => a REAL correction.
		if (_rbHaveHead && _lpLiveHashFrame[_rbHeadIdx] == P - 1) {
			if (_lpLiveHash[_rbHeadIdx] == _rbOldHead) _lpRbSpurious++;
			else                                       _lpRbReal++;
		}
		// Continue the head's input pipeline from the re-sim's final logical frame
		// (P-1) so the head phase joins the confirmed timeline seamlessly (no
		// spurious mispredict at the re-sim/head seam => rollbacks stay bounded).
		_lpHeadPipe = pipeL;
	}

	// ── RUN-AHEAD: drive the head to LEAD the server's live edge by INPUT_DELAY ──
	// serverLive estimate: lockstep serverFrame (updated every hashInterval),
	// extrapolated by head-ticks since its last update, floored by the newest tape
	// frame received. The head then leads the server so local input applied at the
	// head is INSTANT (>= live edge) and, stamped for the head frame, lands on the
	// server in the FUTURE => applied on time => confirmed matches server.
	// Anchor the head to the TAPE (which is server-numbered) with a FIXED lead, so
	// the head frame numbering == the server's and never drifts. LP_LEAD must exceed
	// the tape-delivery latency so the head leads the server's TRUE current frame
	// (newest_tape = server_current - latency; head = newest_tape + LP_LEAD leads by
	// LP_LEAD - latency). The stamp == head == a future server frame => the server
	// applies our input on time and the client applied the same value there.
	// LP_LEAD env-configurable (MAPLECAST_PREDICT_LEAD): the run-ahead lead in frames. Must
	// exceed the tape-delivery latency (network RTT in frames). On a ~0-latency LOCAL server 16
	// is huge overkill -> the head runs 16 frames ahead of a current server -> ~16-deep rollback
	// on EVERY mispredict for no benefit -> jitter. Use 1-2 locally. Clamped to [1, 26].
	static const uint64_t LP_LEAD = [](){ const char* e = std::getenv("MAPLECAST_PREDICT_LEAD");
		uint64_t v = e ? (uint64_t)strtoull(e, nullptr, 10) : 16; return v < 1 ? 1 : (v > 26 ? 26 : v); }();
	// MAX_LEAD env-configurable (MAPLECAST_PREDICT_MAXLEAD): caps the head-confirmed gap, which
	// bounds the ROLLBACK RE-SIM DEPTH (depth <= MAX_LEAD-1). Small (e.g. 3 -> depth<=2) keeps each
	// rollback cheap (1-2 SH-4 ticks, fits the 16ms frame) at the cost of less run-ahead. Must
	// exceed the network RTT in frames for a REMOTE server; on a 0-latency LOCAL server, 3 is ideal.
	static const uint64_t MAX_LEAD = [](){ const char* e = std::getenv("MAPLECAST_PREDICT_MAXLEAD");
		uint64_t v = e ? (uint64_t)strtoull(e, nullptr, 10) : 26; return v < 2 ? 2 : (v > 30 ? 30 : v); }();  // < ring DEPTH (32)
	uint64_t serverLive = maplecast_lockstep::lastServerFrame();   // telemetry only
	if (_lpTapeNewest > serverLive) serverLive = _lpTapeNewest;
	uint64_t target = _lpTapeNewest + LP_LEAD;
	if (target > _lpConfirmed + MAX_LEAD) {                // fell too far behind confirm
		target = _lpConfirmed + MAX_LEAD;
		if (P > _lpConfirmed + 28) {                       // beyond ring -> re-JOIN
			_lpReJoins++;
			printf("[predict-live] head-confirmed gap %llu beyond ring -> re-JOIN\n",
			       (unsigned long long)(P - _lpConfirmed));
			_lpInited = false; requestResync(); return false;
		}
	}
	if (target < P) target = P;                            // never go backward

	// Sample the local stick NOW and forward it stamped for the HEAD frame `target`
	// (a FUTURE server frame => the server applies it exactly there = on time, and
	// the client applies the same value at its head => self-mispredict stays 0).
	LpInput cur{ (uint16_t)(g_localKcode[ls] & 0xFFFF),
	             (uint8_t)(g_localLt[ls] >> 8), (uint8_t)(g_localRt[ls] >> 8) };
	lpForwardStamped(cur, ls, target);

	// Advance the head from P up to `target` (headless predict: local=cur +
	// remote=repeat-last-confirmed), rendering only the final frame. Steady state
	// after the initial jump: target == P => 0 headless frames, one rendered frame.
	// APPLY-PHASE FIX (STEP 2): the head uses the SAME 1-frame input pipeline as
	// the re-sim. logical input for frame f is `cur` (local) / last-confirmed
	// (remote); the APPLIED input is the previous logical frame's (PH==1) so the
	// head reacts on the same game-frame as the server. _lpHeadPipe carries the
	// pipeline across drive calls (steady state = one rendered frame/call).
	LpInput headPipeL = _lpHeadPipe;
	LpInput headPipeR = _lpLastConfRemote;   // remote is a held repeat => delay is a no-op
	auto applyHead = [&](uint64_t f) {
		const int idx = (int)(f % LP_RING);
		const LpInput& appL = (PH == 1) ? headPipeL : cur;
		const LpInput& appR = (PH == 1) ? headPipeR : _lpLastConfRemote;
		kcode[ls] = (uint32_t)appL.btn | 0xFFFF0000u; lt[ls]=(uint16_t)appL.lt<<8; rt[ls]=(uint16_t)appL.rt<<8;
		kcode[rs] = (uint32_t)appR.btn | 0xFFFF0000u; lt[rs]=(uint16_t)appR.lt<<8; rt[rs]=(uint16_t)appR.rt<<8;
		_lpPred[ls][idx] = cur; _lpPred[rs][idx] = _lpLastConfRemote;
		headPipeL = cur; headPipeR = _lpLastConfRemote;
	};
	for (uint64_t f = P; f < target; f++) {                // catch-up (headless, invisible)
		applyHead(f);
		maplecast_predict::advanceHeadlessOneFrame();
		maplecast_predict::ringSave(f + 1);
	}
	applyHead(target);                                     // the head frame — rendered next
	_lpHeadPipe = cur;                                     // seed next call's pipeline

	// Telemetry: lead over server (must be POSITIVE ~INPUT_DELAY), gap/depth.
	const int64_t lead = (int64_t)target - (int64_t)serverLive;
	if ((uint64_t)(lead < 0 ? -lead : lead) != 0) { if ((uint64_t)llabs(lead) < _lpLeadMin) _lpLeadMin=(uint64_t)llabs(lead); }
	if ((uint64_t)llabs(lead) > _lpLeadMax) _lpLeadMax=(uint64_t)llabs(lead);
	const uint64_t gap = (target > _lpConfirmed) ? (target - _lpConfirmed) : 0;
	if (gap > _lpGapMax) _lpGapMax = gap;
	if ((target % 120) == 0)
		printf("[predict-live] tele head=%llu serverLive~%llu lead=%+lld confirmed=%llu gap=%llu "
		       "rollbacks=%llu spur=%llu real=%llu maxDepth=%llu reJoins=%llu confHash=%llu/%llu(match/mism)\n",
		       (unsigned long long)target, (unsigned long long)serverLive, (long long)lead,
		       (unsigned long long)_lpConfirmed, (unsigned long long)gap,
		       (unsigned long long)_lpRollbacks, (unsigned long long)_lpRbSpurious, (unsigned long long)_lpRbReal, (unsigned long long)_lpMaxDepth,
		       (unsigned long long)_lpReJoins,
		       (unsigned long long)_lpConfMatch, (unsigned long long)_lpConfMismatch),
		printf("[predict-live]   confHash-by-offset  N-1:%llu/%llu  N:%llu/%llu  N+1:%llu/%llu (match/mism)\n",
		       (unsigned long long)_lpConfM[0],(unsigned long long)_lpConfMM[0],
		       (unsigned long long)_lpConfM[1],(unsigned long long)_lpConfMM[1],
		       (unsigned long long)_lpConfM[2],(unsigned long long)_lpConfMM[2]);

	if (_perfLog) {
		const long long _dus = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - _perfT0).count();
		printf("[predict-perf] head=%llu drive=%lldus reSimDepth=%llu catchup=%llu rollbacks=%llu lead=%+lld gap=%llu%s\n",
		       (unsigned long long)target, _dus, (unsigned long long)_perfDepth,
		       (unsigned long long)(target > P ? target - P : 0), (unsigned long long)_lpRollbacks,
		       (long long)lead, (unsigned long long)gap, _dus > 16666 ? "  <<OVER-BUDGET" : "");
		fflush(stdout);
	}
	// Advance: Emulator::run renders the head frame `target` after we return true.
	_localFrame.store(target + 1, std::memory_order_relaxed);
	maplecast_predict::setPredictedFrame(target);
	return true;
}

bool frameGate()
{
	if (!_active.load(std::memory_order_relaxed)) return true;  // no-op path

	// Phase 4-lite: snapshot local SDL gamepad state and forward to the
	// server FIRST. The server echoes our inputs back through the tape
	// stamped with the authoritative frame number â€” that's the round-trip
	// that makes the local gamepad "feel real" from the SH4's perspective.
	// Injection test writes g_localKcode BEFORE forwardLocalInput reads it (mimics
	// the gamepad thread's timing). No-op unless MAPLECAST_PREDICT_LIVE_INJECT.
	if (maplecast_predict::liveActive())
		lpInjectTick(_localFrame.load(std::memory_order_relaxed), _fwdClaimedSlot & 1);

	// In predict-LIVE the drive forwards the local input itself, stamped for the
	// RUN-AHEAD head frame (a future server frame) — not predictedFrame()+DELAY.
	// So skip forwardLocalInput here to avoid a double / wrongly-stamped send.
	if (!maplecast_predict::liveActive())
		forwardLocalInput();

	// Phase 3 v2: ONE-SHOT initial state apply. Once _initialSynced is
	// true, clientApplyPending will return false on every subsequent call
	// because the server only sends a STAT envelope once per session.
	// The heavy emu.loadstate work happens exactly once at session start.
	if (!_initialSynced.load(std::memory_order_relaxed)) {
		if (maplecast_state_sync::clientApplyPending()) {
			const uint64_t newFrame = _localFrame.load(std::memory_order_relaxed);
			// Flush any tape entries from BEFORE the seeded frame â€”
			// they're stale relative to the new timeline. The dense
			// tape will start delivering entries at >= newFrame
			// momentarily.
			for (int slot = 0; slot < 2; slot++) {
				std::lock_guard<std::mutex> lock(_queues[slot].mu);
				while (!_queues[slot].entries.empty() &&
				       _queues[slot].entries.front().frame < newFrame)
					_queues[slot].entries.pop_front();
			}
			_initialSynced.store(true, std::memory_order_relaxed);
			// Diagnostic: seeded frame vs what the tape currently holds, so a
			// counter-scale mismatch (JOIN serverFrame vs tape stamps) or a
			// too-stale JOIN (seed older than the buffer) is visible at a
			// glance instead of manifesting as a silent stall.
			uint64_t t0o=0,t0n=0,t1o=0,t1n=0; size_t s0=0,s1=0;
			{
				std::lock_guard<std::mutex> l0(_queues[0].mu);
				s0 = _queues[0].entries.size();
				if (s0) { t0o=_queues[0].entries.front().frame; t0n=_queues[0].entries.back().frame; }
			}
			{
				std::lock_guard<std::mutex> l1(_queues[1].mu);
				s1 = _queues[1].entries.size();
				if (s1) { t1o=_queues[1].entries.front().frame; t1n=_queues[1].entries.back().frame; }
			}
			printf("[player] JOIN applied: seeded localFrame=%llu; tape slot0=[%llu..%llu]/%zu "
			       "slot1=[%llu..%llu]/%zu (seed should fall within tape range)\n",
			       (unsigned long long)newFrame,
			       (unsigned long long)t0o,(unsigned long long)t0n,s0,
			       (unsigned long long)t1o,(unsigned long long)t1n,s1);
		} else {
			// No state yet â€” block the SH4. We have no frame number
			// to apply tape entries against.
			_framesStalled.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
	}

	const uint64_t localFrame = _localFrame.load(std::memory_order_relaxed);
	const StallPolicy policy  = (StallPolicy)_stallPolicy.load(std::memory_order_relaxed);

	// Lockstep-mirror checksum (env-gated). We are about to run frame
	// `localFrame`, so resident guest RAM currently reflects the END of frame
	// (localFrame-1). clientVerify hashes that region and compares against the
	// server's hash for the matching frame, triggering a resync on divergence.
	// No-op unless MAPLECAST_LOCKSTEP=1.
	// In predict-LIVE the current state is the SPECULATIVE run-ahead HEAD (leads
	// the server by INPUT_DELAY), so clientVerify would compare the leading head to
	// the server's authoritative frame and false-mismatch -> resync storm. The
	// predict path's determinism gate is the reconcile (confirmed replays the tape;
	// see livePredictDrive), so skip the lagging-mirror checksum here.
	if (maplecast_lockstep::active() && !maplecast_predict::liveActive())
		maplecast_lockstep::clientVerify(localFrame);

	// Predict/rollback subsystem — STAGE 1 drives the no-render re-sim PRIMITIVE
	// gate here (SH4 is paused between frames). No-op unless MAPLECAST_PREDICT is
	// set. The full predict/ring/reconcile loop is a later stage.
	if (maplecast_predict::active())
		maplecast_predict::onFrameBoundary(localFrame);

	// ── CAPSTONE — live predict-drive (MAPLECAST_PREDICT_LIVE=1) ────────
	// Replaces the lockstep bothExact tape-wait: apply the LOCAL player's input
	// NOW at the predicted head + repeat-last-confirmed remote, advance, render.
	// Reconcile against authoritative tape (rollback on mispredict). Confirmed
	// timeline stays server-authoritative (clientVerify is the determinism gate).
	if (maplecast_predict::liveActive()) {
		// JOIN CATCH-UP GATE (2026-07-25): the predict run-ahead is audio-paced at
		// 60fps and CANNOT outrun the server to close a behind-JOIN gap. Worse, the
		// render loop advances localFrame every frame while `confirmed` only advances
		// as the (60fps) tape reconciles — so a head that starts far behind grows the
		// head-confirmed gap past the ring and WEDGES (the lead=-408 freeze). Fix:
		// only PREDICT when we're near the server's live edge. When far behind, fall
		// through to the lockstep catch-up below — it fastForward-replays the
		// authoritative tape (head AND confirmed advance together, gap stays ~0) until
		// re-centred, then predict re-arms clean. Hysteretic so it can't flap.
		static bool _lpCatchingUp = false;
		const uint64_t sLive    = maplecast_lockstep::lastServerFrame();
		const uint64_t behindBy = (sLive > localFrame) ? (sLive - localFrame) : 0;
		if      (!_lpCatchingUp && behindBy > 16) { _lpCatchingUp = true;  _lpInited = false;
			printf("[predict-live] fell %llu behind live edge -> lockstep catch-up\n", (unsigned long long)behindBy); fflush(stdout); }
		else if ( _lpCatchingUp && behindBy <= 4)  { _lpCatchingUp = false;
			printf("[predict-live] re-centred (%llu behind) -> resume predict\n", (unsigned long long)behindBy); fflush(stdout); }
		if (!_lpCatchingUp) {
			// Near the live edge — run the predict drive. Record the head's game-state
			// hash for the frame that just finished (RAM = END of localFrame-1); when
			// that frame later CONFIRMS without a rollback it's compared to the server's
			// authoritative hash => the determinism gate for the leading predict head.
			if (!_lpLiveHashInit) { for (int i=0;i<LP_RING;i++) _lpLiveHashFrame[i]=UINT64_MAX; _lpLiveHashInit=true; }
			if (localFrame > 0) {
				const uint64_t hf = localFrame - 1;
				_lpLiveHash[hf % LP_RING] = maplecast_rollback::gameStateRegionHash();
				_lpLiveHashFrame[hf % LP_RING] = hf;
				if (std::getenv("MAPLECAST_SUBHASH_LOG"))
					maplecast_rollback::gameStateSubHashes(_lpLiveSub[hf % LP_RING]);
			}
			if (livePredictDrive(localFrame))
				return true;
			return false;   // transient: not enough tape yet to start
		}
		// else: far behind -> DO NOT predict; fall through to the lockstep catch-up
		// (fastForward replay of the authoritative tape) which re-centres us.
	}

	// ── Lockstep catch-up + advance (determinism-correct) ──────────────
	// With a state-sync JOIN + per-frame checksum, the client MUST EXECUTE
	// every frame from the seeded snapshot forward, consuming tape[localFrame]
	// EXACTLY. We never skip execution (the old "fast-forward the counter to
	// the queue head" path jumped over frames without running them — which is
	// fine for a tape-only viewer but DESYNCS a lockstep client, since the
	// game state then reflects fewer frames than the counter claims).
	//
	// Live-join catch-up: after applying a ~10 MB JOIN at frame S the server
	// has run ahead to L, so the tape holds S..L. We replay S..L by running
	// each frame back-to-back; `fastForwardMode` unthrottles wall-clock pacing
	// (never SH4 execution — determinism-safe) so we outrun the live server
	// and settle a few frames behind L. If tape[localFrame] has aged out of
	// the buffer (we fell too far behind to reproduce it), we request a fresh,
	// NEWER JOIN and retry — the self-correcting catch-up loop.
	bool     slotExact[2] = { false, false };
	bool     slotGone[2]  = { false, false };  // localFrame older than buffer
	uint64_t qOldest[2]   = { 0, 0 };
	uint64_t qNewest[2]   = { 0, 0 };
	size_t   qSize[2]     = { 0, 0 };
	maplecast_input::TapeEntry pending[2];

	for (int slot = 0; slot < 2; slot++) {
		PendingQueue& q = _queues[slot];
		std::lock_guard<std::mutex> lock(q.mu);
		// Discard entries strictly older than the frame we need.
		while (!q.entries.empty() && q.entries.front().frame < localFrame)
			q.entries.pop_front();
		qSize[slot] = q.entries.size();
		if (q.entries.empty()) continue;   // transient: no data yet for this slot
		qOldest[slot] = q.entries.front().frame;
		qNewest[slot] = q.entries.back().frame;
		if (qOldest[slot] == localFrame) {
			// PEEK ONLY — do NOT pop here. We must consume the frame from BOTH
			// slots atomically once bothExact; popping a ready slot's frame while
			// the other slot isn't ready yet (queues momentarily out of sync,
			// e.g. right after a JOIN) would LOSE that frame and wedge the two
			// slots permanently (the classic "frame N truly lost" after JOIN).
			pending[slot] = q.entries.front();
			slotExact[slot] = true;
		} else {
			// front > localFrame: the frame we need aged out of the buffer.
			slotGone[slot] = true;
		}
	}

	const bool bothExact = slotExact[0] && slotExact[1];
	const bool anyGone   = slotGone[0] || slotGone[1];

	// ── Playout-buffer pacing (single-threaded, RENDER EVERY FRAME) ──────
	// CRITICAL: single-threaded lockstep must render every frame. present()->
	// getSh4Executor()->Stop() after each frame (Renderer_if.cpp:443) is what
	// returns control to frameGate so the NEXT tape input is applied. Any
	// render-SKIP (config::SkipFrame / rend_enable_renderer=false) makes
	// QueueRender return false -> no present -> no Stop -> runInternal runs
	// SEVERAL frames on STALE input before the next STARTRENDER -> the sim
	// DIVERGES from the server (which applies fresh input every frame). So NO
	// render-skip here (removing it also fixed the genuine mismatches).
	//
	// Rate: with a FAST render (DumpTextures off + native 480p + no vsync,
	// ~9ms/frame) the sim runs faster than realtime, so the audio backend paces
	// it to a STEADY 60fps at the live edge (push() blocks to 44.1kHz,
	// audiostream.cpp:63) with sound. To CATCH UP a post-JOIN backlog we mute
	// audio (fastForwardMode -> AICA muted -> no push block, sgc_if.cpp:1599) so
	// the dynarec runs ~110fps (still rendering every frame) until re-centred on
	// the playout buffer, then hand pacing back to 60fps audio.
	static constexpr int64_t kBufferDepth = 3;   // target frames behind the head
	uint64_t tapeHead = 0;
	if (qNewest[0] && qNewest[1]) tapeHead = std::min(qNewest[0], qNewest[1]);
	const int64_t buffer = (tapeHead > localFrame) ? (int64_t)(tapeHead - localFrame) : 0;
	const int64_t err    = buffer - kBufferDepth;

	// Hysteretic catch-up: mute audio (=> sim runs at ~110fps) only for a LARGE
	// backlog (post-JOIN transfer latency / a stalled re-JOIN). We do NOT try to
	// actively drain small offsets: the fastForward drain (110fps vs the 60fps
	// producer) empties the queue faster than the per-tick disengage check can
	// react, overshooting the buffer to 0 -> underruns/stutter (measured). So the
	// buffer settles at the JOIN transfer latency (~5-6 on localhost, ~90ms). The
	// real low-latency fix is client-side PREDICTION+ROLLBACK (MAPLECAST_PREDICT),
	// which runs the sim AT the live edge and removes the playout buffer entirely.
	static bool _catchUp = false;
	if      (!_catchUp && err >= 15) _catchUp = true;
	else if ( _catchUp && err <= 2)  _catchUp = false;
	if (maplecast_lockstep::active())
		settings.input.fastForwardMode = _catchUp;

	const int64_t nowT = nowUs();

	if (bothExact) {
		// Both slots have localFrame — NOW pop it from both (atomic consume).
		for (int slot = 0; slot < 2; slot++) {
			PendingQueue& q = _queues[slot];
			std::lock_guard<std::mutex> lock(q.mu);
			if (!q.entries.empty() && q.entries.front().frame == localFrame)
				q.entries.pop_front();
		}
		applyEntry(pending[0], 0);
		applyEntry(pending[1], 1);
		// Record the authoritative input just applied for `localFrame` so the
		// predict/reconcile re-sim (and GATE 0) can replay it. No-op unless
		// MAPLECAST_PREDICT is set.
		if (maplecast_predict::active())
			maplecast_predict::recordAppliedInput(localFrame);
		_localFrame.fetch_add(1, std::memory_order_relaxed);
		// Periodic playout telemetry — fps (~60 steady, ~110 during catch-up),
		// buffer depth (should hover ~kBufferDepth, flat), catch-up state.
		if (maplecast_lockstep::active()) {
			static int64_t _t0 = 0; static uint64_t _f0 = 0;
			if (_t0 == 0) { _t0 = nowT; _f0 = localFrame; }
			if (nowT - _t0 >= 1000000) {
				double fps = (double)(localFrame - _f0) * 1e6 / (double)(nowT - _t0);
				printf("[player] PLAYOUT fps=%.1f frame=%llu tapeHead=%llu buffer=%lld catchUp=%d\n",
				       fps, (unsigned long long)localFrame, (unsigned long long)tapeHead,
				       (long long)buffer, (int)_catchUp);
				fflush(stdout);
				_t0 = nowT; _f0 = localFrame;
			}
		}
		return true;
	}

	// Not advanceable this tick. Truly-lost (later frames arrived but this one
	// aged out of BOTH the server ring and our queue) -> re-JOIN. Being a few
	// frames behind is the STEADY STATE now, never an error, so a plain underrun
	// (frame just hasn't arrived yet) only HOLDS and refills.
	if (anyGone) {
		// localFrame isn't in the queue but LATER frames are. Usually it's just
		// a late/out-of-order packet — HOLD and let it arrive (enqueueEntry sorts
		// it back into place). Only if it never shows after a grace window is it
		// truly lost, and only THEN re-JOIN. This keeps the mirror running
		// through ordinary UDP jitter instead of storming 10 MB re-JOINs.
		static uint64_t _waitFrame = 0; static int64_t _waitStartUs = 0;
		if (_waitFrame != localFrame) { _waitFrame = localFrame; _waitStartUs = nowT; }
		if (nowT - _waitStartUs >= 500000) {
			static int64_t _lastReJoinUs = 0;
			if (nowT - _lastReJoinUs > 2000000) {
				_lastReJoinUs = nowT;
				printf("[player] tape frame %llu truly lost (queue [%llu..%llu]/%zu, "
				       "waited 500ms) — re-JOIN\n",
				       (unsigned long long)localFrame,
				       (unsigned long long)qOldest[0], (unsigned long long)qNewest[0], qSize[0]);
				requestResync();
			}
		} else {
			static uint64_t _lateCtr = 0;
			if ((_lateCtr++ % 60) == 0)
				printf("[player] tape frame %llu late (queue [%llu..%llu]/%zu) — holding\n",
				       (unsigned long long)localFrame,
				       (unsigned long long)qOldest[0], (unsigned long long)qNewest[0], qSize[0]);
		}
	} else {
		static uint64_t _underrunCtr = 0;
		if ((_underrunCtr++ % 240) == 0)
			printf("[player] playout underrun: want=%llu tapeHead=%llu buffer=%lld "
			       "(holding, buffer refills)\n",
			       (unsigned long long)localFrame, (unsigned long long)tapeHead,
			       (long long)buffer);
	}
	_framesStalled.fetch_add(1, std::memory_order_relaxed);
	return false;
}

Stats getStats()
{
	Stats s{};
	s.active              = _active.load(std::memory_order_relaxed);
	s.connected           = _connected.load(std::memory_order_relaxed);
	s.packetsReceived     = _packetsReceived.load(std::memory_order_relaxed);
	s.entriesReceived     = _entriesReceived.load(std::memory_order_relaxed);
	s.entriesApplied      = _entriesApplied.load(std::memory_order_relaxed);
	s.entriesDroppedStale = _entriesDroppedStale.load(std::memory_order_relaxed);
	s.framesStalled       = _framesStalled.load(std::memory_order_relaxed);
	s.framesSpeculated    = _framesSpeculated.load(std::memory_order_relaxed);
	s.lastAppliedFrame    = _lastAppliedFrame.load(std::memory_order_relaxed);
	s.serverLatestFrame   = _serverLatestFrame.load(std::memory_order_relaxed);
	s.lastPacketArrivalUs = _lastPacketArrivalUs.load(std::memory_order_relaxed);
	return s;
}

}
