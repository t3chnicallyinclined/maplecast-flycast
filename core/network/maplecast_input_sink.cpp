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

static void sendState()
{
	if (_sock < 0) return;
	// Always read live analog trigger values — the button callback
	// only handles digital triggers, but Xbox LT/RT are analog axes.
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

}
