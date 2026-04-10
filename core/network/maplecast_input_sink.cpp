/*
	MapleCast Input Sink — implementation.
	See maplecast_input_sink.h for design.
*/
#include "maplecast_input_sink.h"
#include "input/gamepad_device.h"

#include <cstdio>
#include <cstring>
#include <atomic>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

namespace maplecast_input_sink
{

static int            _sock = -1;
static sockaddr_in    _addr{};
static int            _slot = 0;
static bool           _active = false;

// Accumulated button state (active-low, same as kcode[] format).
// Only modified by the ButtonListener callback on the SDL event thread.
static uint16_t       _buttons = 0xFFFF;
static uint8_t        _lt = 0;
static uint8_t        _rt = 0;

static void sendState()
{
	if (_sock < 0) return;
	uint8_t pkt[7] = {
		'P', 'C', (uint8_t)_slot,
		_lt, _rt,
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
	else if (key == DC_AXIS_LT) {
		_lt = pressed ? 0xFF : 0;
		sendState();
	}
	else if (key == DC_AXIS_RT) {
		_rt = pressed ? 0xFF : 0;
		sendState();
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

	// Register the ButtonListener on all gamepad devices
	GamepadDevice::listenButtonsGlobal(onButton);

	_active = true;
	char ipstr[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &_addr.sin_addr, ipstr, sizeof(ipstr));
	printf("[input-sink] ready → %s:7100 slot %d\n", ipstr, _slot);
	return true;
}

void shutdown()
{
	if (!_active) return;
	GamepadDevice::unlistenButtonsGlobal(onButton);
	if (_sock >= 0) { close(_sock); _sock = -1; }
	_active = false;
	printf("[input-sink] shutdown\n");
}

bool active() { return _active; }

}
