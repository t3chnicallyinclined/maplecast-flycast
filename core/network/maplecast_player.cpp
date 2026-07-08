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

#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
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

bool frameGate()
{
	if (!_active.load(std::memory_order_relaxed)) return true;  // no-op path

	// Phase 4-lite: snapshot local SDL gamepad state and forward to the
	// server FIRST. The server echoes our inputs back through the tape
	// stamped with the authoritative frame number â€” that's the round-trip
	// that makes the local gamepad "feel real" from the SH4's perspective.
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
	if (maplecast_lockstep::active())
		maplecast_lockstep::clientVerify(localFrame);

	// Predict/rollback subsystem — STAGE 1 drives the no-render re-sim PRIMITIVE
	// gate here (SH4 is paused between frames). No-op unless MAPLECAST_PREDICT is
	// set. The full predict/ring/reconcile loop is a later stage.
	if (maplecast_predict::active())
		maplecast_predict::onFrameBoundary(localFrame);

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
