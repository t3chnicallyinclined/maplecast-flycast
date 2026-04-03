/*
	MapleCast Input Server — single source of truth for all player input.

	Architecture (inspired by NOBD firmware):
	  NOBD: GPIO → cmd9ReadyW3 (always fresh, ISR just reads it)
	  This: UDP thread → kcode[] atomics (always fresh, CMD9 just reads them)

	All input paths converge here:
	  - NOBD stick UDP → recvfrom thread → updateSlot()
	  - Browser gamepad → WebSocket → injectInput() → updateSlot()
	  - XDP (future) → ring buffer → updateSlot()

	One registry. One set of globals. One latency tracker.
*/
#include "types.h"
#include "maplecast_input_server.h"
#include "maplecast_telemetry.h"
#include "input/gamepad_device.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <mutex>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

// Gamepad globals — CMD9 reads these via ggpo::getLocalInput()
extern u32 kcode[4];
extern u16 rt[4], lt[4];

namespace maplecast_input
{

static std::atomic<bool> _active{false};
static std::thread _udpThread;
static int _udpSock = -1;

// Player registry — THE single source of truth
static PlayerInfo _players[2] = {};
static std::mutex _registryMutex;

// Telemetry
static std::atomic<uint64_t> _totalPackets{0};

static inline int64_t nowUs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

// Write button state to kcode[]/lt[]/rt[] globals AND update player stats
static void updateSlot(int slot, uint8_t ltVal, uint8_t rtVal, uint16_t buttons)
{
	if (slot < 0 || slot > 1) return;

	// Atomic write to gamepad globals — CMD9 reads these
	kcode[slot] = buttons | 0xFFFF0000;  // active-low, upper 16 bits set
	lt[slot] = (uint16_t)ltVal << 8;
	rt[slot] = (uint16_t)rtVal << 8;

	// Update player stats
	PlayerInfo& p = _players[slot];
	int64_t now = nowUs();
	p.lastPacketUs = now;
	p.lt = ltVal;
	p.rt = rtVal;

	// Track state changes
	if (buttons != p._prevButtons)
	{
		p._chgAccum++;
		p._prevButtons = buttons;
	}
	p.buttons = buttons;
	p._pktAccum++;

	// Update per-second rates every second
	if (now - p._lastRateTime >= 1000000)
	{
		int64_t elapsed = now - p._lastRateTime;
		p.packetsPerSec = (uint32_t)(p._pktAccum * 1000000LL / elapsed);
		p.changesPerSec = (uint32_t)(p._chgAccum * 1000000LL / elapsed);
		p._pktAccum = 0;
		p._chgAccum = 0;
		p._lastRateTime = now;
	}

	_totalPackets.fetch_add(1, std::memory_order_relaxed);
}

// Find which slot a source IP belongs to (-1 if unknown)
static int findSlotByIP(uint32_t srcIP)
{
	for (int i = 0; i < 2; i++)
		if (_players[i].connected && _players[i].srcIP == srcIP)
			return i;
	return -1;
}

// Auto-assign a NOBD stick by source IP
static int autoAssignUDP(uint32_t srcIP, uint16_t srcPort)
{
	std::lock_guard<std::mutex> lock(_registryMutex);

	// Already known?
	int slot = findSlotByIP(srcIP);
	if (slot >= 0) return slot;

	// Find first empty slot
	for (int i = 0; i < 2; i++)
	{
		if (!_players[i].connected)
		{
			_players[i].connected = true;
			_players[i].type = InputType::NobdUDP;
			_players[i].srcIP = srcIP;
			_players[i].srcPort = srcPort;
			_players[i].buttons = 0xFFFF;
			_players[i]._prevButtons = 0xFFFF;
			_players[i]._lastRateTime = nowUs();

			uint8_t *ip = (uint8_t *)&srcIP;
			snprintf(_players[i].id, sizeof(_players[i].id), "nobd_%u.%u.%u.%u",
				ip[0], ip[1], ip[2], ip[3]);
			snprintf(_players[i].name, sizeof(_players[i].name), "NOBD Stick");
			snprintf(_players[i].device, sizeof(_players[i].device),
				"NOBD %u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], ntohs(srcPort));

			printf("[input-server] P%d ASSIGNED: %s (%s)\n", i + 1, _players[i].device, _players[i].id);
			maplecast_telemetry::send("[input-server] P%d ASSIGNED: %s", i + 1, _players[i].device);
			return i;
		}
	}

	return -1;  // full
}

// UDP listener thread — receives NOBD stick and WebSocket-forwarded packets
static void udpThreadLoop(int port)
{
	printf("[input-server] UDP thread started on port %d\n", port);

	// SO_BUSY_POLL: spin-poll NIC for 10µs before sleeping
	int busy_poll = 10;
	setsockopt(_udpSock, SOL_SOCKET, SO_BUSY_POLL, &busy_poll, sizeof(busy_poll));

	struct timeval tv = { .tv_sec = 0, .tv_usec = 1000 };  // 1ms recv timeout
	setsockopt(_udpSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	uint8_t buf[64];
	struct sockaddr_in from;
	socklen_t fromLen;

	while (_active.load(std::memory_order_relaxed))
	{
		fromLen = sizeof(from);
		int n = recvfrom(_udpSock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromLen);
		if (n < 4) continue;

		const uint8_t *w3 = buf;
		int slot = -1;

		// 5-byte tagged from WebSocket: [slot][LT][RT][btn_hi][btn_lo]
		if (n >= 5 && buf[0] <= 1 && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK))
		{
			slot = buf[0];
			w3 = buf + 1;
		}

		// Legacy 4-byte W3 from NOBD stick: identify/assign by source IP
		if (slot < 0)
		{
			slot = findSlotByIP(from.sin_addr.s_addr);
			if (slot < 0)
				slot = autoAssignUDP(from.sin_addr.s_addr, from.sin_port);
		}

		if (slot < 0) continue;

		uint16_t buttons = ((uint16_t)w3[2] << 8) | w3[3];
		updateSlot(slot, w3[0], w3[1], buttons);
	}

	printf("[input-server] UDP thread stopped (%lu total packets)\n",
		_totalPackets.load());
}

// === Public API ===

bool init(int udpPort)
{
	if (_active) return true;

	memset(_players, 0, sizeof(_players));
	for (int i = 0; i < 2; i++)
	{
		_players[i].buttons = 0xFFFF;
		_players[i]._prevButtons = 0xFFFF;
		_players[i]._lastRateTime = nowUs();
	}

	// Create UDP socket
	_udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (_udpSock < 0)
	{
		printf("[input-server] socket failed: %s\n", strerror(errno));
		return false;
	}

	int reuse = 1;
	setsockopt(_udpSock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(udpPort);

	if (bind(_udpSock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		printf("[input-server] bind port %d failed: %s\n", udpPort, strerror(errno));
		close(_udpSock);
		_udpSock = -1;
		return false;
	}

	_active = true;
	_udpThread = std::thread(udpThreadLoop, udpPort);

	printf("[input-server] === READY === port %d\n", udpPort);
	printf("[input-server] waiting for players (NOBD UDP or browser WebSocket)\n");
	maplecast_telemetry::send("[input-server] ready on port %d", udpPort);
	return true;
}

void shutdown()
{
	if (!_active) return;
	_active = false;

	if (_udpSock >= 0) { close(_udpSock); _udpSock = -1; }
	if (_udpThread.joinable()) _udpThread.join();

	printf("[input-server] shutdown\n");
}

int registerPlayer(const char* id, const char* name, const char* device, InputType type)
{
	std::lock_guard<std::mutex> lock(_registryMutex);

	// Check if already registered (reconnect)
	for (int i = 0; i < 2; i++)
	{
		if (_players[i].connected && strncmp(_players[i].id, id, sizeof(_players[i].id)) == 0)
		{
			// Reconnect — update info, keep slot
			strncpy(_players[i].name, name, sizeof(_players[i].name) - 1);
			strncpy(_players[i].device, device, sizeof(_players[i].device) - 1);
			_players[i].type = type;
			printf("[input-server] P%d RECONNECTED: %s (%s)\n", i + 1, name, device);
			return i;
		}
	}

	// Find first empty slot
	for (int i = 0; i < 2; i++)
	{
		if (!_players[i].connected)
		{
			_players[i].connected = true;
			_players[i].type = type;
			_players[i].buttons = 0xFFFF;
			_players[i]._prevButtons = 0xFFFF;
			_players[i]._lastRateTime = nowUs();
			strncpy(_players[i].id, id, sizeof(_players[i].id) - 1);
			strncpy(_players[i].name, name, sizeof(_players[i].name) - 1);
			strncpy(_players[i].device, device, sizeof(_players[i].device) - 1);

			printf("[input-server] P%d JOINED: %s (%s) via %s\n", i + 1, name, device,
				type == InputType::BrowserWS ? "WebSocket" : "UDP");
			maplecast_telemetry::send("[input-server] P%d JOINED: %s (%s)", i + 1, name, device);
			return i;
		}
	}

	printf("[input-server] FULL — rejected %s (%s)\n", name, device);
	return -1;
}

void disconnectPlayer(int slot)
{
	if (slot < 0 || slot > 1) return;
	std::lock_guard<std::mutex> lock(_registryMutex);

	if (_players[slot].connected)
	{
		printf("[input-server] P%d DISCONNECTED: %s\n", slot + 1, _players[slot].name);
		_players[slot].connected = false;
		_players[slot].type = InputType::None;

		// Reset gamepad state to neutral
		kcode[slot] = 0xFFFFFFFF;
		lt[slot] = 0;
		rt[slot] = 0;
	}
}

void injectInput(int slot, uint8_t ltVal, uint8_t rtVal, uint16_t buttons)
{
	updateSlot(slot, ltVal, rtVal, buttons);
}

const PlayerInfo& getPlayer(int slot)
{
	static PlayerInfo empty = {};
	if (slot < 0 || slot > 1) return empty;
	return _players[slot];
}

int connectedCount()
{
	int count = 0;
	for (int i = 0; i < 2; i++)
		if (_players[i].connected) count++;
	return count;
}

bool active()
{
	return _active.load(std::memory_order_relaxed);
}

} // namespace maplecast_input
