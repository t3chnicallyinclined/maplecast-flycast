/*
	================================================================
	SHELVED 2026-04-10 — superseded by native TA stream viewer
	================================================================
	The replica approach (local SH4 + tape inputs + TA correction)
	worked for sync but the local SH4 is unnecessary when the TA
	stream is the visual authority. The new approach is a native
	renderer-only client (no SH4) that:
	  1. Receives the TA stream from the relay (same as WASM viewers)
	  2. Sends local input to the server via UDP input sink
	  3. No SH4, no kcode[], no tape, no state-sync
	See packages/viewer/ for the new native client.

	This file is kept for reference. The input sink callback pattern
	(onButtonEvent / sinkSend) may be reused in the native viewer.
	================================================================

	MapleCast Replica Client — deterministic SH4 replica of the headless server.

	rx thread receives UDP tape packets from the server and writes
	directly into kcode[]/lt[]/rt[]. The SH4 reads these at CMD9.

	Periodic state sync (~1/sec): the state-sync TCP client receives
	STAT envelopes. When one arrives, we stop the SH4 executor, apply
	the state on the emu thread (via frameGate), and let it resume.
	This snaps the machine to the server's authoritative state.
*/
#include "types.h"
#include "maplecast_replica.h"
#include "maplecast_input_server.h"
#include "maplecast_state_sync.h"
#include "maplecast_mirror.h"
#include "emulator.h"
#include "hw/sh4/sh4_if.h"
#include "input/gamepad_device.h"
#include "cfg/option.h"

#include <atomic>
#include <thread>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

extern u32 kcode[4];
extern u16 rt[4], lt[4];

namespace maplecast_replica
{

static std::atomic<bool>      _active{false};
static std::atomic<bool>      _bootstrapped{false};
static std::thread            _rxThread;
static std::atomic<bool>      _rxRun{false};
static int                    _rxSock = -1;
static sockaddr_in            _serverAddr{};
static std::string            _serverHost;
static std::atomic<uint64_t>  _packetsReceived{0};
static std::atomic<uint64_t>  _syncsApplied{0};

// Input sink — sends local SDL gamepad events directly to the server's
// input port (UDP:7100) via a ButtonListener callback. No polling, no
// kcode[] races. SDL fires the callback synchronously on the main thread
// the instant a button changes; we sendto() inline.
static int                    _sinkSock = -1;
static sockaddr_in            _sinkAddr{};
static int                    _sinkSlot = 0;
static std::atomic<uint16_t>  _sinkButtons{0xFFFF};   // accumulated button state (active-low)
static std::atomic<uint8_t>   _sinkLt{0};
static std::atomic<uint8_t>   _sinkRt{0};

// Set by the tape rx thread when it's time to apply a periodic state sync.
// Checked by frameGate on the emu thread (between runInternal calls).
static std::atomic<bool>      _wantSync{false};

static inline int64_t nowUs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static bool resolveServer(const char* spec)
{
	if (!spec || !*spec) return false;
	std::string s = spec, host = s;
	int port = 7101;
	size_t colon = s.find_last_of(':');
	if (colon != std::string::npos) {
		host = s.substr(0, colon);
		port = std::atoi(s.c_str() + colon + 1);
		if (port <= 0 || port > 65535) return false;
	}
	_serverHost = host;
	struct addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	struct addrinfo* res = nullptr;
	if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
	memcpy(&_serverAddr, res->ai_addr, sizeof(sockaddr_in));
	_serverAddr.sin_port = htons((uint16_t)port);
	freeaddrinfo(res);
	printf("[replica] tape server: %s:%d\n", host.c_str(), port);
	return true;
}

// Send the current accumulated button+trigger state to the input sink.
static void sinkSend()
{
	if (_sinkSock < 0) return;
	const uint16_t btn = _sinkButtons.load(std::memory_order_relaxed);
	const uint8_t ltv  = _sinkLt.load(std::memory_order_relaxed);
	const uint8_t rtv  = _sinkRt.load(std::memory_order_relaxed);
	uint8_t pkt[7] = { 'P', 'C', (uint8_t)_sinkSlot, ltv, rtv,
	                   (uint8_t)(btn >> 8), (uint8_t)(btn & 0xFF) };
	sendto(_sinkSock, pkt, sizeof(pkt), 0,
	       (const sockaddr*)&_sinkAddr, sizeof(_sinkAddr));
}

// SDL ButtonListener callback — fires synchronously on the main thread
// the instant a button is pressed or released. We accumulate into our
// own atomic button state and send immediately. No kcode[] involvement.
static void onButtonEvent(int port, DreamcastKey key, bool pressed)
{
	if (port != _sinkSlot) return;
	if (key > DC_BTN_BITMAPPED_LAST) {
		// Trigger buttons (LT/RT mapped as digital)
		if (key == DC_AXIS_LT) {
			_sinkLt.store(pressed ? 0xFF : 0, std::memory_order_relaxed);
			sinkSend();
		} else if (key == DC_AXIS_RT) {
			_sinkRt.store(pressed ? 0xFF : 0, std::memory_order_relaxed);
			sinkSend();
		}
		return;
	}
	uint16_t cur = _sinkButtons.load(std::memory_order_relaxed);
	if (pressed)
		cur &= ~(uint16_t)key;
	else
		cur |= (uint16_t)key;
	_sinkButtons.store(cur, std::memory_order_relaxed);
	sinkSend();
}

// --- Receive thread: writes directly into kcode[]/lt[]/rt[] ---
// Also triggers periodic state sync by stopping the SH4 executor
// when a new STAT is pending.

static void rxLoop()
{
	printf("[replica] rx thread started\n");
	int bsock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (bsock < 0) { printf("[replica] socket() failed\n"); return; }
	sockaddr_in local{};
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = INADDR_ANY;
	local.sin_port = 0;
	bind(bsock, (sockaddr*)&local, sizeof(local));
	connect(bsock, (sockaddr*)&_serverAddr, sizeof(_serverAddr));
	_rxSock = bsock;
	struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
	setsockopt(_rxSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	int64_t lastHeloUs = 0;
	int64_t lastSyncCheckUs = 0;
	uint8_t buf[2048];
	while (_rxRun.load(std::memory_order_relaxed))
	{
		int64_t now = nowUs();

		// HELO keepalive
		if (now - lastHeloUs > 900000) {
			send(_rxSock, "HELO", 4, 0);
			lastHeloUs = now;
		}

		// Periodic sync check: every ~1 second, see if the state-sync
		// client has a pending STAT. If so, stop the SH4 executor so
		// frameGate can apply it safely on the emu thread.
		if (now - lastSyncCheckUs > 1000000) {
			lastSyncCheckUs = now;
			// Check if state-sync has a pending state
			auto stats = maplecast_state_sync::getClientStats();
			if (stats.statesReceived > _syncsApplied.load(std::memory_order_relaxed)) {
				_wantSync.store(true, std::memory_order_relaxed);
				// Stop the SH4 — this makes runInternal() return,
				// the emu loop re-enters and calls frameGate() where
				// we apply the state safely on the emu thread.
				emu.getSh4Executor()->Stop();
			}
		}

		ssize_t n = recv(_rxSock, buf, sizeof(buf), 0);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
			break;
		}
		if (n < 8) continue;
		if (!(buf[0]=='I' && buf[1]=='N' && buf[2]=='P' && buf[3]=='T')) continue;
		if (buf[4] != 1) continue;
		uint8_t count = buf[5];
		if ((size_t)n < 8 + (size_t)count * sizeof(maplecast_input::TapeEntry)) continue;

		uint64_t pkts = _packetsReceived.fetch_add(1, std::memory_order_relaxed) + 1;

		// TODO: local input forwarding disabled — sticky button bug.
		// The TA stream correction path works without it. Local input
		// forwarding (native gamepad → server) is deferred to a future
		// session. The current architecture (TA stream + tape for opponent
		// inputs) works on any device including Chromebooks.

		const maplecast_input::TapeEntry* entries =
			reinterpret_cast<const maplecast_input::TapeEntry*>(buf + 8);
		for (uint8_t i = 0; i < count; i++) {
			uint32_t seq; uint8_t slot;
			maplecast_input::unpackSeqSlot(entries[i].seqAndSlot, seq, slot);
			if (slot > 1) continue;
			kcode[slot] = entries[i].buttons | 0xFFFF0000u;
			lt[slot]    = (uint16_t)entries[i].lt << 8;
			rt[slot]    = (uint16_t)entries[i].rt << 8;
		}

		if (pkts % 60 == 1) {
			printf("[replica] rx pkts=%llu syncs=%llu\n",
			       (unsigned long long)pkts,
			       (unsigned long long)_syncsApplied.load());
		}
	}
	close(_rxSock); _rxSock = -1;
	printf("[replica] rx thread stopped\n");
}

// --- frameGate ---
// Called on the emu thread between runInternal() calls. Normally a
// pass-through. When _wantSync is set, applies the pending STAT
// (safe because the SH4 executor has been stopped).

bool frameGate()
{
	if (!_active.load(std::memory_order_relaxed)) return true;
	if (!_bootstrapped.load(std::memory_order_relaxed)) return false;

	if (_wantSync.load(std::memory_order_relaxed)) {
		_wantSync.store(false, std::memory_order_relaxed);
		if (maplecast_state_sync::clientApplyPending()) {
			_syncsApplied.fetch_add(1, std::memory_order_relaxed);
			auto stats = maplecast_state_sync::getClientStats();
			printf("[replica] SYNC applied @ frame %llu\n",
			       (unsigned long long)stats.lastAppliedFrame);
		}
	}

	return true;
}

// --- Lifecycle ---

bool init()
{
	if (_active.load()) return true;
	const char* spec = std::getenv("MAPLECAST_REPLICA");
	if (!spec || !*spec) return false;
	if (!resolveServer(spec)) return false;

	settings.aica.muteAudio = true;

	_active.store(true);
	_rxRun.store(true);
	_rxThread = std::thread(rxLoop);

	if (!maplecast_state_sync::clientStart(_serverHost.c_str())) {
		printf("[replica] state-sync failed\n");
		_rxRun.store(false);
		if (_rxThread.joinable()) _rxThread.join();
		_active.store(false);
		settings.aica.muteAudio = false;
		return false;
	}

	// Block until initial STAT arrives — applied on main thread, cold executor
	printf("[replica] waiting for state-sync...\n");
	for (int i = 0; i < 1000; i++) {
		if (maplecast_state_sync::clientApplyPending()) {
			_bootstrapped.store(true, std::memory_order_relaxed);
			_syncsApplied.fetch_add(1, std::memory_order_relaxed);
			auto stats = maplecast_state_sync::getClientStats();
			printf("[replica] === REPLICA MODE === boot frame %llu\n",
			       (unsigned long long)stats.lastAppliedFrame);

			// Start the TA mirror stream as a correction channel.
			// startMirrorStream launches the WS receiver thread WITHOUT
			// setting isClient mode — so the GUI and SH4 keep running.
			char ipstr[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &_serverAddr.sin_addr, ipstr, sizeof(ipstr));
			printf("[replica] starting TA mirror stream from %s:7201\n", ipstr);
			maplecast_mirror::startMirrorStream(ipstr, 7201);

			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	printf("[replica] BOOTSTRAP TIMEOUT\n");
	_rxRun.store(false);
	if (_rxSock >= 0) ::shutdown(_rxSock, SHUT_RDWR);
	if (_rxThread.joinable()) _rxThread.join();
	maplecast_state_sync::clientStop();
	_active.store(false);
	settings.aica.muteAudio = false;
	return false;
}

void shutdown()
{
	if (!_active.load()) return;
	_active.store(false);
	_rxRun.store(false);
	settings.aica.muteAudio = false;
	maplecast_state_sync::clientStop();
	if (_rxSock >= 0) ::shutdown(_rxSock, SHUT_RDWR);
	if (_rxThread.joinable()) _rxThread.join();
	printf("[replica] shutdown\n");
}

bool active() { return _active.load(std::memory_order_relaxed); }

Stats getStats()
{
	Stats s{};
	s.active = _active.load(std::memory_order_relaxed);
	s.bootstrapped = _bootstrapped.load(std::memory_order_relaxed);
	s.tapeConnected = _packetsReceived.load() > 0;
	s.tapePacketsReceived = _packetsReceived.load(std::memory_order_relaxed);
	return s;
}

}
