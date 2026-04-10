/*
	MapleCast Input Sink — implementation.
	See maplecast_input_sink.h for design.
*/
#include "types.h"
#include "maplecast_input_sink.h"
#include "input/gamepad_device.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

extern u16 lt[4], rt[4];

namespace maplecast_input_sink
{

static int            _sock = -1;
static sockaddr_in    _addr{};
static int            _slot = 0;
static bool           _active = false;
static std::thread    _triggerThread;
static std::atomic<bool> _triggerRun{false};

// Accumulated button state (active-low, same as kcode[] format).
static uint16_t       _buttons = 0xFFFF;

// Stats
static std::atomic<uint64_t> _packetsSent{0};
static std::atomic<uint64_t> _buttonChanges{0};
static std::atomic<uint64_t> _triggerChanges{0};
static std::atomic<int64_t>  _lastSendUs{0};
static std::atomic<uint64_t> _prevPackets{0};
static std::atomic<int64_t>  _prevRateTime{0};
static std::atomic<uint32_t> _sendRateHz{0};

// E2E latency probe — lock-free, zero-cost on the hot path.
//
// When a button changes, we store the monotonic timestamp in
// _probeStartUs. When onVisualChange() is called (from the mirror
// WS thread on TA frame arrival with dirty pages), it reads the
// probe, computes the delta, stores the result, and clears the probe.
// If no probe is pending (0), onVisualChange is a single atomic load.
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

static void sendState()
{
	if (_sock < 0) return;
	uint8_t ltVal = (uint8_t)(lt[_slot] >> 8);
	uint8_t rtVal = (uint8_t)(rt[_slot] >> 8);
	uint8_t pkt[7] = {
		'P', 'C', (uint8_t)_slot,
		ltVal, rtVal,
		(uint8_t)(_buttons >> 8),
		(uint8_t)(_buttons & 0xFF)
	};
	sendto(_sock, pkt, sizeof(pkt), 0,
	       (const sockaddr*)&_addr, sizeof(_addr));
	_packetsSent.fetch_add(1, std::memory_order_relaxed);
	_lastSendUs.store(nowUs(), std::memory_order_relaxed);
}

// SDL ButtonListener — fires synchronously on the main thread the
// instant a button is pressed or released.
static void onButton(int port, DreamcastKey key, bool pressed)
{
	if (port != _slot) return;

	if (key <= DC_BTN_BITMAPPED_LAST) {
		if (pressed)
			_buttons &= ~(uint16_t)key;
		else
			_buttons |= (uint16_t)key;
		_buttonChanges.fetch_add(1, std::memory_order_relaxed);
		// Arm E2E probe — only if no probe is already pending.
		// This way we measure from the FIRST button change, not the last.
		int64_t zero = 0;
		_probeStartUs.compare_exchange_strong(zero, nowUs(), std::memory_order_relaxed);
		sendState();
	}
	// LT/RT handled by trigger poll thread (analog axes)
}

// Trigger polling thread — reads lt[]/rt[] at ~120Hz and sends when
// they change. Needed because analog triggers go through the axis path
// in flycast and never fire the ButtonListener callback.
static void triggerPollLoop()
{
	uint8_t lastLt = 0, lastRt = 0;
	while (_triggerRun.load(std::memory_order_relaxed)) {
		uint8_t curLt = (uint8_t)(lt[_slot] >> 8);
		uint8_t curRt = (uint8_t)(rt[_slot] >> 8);
		if (curLt != lastLt || curRt != lastRt) {
			lastLt = curLt;
			lastRt = curRt;
			_triggerChanges.fetch_add(1, std::memory_order_relaxed);
			sendState();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(8)); // ~120Hz
	}
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
	_addr.sin_port = htons(7100);
	freeaddrinfo(res);

	_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (_sock < 0) {
		printf("[input-sink] socket() failed\n");
		return false;
	}

	GamepadDevice::listenButtonsGlobal(onButton);

	// Start trigger polling thread for analog LT/RT
	_triggerRun.store(true);
	_triggerThread = std::thread(triggerPollLoop);

	_active = true;
	char ipstr[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &_addr.sin_addr, ipstr, sizeof(ipstr));
	printf("[input-sink] ready → %s:7100 slot %d\n", ipstr, _slot);
	return true;
}

void shutdown()
{
	if (!_active) return;
	_triggerRun.store(false);
	GamepadDevice::unlistenButtonsGlobal(onButton);
	if (_triggerThread.joinable()) _triggerThread.join();
	if (_sock >= 0) { close(_sock); _sock = -1; }
	_active = false;
	printf("[input-sink] shutdown\n");
}

bool active() { return _active; }

void onVisualChange()
{
	// Complete the E2E probe if one is pending.
	// Single atomic load — zero cost when no probe is armed.
	int64_t start = _probeStartUs.exchange(0, std::memory_order_relaxed);
	if (start == 0) return;  // no probe pending

	double ms = (nowUs() - start) / 1000.0;
	_e2eLastMs.store(ms, std::memory_order_relaxed);
	_e2eProbes.fetch_add(1, std::memory_order_relaxed);

	// EMA with factor 1/8
	double prev = _e2eEmaMs.load(std::memory_order_relaxed);
	double ema = (prev == 0.0) ? ms : prev + (ms - prev) * 0.125;
	_e2eEmaMs.store(ema, std::memory_order_relaxed);

	// Min/max
	double curMin = _e2eMinMs.load(std::memory_order_relaxed);
	if (ms < curMin) _e2eMinMs.store(ms, std::memory_order_relaxed);
	double curMax = _e2eMaxMs.load(std::memory_order_relaxed);
	if (ms > curMax) _e2eMaxMs.store(ms, std::memory_order_relaxed);
}

Stats getStats()
{
	Stats s{};
	s.packetsSent = _packetsSent.load(std::memory_order_relaxed);
	s.buttonChanges = _buttonChanges.load(std::memory_order_relaxed);
	s.triggerChanges = _triggerChanges.load(std::memory_order_relaxed);
	s.lastSendUs = _lastSendUs.load(std::memory_order_relaxed);

	// Compute send rate (packets/sec) from delta
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
	if (s.e2eMinMs > 100000.0) s.e2eMinMs = 0.0;  // not yet measured

	return s;
}

}
