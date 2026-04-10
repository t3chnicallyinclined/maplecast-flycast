/*
	================================================================
	SHELVED 2026-04-09 — superseded by GGPO peer mode
	================================================================
	This file (and maplecast_state_sync.cpp) was a hand-rolled
	replay client: bespoke UDP tape subscriber + bespoke TCP state-
	sync + a stall-only frameGate() with no rollback. It boots and
	runs against nobd.net but desyncs from the authoritative server
	because it lacks save-state ring + rollback + fast-forward.

	The correct architecture — which this branch is now moving to —
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
	    maplecast_player::init() / frameGate() — emulator.cpp:1006-
	    1010 and :1090 / :1155 call into it.
	  - Held as a fallback diagnostic until the GGPO peer mode is
	    proven end-to-end against nobd.net.
	  - Once GGPO peer mode is green, this file + maplecast_state_sync.*
	    + the publishFrameTick / tape ring code in
	    maplecast_input_server.cpp can all be removed in one commit.

	DO NOT add features here. Anything new goes into the GGPO path.
	================================================================

	MapleCast Player Client — lockstep tape subscriber.

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

#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

// kcode[]/lt[]/rt[] globals — same globals the server writes in updateSlot()
extern u32 kcode[4];
extern u16 rt[4], lt[4];

namespace maplecast_player
{

static std::atomic<bool>      _active{false};
static std::atomic<bool>      _connected{false};
static std::thread            _rxThread;
static int                    _sock = -1;
static sockaddr_in            _serverAddr{};     // tape server endpoint
static std::string            _serverHost;       // bare host (no :port) — state-sync connects to kStatePort
static std::atomic<uint8_t>   _stallPolicy{(uint8_t)StallPolicy::Hard};

// True once maplecast_state_sync::clientApplyPending() has successfully
// applied at least one STAT envelope. Until then we don't know what
// server frame the SH4 is at, so frameGate MUST stall — we can't trust
// the internal _localFrame counter against the tape queue.
static std::atomic<bool>      _initialSynced{false};

// Per-slot pending queue of tape entries, ordered by frame. The receive
// thread pushes back; frameGate pops front as entries mature.
//
// Ordering: the server emits entries in push order per slot, and UDP is
// unordered but single-host. We sort-insert on arrival to tolerate out-
// of-order packets. Capped length — if the client is catastrophically
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
// Phase 3 savestate sync). Owned by the emu thread after init — the
// atomic is defensive in case other threads want to read it for UI.
static std::atomic<uint64_t> _localFrame{0};

// Phase 4-lite: local SDL gamepad input forwarding. We snapshot the
// locally-written kcode[0]/lt[0]/rt[0] at the top of frameGate() —
// BEFORE the tape overwrites them with authoritative server state —
// and if the state has changed since the last send, ship a 7-byte PC
// packet to the server's MapleCast input port (7100). The server
// auto-binds our source IP to slot 0 on first packet.
//
// This is a raw UDP send on the emu thread — no retry, no acks. The
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
		// Ignore IPv6-style brackets for now — this project's deploy is v4.
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
// they're useless — the only reason to keep older entries is if the emu
// is still catching up, which is handled by the front-pop drain in
// frameGate.
static void enqueueEntry(const maplecast_input::TapeEntry& e, uint8_t slot)
{
	if (slot > 1) return;
	PendingQueue& q = _queues[slot];
	std::lock_guard<std::mutex> lock(q.mu);

	// Hard cap — if someone's queue runaway, drop the oldest.
	while (q.entries.size() >= kMaxQueueLen) {
		q.entries.pop_front();
		_entriesDroppedStale.fetch_add(1, std::memory_order_relaxed);
	}

	// Fast path: entry is newer than tail → append.
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
	// recv() only delivers datagrams from that source (light filtering)
	// and send() can go without a destination.
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
		close(bsock);
		return;
	}
	if (connect(bsock, (sockaddr*)&_serverAddr, sizeof(_serverAddr)) < 0) {
		// Not fatal — just means we do explicit sendto() fallbacks below.
		// But on a healthy setup connect() to a UDP socket always succeeds,
		// so log and continue.
		printf("[player] connect() warning: %s\n", strerror(errno));
	}
	_sock = bsock;

	// 100ms receive timeout so we can interleave HELO keepalives and
	// still shut down promptly.
	struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
	setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	int64_t lastHeloUs = 0;
	uint8_t buf[2048];

	while (_active.load(std::memory_order_relaxed))
	{
		// Send HELO every ~1 second (subscriber TTL on the server is 5s).
		int64_t now = nowUs();
		if (now - lastHeloUs > 900000) {
			const char helo[4] = { 'H', 'E', 'L', 'O' };
			ssize_t sent = send(_sock, helo, 4, 0);
			if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
				// Non-fatal — server may be momentarily unreachable.
				static int64_t lastLogUs = 0;
				if (now - lastLogUs > 5000000) {
					printf("[player] HELO send failed: %s\n", strerror(errno));
					lastLogUs = now;
				}
			}
			lastHeloUs = now;
		}

		ssize_t n = recv(_sock, buf, sizeof(buf), 0);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
			printf("[player] recv error: %s\n", strerror(errno));
			break;
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

	close(_sock);
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

	const uint32_t rawKcode = kcode[0];
	const uint16_t buttons  = (uint16_t)(rawKcode & 0xFFFF);
	const uint8_t  ltVal    = (uint8_t)(lt[0] >> 8);
	const uint8_t  rtVal    = (uint8_t)(rt[0] >> 8);

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

	ssize_t sent = sendto(_fwdSock, pkt, sizeof(pkt), 0,
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

	_active.store(true);
	_rxThread = std::thread(rxLoop);

	// Start the state-sync TCP client. Reconnects automatically on
	// failure so it's safe to call before the server is necessarily
	// listening. Until the first STAT envelope arrives and is applied,
	// frameGate stalls the SH4 unconditionally (see _initialSynced).
	if (!maplecast_state_sync::clientStart(_serverHost.c_str())) {
		printf("[player] warning: state-sync clientStart failed — SH4 will stall "
		       "until a state arrives\n");
	}

	printf("[player] === PLAYER CLIENT MODE ENABLED ===\n");
	return true;
}

void shutdown()
{
	if (!_active.load()) return;
	_active.store(false);
	maplecast_state_sync::clientStop();
	if (_fwdSock >= 0) {
		close(_fwdSock);
		_fwdSock = -1;
	}
	if (_sock >= 0) {
		// Kick the rx thread out of its recv() via shutdown() — close()
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

bool frameGate()
{
	if (!_active.load(std::memory_order_relaxed)) return true;  // no-op path

	// Phase 4-lite: snapshot local SDL gamepad state and forward to the
	// server FIRST. The server echoes our inputs back through the tape
	// stamped with the authoritative frame number — that's the round-trip
	// that makes the local gamepad "feel real" from the SH4's perspective.
	forwardLocalInput();

	// Phase 3 v2: ONE-SHOT initial state apply. Once _initialSynced is
	// true, clientApplyPending will return false on every subsequent call
	// because the server only sends a STAT envelope once per session.
	// The heavy emu.loadstate work happens exactly once at session start.
	if (!_initialSynced.load(std::memory_order_relaxed)) {
		if (maplecast_state_sync::clientApplyPending()) {
			const uint64_t newFrame = _localFrame.load(std::memory_order_relaxed);
			// Flush any tape entries from BEFORE the seeded frame —
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
		} else {
			// No state yet — block the SH4. We have no frame number
			// to apply tape entries against.
			_framesStalled.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
	}

	const uint64_t localFrame = _localFrame.load(std::memory_order_relaxed);
	const StallPolicy policy  = (StallPolicy)_stallPolicy.load(std::memory_order_relaxed);

	// GGPO-style blocking read against the dense tape. The server now
	// publishes one entry per slot per server frame (see
	// maplecast_input_server::publishFrameTick), so for any localFrame
	// the answer is unambiguous:
	//
	//   - if the queue front for this slot is at frame < localFrame,
	//     it's a stale entry from before we caught up — discard it.
	//   - if the queue front is at frame == localFrame, apply it (this
	//     is the input the server's SH4 saw at this frame) and pop.
	//   - if the queue front is at frame > localFrame, the server has
	//     moved past us — fast-forward localFrame to that entry's frame
	//     and apply (the catchup-from-stall path).
	//   - if the queue is empty for this slot, we don't have data yet,
	//     stall.
	//
	// Both slots must be satisfied before the SH4 can advance.
	bool slotSatisfied[2]      = { false, false };
	uint64_t advanceTarget     = localFrame;
	bool needFastForward       = false;
	uint64_t fastForwardFrame  = localFrame;

	for (int slot = 0; slot < 2; slot++) {
		PendingQueue& q = _queues[slot];
		std::lock_guard<std::mutex> lock(q.mu);
		// Discard stale heads.
		while (!q.entries.empty() && q.entries.front().frame < localFrame)
			q.entries.pop_front();
		if (q.entries.empty()) continue;
		const uint64_t headFrame = q.entries.front().frame;
		if (headFrame == localFrame) {
			maplecast_input::TapeEntry e = q.entries.front();
			q.entries.pop_front();
			applyEntry(e, (uint8_t)slot);
			slotSatisfied[slot] = true;
		} else if (headFrame > localFrame) {
			// Catchup. Take the head — the server has produced data
			// for this slot at headFrame, we should fast-forward to
			// match. Both slots will independently arrive at the same
			// fast-forward frame because publishFrameTick stamps both
			// slots with the same frame number per server tick.
			needFastForward = true;
			if (headFrame > fastForwardFrame) fastForwardFrame = headFrame;
			maplecast_input::TapeEntry e = q.entries.front();
			q.entries.pop_front();
			applyEntry(e, (uint8_t)slot);
			slotSatisfied[slot] = true;
		}
	}

	const bool bothSatisfied = slotSatisfied[0] && slotSatisfied[1];

	if (bothSatisfied) {
		if (needFastForward) {
			_localFrame.store(fastForwardFrame + 1, std::memory_order_relaxed);
		} else {
			_localFrame.fetch_add(1, std::memory_order_relaxed);
		}
		return true;
	}

	if (policy == StallPolicy::Speculate) {
		// Speculate: advance with the last-applied kcode for any slot
		// we couldn't satisfy. Used for bad-network fallback testing.
		_framesSpeculated.fetch_add(1, std::memory_order_relaxed);
		_localFrame.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	// Hard policy: spin the emu loop. The 250µs sleep in the emu loop
	// caller (core/emulator.cpp) keeps the CPU from melting.
	_framesStalled.fetch_add(1, std::memory_order_relaxed);
	(void)advanceTarget;  // reserved for future telemetry
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
