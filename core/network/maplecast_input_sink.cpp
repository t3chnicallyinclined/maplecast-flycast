/*
	MapleCast Input Sink â€” implementation.
	See maplecast_input_sink.h for design.

	â”€â”€ Phase 2 (2026-04-14) competitive features â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
	â€¢ Wire format: 11 bytes [P][C][slot][seq:u32_LE][LT][RT][btn_hi][btn_lo].
	â€¢ Every packet sent twice (T+0 + T+1ms via deferred-send thread).
	â€¢ Hot-standby socket to backup server, instant failover.
	â€¢ SCHED_FIFO on the trigger poll thread (graceful degrade).
*/
#include "types.h"
#include "maplecast_input_sink.h"
#include "input/gamepad_device.h"
#include "ui/gui_game_overlay.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>

#include "net_platform.h"
#include "maplecast_compat.h"
#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#endif

extern u16 lt[4], rt[4];

namespace maplecast_input_sink
{

// â”€â”€ Primary + standby sockets â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static int            _sock = -1;          // primary
static sockaddr_in    _addr{};
static int            _backupSock = -1;    // standby (always ready)
static sockaddr_in    _backupAddr{};
static std::atomic<bool> _hasBackup{false};
static std::atomic<bool> _onBackup{false}; // true â†’ primary failed over

static int            _slot = 0;
static std::atomic<int> _gamepadPort{-1}; // actual SDL gamepad port (may differ from _slot)
static bool           _active = false;
static std::thread    _triggerThread;
static std::atomic<bool> _triggerRun{false};
static std::thread    _redundantThread;
static std::atomic<bool> _redundantRun{false};

// Accumulated button state (active-low, same as kcode[] format).
static uint16_t       _buttons = 0xFFFF;

// Per-packet sequence number (monotonic, server uses for dedup)
static std::atomic<uint32_t> _packetSeq{0};

// Stats
static std::atomic<uint64_t> _packetsSent{0};
static std::atomic<uint64_t> _redundantSends{0};
static std::atomic<uint64_t> _failovers{0};
static std::atomic<uint64_t> _buttonChanges{0};
static std::atomic<uint64_t> _triggerChanges{0};
static std::atomic<int64_t>  _lastSendUs{0};
static std::atomic<int64_t>  _lastAckFromPrimaryUs{0}; // last echo from primary
static std::atomic<uint64_t> _prevPackets{0};
static std::atomic<int64_t>  _prevRateTime{0};
static std::atomic<uint32_t> _sendRateHz{0};

// E2E latency probe â€” see header for details.
static std::atomic<int64_t>  _probeStartUs{0};
static std::atomic<double>   _e2eLastMs{0.0};
static std::atomic<double>   _e2eEmaMs{0.0};
static std::atomic<double>   _e2eMinMs{999999.0};
static std::atomic<double>   _e2eMaxMs{0.0};
static std::atomic<uint64_t> _e2eProbes{0};

static inline int64_t nowUs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

// â”€â”€ Deferred-send queue for T+1ms redundant copies â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// Each input change schedules TWO sends:
//   1. Immediate (called inline from onButton/triggerPoll â†’ sendStateOnce)
//   2. Deferred ~1ms later (queued here, drained by _redundantThread)
//
// Different network jitter window â†’ improves recovery from single-packet
// drops without adding artificial latency to the critical first send.

struct PendingSend {
	uint8_t pkt[19];
	int64_t fireAtUs;
};

static std::mutex              _pendingMtx;
static std::condition_variable _pendingCv;
static std::queue<PendingSend> _pendingQ;

// â”€â”€ Helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// Build the 19-byte wire packet at *out using the current state.
// Format: [P][C][slot][seq:u32_LE][LT][RT][btn_hi][btn_lo][client_ts:u64_LE]
// The 8-byte client_ts is CLOCK_MONOTONIC microseconds at send time.
// Server uses it to measure input age at vblank latch and phase-lock.
// Backward compatible: server detects 11 vs 19 byte packets.
static void buildPacket(uint8_t* out, uint32_t seq)
{
	int gp = _gamepadPort.load(std::memory_order_relaxed);
	int trigPort = (gp >= 0 && gp < 4) ? gp : _slot;
	uint8_t ltVal = (uint8_t)(lt[trigPort] >> 8);
	uint8_t rtVal = (uint8_t)(rt[trigPort] >> 8);
	out[0] = 'P';
	out[1] = 'C';
	out[2] = (uint8_t)_slot;
	out[3] = (uint8_t)(seq);
	out[4] = (uint8_t)(seq >> 8);
	out[5] = (uint8_t)(seq >> 16);
	out[6] = (uint8_t)(seq >> 24);
	out[7] = ltVal;
	out[8] = rtVal;
	out[9] = (uint8_t)(_buttons >> 8);
	out[10] = (uint8_t)(_buttons & 0xFF);
	// Client monotonic timestamp (microseconds)
	uint64_t ts = (uint64_t)nowUs();
	out[11] = (uint8_t)(ts);
	out[12] = (uint8_t)(ts >> 8);
	out[13] = (uint8_t)(ts >> 16);
	out[14] = (uint8_t)(ts >> 24);
	out[15] = (uint8_t)(ts >> 32);
	out[16] = (uint8_t)(ts >> 40);
	out[17] = (uint8_t)(ts >> 48);
	out[18] = (uint8_t)(ts >> 56);
}

// Send a pre-built packet to the currently-active server (primary or backup).
static void sendPacket(const uint8_t* pkt, size_t len)
{
	bool useBackup = _onBackup.load(std::memory_order_relaxed)
	              && _hasBackup.load(std::memory_order_relaxed);
	int     sock = useBackup ? _backupSock : _sock;
	const sockaddr_in& addr = useBackup ? _backupAddr : _addr;
	if (sock < 0) return;
	mc_sendto(sock, pkt, len, 0, (const sockaddr*)&addr, sizeof(addr));
	_packetsSent.fetch_add(1, std::memory_order_relaxed);
	_lastSendUs.store(nowUs(), std::memory_order_relaxed);
}

// Build + send + schedule the deferred redundant copy.
// Called from onButton (inline) and triggerPollLoop.
static void sendState()
{
	if (_sock < 0) return;
	uint32_t seq = _packetSeq.fetch_add(1, std::memory_order_relaxed) + 1;

	uint8_t pkt[19];
	buildPacket(pkt, seq);
	sendPacket(pkt, sizeof(pkt));

	// Queue the redundant copy for T+1ms.
	PendingSend p;
	memcpy(p.pkt, pkt, sizeof(pkt));
	p.fireAtUs = nowUs() + 1000;  // +1ms
	{
		std::lock_guard<std::mutex> lk(_pendingMtx);
		_pendingQ.push(p);
	}
	_pendingCv.notify_one();
}

// Background thread: drains the pending queue, sleeping until each
// packet's fireAtUs. Each packet is the same bytes as the primary
// send â€” server dedups by sequence number.
static void redundantSendLoop()
{
	std::unique_lock<std::mutex> lk(_pendingMtx);
	while (_redundantRun.load(std::memory_order_relaxed)) {
		if (_pendingQ.empty()) {
			_pendingCv.wait_for(lk, std::chrono::milliseconds(50));
			continue;
		}
		auto p = _pendingQ.front();
		int64_t now = nowUs();
		int64_t wait = p.fireAtUs - now;
		if (wait > 0) {
			_pendingCv.wait_for(lk, std::chrono::microseconds(wait));
			continue;  // re-check time after wait (might have been spurious wakeup)
		}
		_pendingQ.pop();
		lk.unlock();
		sendPacket(p.pkt, sizeof(p.pkt));
		_redundantSends.fetch_add(1, std::memory_order_relaxed);
		lk.lock();
	}
}

// â”€â”€ SDL ButtonListener â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static void onButton(int port, DreamcastKey key, bool pressed)
{
	// Accept input from any port â€” mirror client only has one local player,
	// but saved config may map the gamepad to a port != _slot.
	// Track which port is actually sending so triggerPollLoop reads the
	// right lt[]/rt[] slot.
	_gamepadPort.store(port, std::memory_order_relaxed);

	if (key <= DC_BTN_BITMAPPED_LAST) {
		// Dedupe across input sources. Both SDL and the XInput direct-poll
		// bypass register as ButtonListeners, so each physical press fires
		// onButton twice. Compute the new state and bail if nothing actually
		// changed — saves an identical UDP packet that the server's seq
		// dedup would discard anyway.
		const uint16_t prevButtons = _buttons;
		if (pressed)
			_buttons &= ~(uint16_t)key;
		else
			_buttons |= (uint16_t)key;
		if (_buttons == prevButtons)
			return;   // duplicate fire from a second input source — drop
		_buttonChanges.fetch_add(1, std::memory_order_relaxed);
		// Arm E2E probe - only if no probe is already pending.
		int64_t zero = 0;
		_probeStartUs.compare_exchange_strong(zero, nowUs(), std::memory_order_relaxed);
		sendState();
		// Record for input display overlay
		int gp = _gamepadPort.load(std::memory_order_relaxed);
		int tp = (gp >= 0 && gp < 4) ? gp : _slot;
		gui_game_overlay::recordInput(_buttons,
			(uint8_t)(lt[tp] >> 8), (uint8_t)(rt[tp] >> 8));

		// Per-button debug log — verbose, off by default. Set MAPLECAST_INPUT_DBG=1
		// to re-enable when diagnosing button mapping or wire-format issues.
		static const bool _inputDbgEnabled = std::getenv("MAPLECAST_INPUT_DBG") != nullptr;
		if (_inputDbgEnabled) {
			static uint16_t _prevDbg = 0xFFFF;
			if (_buttons != _prevDbg) {
				uint16_t changed = _prevDbg ^ _buttons;
				printf("[input-dbg] buttons=0x%04x key=0x%x %s (changed=0x%04x)\n",
				       _buttons, (unsigned)key, pressed ? "DOWN" : "UP", changed);
				fflush(stdout);
				_prevDbg = _buttons;
			}
		}
	}
}

// Trigger polling thread â€” also doubles as the failover detector.
//
// At ~120Hz (8ms), we:
//   1. Poll lt/rt for changes, send if changed
//   2. Check if primary has been silent (no probe-ACK) for 100ms+
//      â†’ if standby is healthy, swap to it
//
// Probe-ACK reception happens in the same thread via a non-blocking
// recvfrom on the primary socket. The server sends a [0xFE, ...] reply
// to every input packet (the same probe-ACK code path used by hub
// discovery â€” server doesn't differentiate). We don't care about the
// payload, just the heartbeat.
static void triggerPollLoop()
{
	// Try to bump priority for tighter timing on input. Graceful degrade
	// if we don't have the privileges. Linux uses SCHED_FIFO via pthread;
	// Windows uses THREAD_PRIORITY_TIME_CRITICAL (Pro Audio class would be
	// even higher but is reserved for actual audio work).
#ifdef __linux__
	struct sched_param sp{};
	sp.sched_priority = 50;  // mid-FIFO; don't starve audio (typically 70+)
	if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) {
		printf("[input-sink] trigger thread -> SCHED_FIFO priority 50\n");
	} else {
		// Fallback: nice -10. Doesn't need root.
		printf("[input-sink] SCHED_FIFO not granted (need CAP_SYS_NICE), staying SCHED_OTHER\n");
	}
#elif defined(_WIN32)
	if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
		printf("[input-sink] trigger thread -> THREAD_PRIORITY_TIME_CRITICAL\n");
	else
		printf("[input-sink] SetThreadPriority(TIME_CRITICAL) failed (errno=%lu)\n", GetLastError());
#endif

	uint8_t lastLt = 0, lastRt = 0;

	while (_triggerRun.load(std::memory_order_relaxed)) {
		// 1. Trigger change detection — read from actual gamepad port
		int gp = _gamepadPort.load(std::memory_order_relaxed);
		int trigPort = (gp >= 0 && gp < 4) ? gp : _slot;
		uint8_t curLt = (uint8_t)(lt[trigPort] >> 8);
		uint8_t curRt = (uint8_t)(rt[trigPort] >> 8);

		if (curLt != lastLt || curRt != lastRt) {
			lastLt = curLt;
			lastRt = curRt;
			_triggerChanges.fetch_add(1, std::memory_order_relaxed);
			sendState();
			// Record for input display overlay
			gui_game_overlay::recordInput(_buttons, curLt, curRt);
		}

		// 2. Drain any probe-ACKs from the active socket (non-blocking).
		//    Server replies [0xFE, seq, ts:u48_LE] to every input packet.
		//    We just want to know "primary is alive."
		bool useBackup = _onBackup.load(std::memory_order_relaxed)
		              && _hasBackup.load(std::memory_order_relaxed);
		int activeSock = useBackup ? _backupSock : _sock;
		if (activeSock >= 0) {
			uint8_t buf[16];
			ssize_t n;
			while ((n = mc_recv(activeSock, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
				if (n >= 2 && buf[0] == 0xFE) {
					_lastAckFromPrimaryUs.store(nowUs(), std::memory_order_relaxed);
				}
			}
		}

		// 3. Failover check â€” if primary has been silent for >100ms AND
		//    we have a healthy backup, swap. (Only swap if we've seen
		//    at least one ACK from the primary at any point â€” else we
		//    might just have an idle player.)
		if (_hasBackup.load(std::memory_order_relaxed)
		    && !_onBackup.load(std::memory_order_relaxed)) {
			int64_t lastAck = _lastAckFromPrimaryUs.load(std::memory_order_relaxed);
			int64_t lastSent = _lastSendUs.load(std::memory_order_relaxed);
			// Only consider failover if we've actually been sending recently
			// (within last 500ms) â€” no point swapping for an idle player.
			if (lastAck > 0 && lastSent > 0 && (nowUs() - lastSent) < 500000) {
				int64_t gap = nowUs() - lastAck;
				if (gap > 100000) {  // 100ms silence
					_onBackup.store(true, std::memory_order_release);
					_failovers.fetch_add(1, std::memory_order_relaxed);
					printf("[input-sink] FAILOVER â†’ backup server (primary silent %lldms)\n",
					       (long long)(gap / 1000));
				}
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(8));  // ~120Hz
	}
}

// â”€â”€ Public API â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// Pick the UDP target port for the input sink. Default 7100 (production
// flycast input server). Override with MAPLECAST_INPUT_TARGET_PORT for
// parallel-instance deploys (e.g., the Option 6 lookup sandbox at 7150).
// MAPLECAST_PORT is NOT reused here because that env var configures the
// server's LISTEN port, not the client's TARGET port — different role,
// different default, kept separate to avoid mode-dependent confusion.
static int targetPort()
{
	if (const char* p = std::getenv("MAPLECAST_INPUT_TARGET_PORT")) {
		int v = std::atoi(p);
		if (v > 0 && v < 65536) return v;
	}
	return 7100;
}

bool init(const char* host, int slot)
{
	if (_active) return true;
	if (!host || !*host) return false;

	_slot = slot;

	// Resolve host
	struct addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	struct addrinfo* res = nullptr;
	if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) {
		printf("[input-sink] getaddrinfo('%s') failed\n", host);
		return false;
	}
	memcpy(&_addr, res->ai_addr, sizeof(sockaddr_in));
	int port = targetPort();
	_addr.sin_port = htons((uint16_t)port);
	freeaddrinfo(res);

	_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (_sock < 0) {
		printf("[input-sink] socket() failed\n");
		return false;
	}

	// Connect-style setup so mc_recv() works without specifying remote
	// (so probe-ACK heartbeat detection is simple)
	connect(_sock, (const sockaddr*)&_addr, sizeof(_addr));

	// Make the socket non-blocking. The trigger poll loop calls mc_recv()
	// with MSG_DONTWAIT to drain probe-ACKs, but on Windows MSG_DONTWAIT
	// is not supported and our compat shim defines it to 0 — meaning the
	// recv() would BLOCK forever waiting for data. Using FIONBIO at the
	// socket level gets the same "non-blocking" behavior cross-platform.
	set_non_blocking(_sock);

	GamepadDevice::listenButtonsGlobal(onButton);

	// Start the trigger polling + failover detection thread
	_triggerRun.store(true);
	_triggerThread = std::thread(triggerPollLoop);

	// Start the deferred-send thread for the T+1ms redundancy
	_redundantRun.store(true);
	_redundantThread = std::thread(redundantSendLoop);

	_active = true;
	char ipstr[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &_addr.sin_addr, ipstr, sizeof(ipstr));
	printf("[input-sink] ready â†’ %s:%d slot %d (19-byte seq+ts + redundant send)\n",
	       ipstr, port, _slot);
	return true;
}

void setBackupServer(const char* host)
{
	if (!host || !*host) {
		// Disable existing standby
		if (_backupSock >= 0) {
			mc_closesocket(_backupSock);
			_backupSock = -1;
		}
		_hasBackup.store(false, std::memory_order_release);
		return;
	}

	struct addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	struct addrinfo* res = nullptr;
	if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) {
		printf("[input-sink] backup getaddrinfo('%s') failed\n", host);
		return;
	}
	memcpy(&_backupAddr, res->ai_addr, sizeof(sockaddr_in));
	int backupPort = targetPort();
	_backupAddr.sin_port = htons((uint16_t)backupPort);
	freeaddrinfo(res);

	if (_backupSock >= 0) mc_closesocket(_backupSock);
	_backupSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (_backupSock < 0) {
		printf("[input-sink] backup socket() failed\n");
		return;
	}
	connect(_backupSock, (const sockaddr*)&_backupAddr, sizeof(_backupAddr));
	set_non_blocking(_backupSock);   // see init() above for why

	_hasBackup.store(true, std::memory_order_release);
	char ipstr[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &_backupAddr.sin_addr, ipstr, sizeof(ipstr));
	printf("[input-sink] hot-standby ready â†’ %s:%d\n", ipstr, backupPort);
}

void directUpdate(uint16_t buttons, uint8_t newLt, uint8_t newRt)
{
	if (!_active || _sock < 0) return;
	_buttons = buttons;
	// Write triggers to the global lt[]/rt[] so buildPacket reads them
	int gp = _gamepadPort.load(std::memory_order_relaxed);
	int tp = (gp >= 0 && gp < 4) ? gp : _slot;
	lt[tp] = (uint16_t)newLt << 8;
	rt[tp] = (uint16_t)newRt << 8;
	_buttonChanges.fetch_add(1, std::memory_order_relaxed);
	// Arm E2E probe â€” same as onButton path
	int64_t zero = 0;
	_probeStartUs.compare_exchange_strong(zero, nowUs(), std::memory_order_relaxed);
	sendState();
}

void shutdown()
{
	if (!_active) return;
	_triggerRun.store(false);
	_redundantRun.store(false);
	_pendingCv.notify_all();
	GamepadDevice::unlistenButtonsGlobal(onButton);
	if (_triggerThread.joinable()) _triggerThread.join();
	if (_redundantThread.joinable()) _redundantThread.join();
	if (_sock >= 0) { mc_closesocket(_sock); _sock = -1; }
	if (_backupSock >= 0) { mc_closesocket(_backupSock); _backupSock = -1; }
	_active = false;
	printf("[input-sink] shutdown\n");
}

bool active() { return _active; }

void onVisualChange()
{
	int64_t start = _probeStartUs.exchange(0, std::memory_order_relaxed);
	if (start == 0) return;

	double ms = (nowUs() - start) / 1000.0;
	_e2eLastMs.store(ms, std::memory_order_relaxed);
	_e2eProbes.fetch_add(1, std::memory_order_relaxed);

	double prev = _e2eEmaMs.load(std::memory_order_relaxed);
	double ema = (prev == 0.0) ? ms : prev + (ms - prev) * 0.125;
	_e2eEmaMs.store(ema, std::memory_order_relaxed);

	double curMin = _e2eMinMs.load(std::memory_order_relaxed);
	if (ms < curMin) _e2eMinMs.store(ms, std::memory_order_relaxed);
	double curMax = _e2eMaxMs.load(std::memory_order_relaxed);
	if (ms > curMax) _e2eMaxMs.store(ms, std::memory_order_relaxed);
}

Stats getStats()
{
	Stats s{};
	s.packetsSent     = _packetsSent.load(std::memory_order_relaxed);
	s.redundantSends  = _redundantSends.load(std::memory_order_relaxed);
	s.failovers       = _failovers.load(std::memory_order_relaxed);
	s.buttonChanges   = _buttonChanges.load(std::memory_order_relaxed);
	s.triggerChanges  = _triggerChanges.load(std::memory_order_relaxed);
	s.lastSendUs      = _lastSendUs.load(std::memory_order_relaxed);
	s.onBackupServer  = _onBackup.load(std::memory_order_relaxed);
	s.hasBackup       = _hasBackup.load(std::memory_order_relaxed);

	int64_t now = nowUs();
	int64_t prevTime = _prevRateTime.load(std::memory_order_relaxed);
	uint64_t prevPkts = _prevPackets.load(std::memory_order_relaxed);
	if (prevTime > 0) {
		double dt = (now - prevTime) / 1000000.0;
		if (dt > 0.0)
			s.sendRateHz = (uint32_t)((s.packetsSent - prevPkts) / dt);
	}
	_prevRateTime.store(now, std::memory_order_relaxed);
	_prevPackets.store(s.packetsSent, std::memory_order_relaxed);

	s.e2eLastMs = _e2eLastMs.load(std::memory_order_relaxed);
	s.e2eEmaMs  = _e2eEmaMs.load(std::memory_order_relaxed);
	s.e2eMinMs  = _e2eMinMs.load(std::memory_order_relaxed);
	s.e2eMaxMs  = _e2eMaxMs.load(std::memory_order_relaxed);
	s.e2eProbes = _e2eProbes.load(std::memory_order_relaxed);
	if (s.e2eMinMs > 100000.0) s.e2eMinMs = 0.0;

	return s;
}

}
