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
#include <algorithm>
#include <vector>

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
static std::thread _bufferThread;
static int _udpSock = -1;

// ==================== Input Buffer (Fairness) ====================
// Coalesces inputs into N-ms windows so ping differences don't give
// one player a frame advantage. Each slot's latest input is staged,
// then flushed to kcode[] every _bufferMs milliseconds.
// _bufferMs=0 means instant (no buffer, raw arcade mode).

static std::atomic<int> _bufferMs{0};       // configurable per-match
static std::atomic<bool> _bufferActive{false};

struct StagedInput {
	uint16_t buttons = 0xFFFF;
	uint8_t lt = 0;
	uint8_t rt = 0;
	std::atomic<bool> dirty{false};         // new input since last flush
};
static StagedInput _staged[2];

// Buffer negotiation state
struct BufferNegotiation {
	bool pending = false;
	int proposedMs = 0;
	bool p1Accepted = false;
	bool p2Accepted = false;
};
static BufferNegotiation _negotiation;

// Player registry — THE single source of truth
static PlayerInfo _players[2] = {};
static std::mutex _registryMutex;

// Telemetry
static std::atomic<uint64_t> _totalPackets{0};

// Stick registration — username-based
struct StickBinding {
	uint32_t srcIP;
	uint16_t srcPort;
	char username[16];       // 4-12 chars, [a-zA-Z0-9_]
	char browserId[64];      // legacy compat (same as username for new registrations)
	int64_t lastInputUs;     // for online detection
};
static std::vector<StickBinding> _stickBindings;

// Rhythm registration in progress (legacy)
static bool _registering = false;
static char _registerBrowserId[64] = {};

// Web registration — simpler "any press" mode
static bool _webRegistering = false;
static char _webRegisterUsername[16] = {};
static int64_t _webRegisterStartUs = 0;

struct RhythmTracker {
	uint32_t srcIP;
	uint16_t srcPort;
	uint16_t prevButtons;
	int tapCount;        // taps in current burst
	int burstCount;      // completed bursts (need 2)
	int64_t lastTapUs;   // time of last tap
	int64_t burstEndUs;  // when first burst ended
};
static std::vector<RhythmTracker> _rhythmTrackers;

static inline int64_t nowUs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

// Flush staged input to kcode[] — called by buffer thread or directly
static void flushSlot(int slot)
{
	if (slot < 0 || slot > 1) return;
	StagedInput& s = _staged[slot];
	if (!s.dirty.load(std::memory_order_relaxed)) return;
	kcode[slot] = s.buttons | 0xFFFF0000;
	lt[slot] = (uint16_t)s.lt << 8;
	rt[slot] = (uint16_t)s.rt << 8;
	s.dirty.store(false, std::memory_order_relaxed);
}

// Buffer thread — flushes staged inputs every _bufferMs
static void bufferThreadLoop()
{
	printf("[input-server] Buffer thread started\n");
	while (_active.load()) {
		int ms = _bufferMs.load(std::memory_order_relaxed);
		if (ms <= 0) {
			// Buffer disabled — sleep and check again
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(ms));
		if (_bufferActive.load(std::memory_order_relaxed)) {
			flushSlot(0);
			flushSlot(1);
		}
	}
	printf("[input-server] Buffer thread stopped\n");
}

// Write button state to kcode[]/lt[]/rt[] globals AND update player stats
static void updateSlot(int slot, uint8_t ltVal, uint8_t rtVal, uint16_t buttons)
{
	if (slot < 0 || slot > 1) return;

	int ms = _bufferMs.load(std::memory_order_relaxed);
	if (ms > 0 && _bufferActive.load(std::memory_order_relaxed)) {
		// Buffer active — stage input, buffer thread flushes on tick
		StagedInput& s = _staged[slot];
		s.buttons = buttons;
		s.lt = ltVal;
		s.rt = rtVal;
		s.dirty.store(true, std::memory_order_relaxed);
	} else {
		// No buffer — write directly (raw arcade mode)
		kcode[slot] = buttons | 0xFFFF0000;
		lt[slot] = (uint16_t)ltVal << 8;
		rt[slot] = (uint16_t)rtVal << 8;
	}

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

// autoAssignUDP removed — NOBD sticks must register via browser first

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

		// Web registration — any press from unregistered stick binds it
		if (_webRegistering && from.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
		{
			uint16_t buttons = ((uint16_t)w3[2] << 8) | w3[3];
			// Any button pressed (active-low: not 0xFFFF)
			if (buttons != 0xFFFF)
			{
				// Only register if this IP isn't already registered
				if (!getRegisteredBrowserId(from.sin_addr.s_addr, from.sin_port))
				{
					registerStick(from.sin_addr.s_addr, from.sin_port, _webRegisterUsername);
					printf("[input-server] WEB REGISTER: stick %08X bound to '%s'\n",
						from.sin_addr.s_addr, _webRegisterUsername);
					_webRegistering = false;
				}
			}
		}

		// Update last input time for online tracking
		for (auto& b : _stickBindings)
		{
			if (b.srcIP == from.sin_addr.s_addr) {
				b.lastInputUs = nowUs();
				break;
			}
		}

		// Check for stick registration rhythm (only from real NOBD sticks, not loopback)
		if (_registering && from.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
		{
			uint16_t buttons = ((uint16_t)w3[2] << 8) | w3[3];
			int64_t now = nowUs();

			// Find or create tracker for this source
			RhythmTracker* rt = nullptr;
			for (auto& t : _rhythmTrackers)
				if (t.srcIP == from.sin_addr.s_addr) { rt = &t; break; }
			if (!rt) {
				_rhythmTrackers.push_back({from.sin_addr.s_addr, from.sin_port, 0xFFFF, 0, 0, 0, 0});
				rt = &_rhythmTrackers.back();
			}

			// Detect button press: any bit went from 1→0 (active-low)
			uint16_t newPresses = rt->prevButtons & ~buttons;
			rt->prevButtons = buttons;

			if (newPresses)
			{
				int64_t sinceLast = now - rt->lastTapUs;
				rt->lastTapUs = now;

				if (rt->burstCount == 0)
				{
					// First burst
					if (sinceLast > 2000000) { rt->tapCount = 1; } // reset if >2s gap
					else { rt->tapCount++; }

					if (rt->tapCount >= 5) {
						rt->burstCount = 1;
						rt->burstEndUs = now;
						rt->tapCount = 0;
						printf("[input-server] Registration: burst 1 detected from %08X\n", rt->srcIP);
					}
				}
				else if (rt->burstCount == 1)
				{
					int64_t sinceBurst = now - rt->burstEndUs;
					if (sinceBurst < 500000) {
						// Too fast after first burst — still part of burst 1, ignore
					} else if (sinceBurst > 3000000) {
						// Too slow — reset
						rt->burstCount = 0;
						rt->tapCount = 1;
					} else {
						// In the pause window — count second burst
						rt->tapCount++;
						if (rt->tapCount >= 5) {
							// Pattern complete!
							registerStick(rt->srcIP, rt->srcPort, _registerBrowserId);
							_registering = false;
							_rhythmTrackers.clear();
							printf("[input-server] STICK REGISTERED to %s via rhythm!\n", _registerBrowserId);
						}
					}
				}
			}
		}

		// NOBD stick input routing — only registered sticks with active slots
		if (slot < 0 && from.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
		{
			// First check if already assigned to a slot
			slot = findSlotByIP(from.sin_addr.s_addr);

			// If not assigned, check if this stick is registered to a browser user
			// who has an active slot — if so, bind this stick to that slot
			if (slot < 0)
			{
				const char* browserId = getRegisteredBrowserId(from.sin_addr.s_addr, from.sin_port);
				if (browserId)
				{
					// Find which slot this browser user is in
					for (int i = 0; i < 2; i++)
					{
						if (_players[i].connected && strncmp(_players[i].id, browserId, 8) == 0)
						{
							// Bind this stick's IP to the slot
							_players[i].srcIP = from.sin_addr.s_addr;
							_players[i].srcPort = from.sin_port;
							_players[i].type = InputType::NobdUDP;
							snprintf(_players[i].device, sizeof(_players[i].device), "NOBD Stick");
							slot = i;
							printf("[input-server] P%d NOBD stick bound to %s\n", i + 1, browserId);
							break;
						}
					}
				}
				// Unregistered sticks: silently ignore (no auto-assign)
			}
		}

		// Tagged packets from WebSocket still need slot lookup
		if (slot < 0 && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK))
		{
			// Already handled above via tagged[0]
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
	_bufferThread = std::thread(bufferThreadLoop);

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
	if (_bufferThread.joinable()) _bufferThread.join();

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

void startStickRegistration(const char* browserId)
{
	strncpy(_registerBrowserId, browserId, sizeof(_registerBrowserId) - 1);
	_rhythmTrackers.clear();
	_registering = true;
	printf("[input-server] Registration started for %s — tap any button 5x, pause, 5x again\n", browserId);
}

void cancelStickRegistration()
{
	_registering = false;
	_rhythmTrackers.clear();
	printf("[input-server] Registration cancelled\n");
}

bool isRegistering()
{
	return _registering;
}

const char* getRegisteredBrowserId(uint32_t srcIP, uint16_t srcPort)
{
	for (const auto& b : _stickBindings)
		if (b.srcIP == srcIP)  // match by IP only, port can change
			return b.browserId;
	return nullptr;
}

const char* getRegisteredUsername(uint32_t srcIP, uint16_t srcPort)
{
	for (const auto& b : _stickBindings)
		if (b.srcIP == srcIP)
			return b.username[0] ? b.username : b.browserId;
	return nullptr;
}

void registerStick(uint32_t srcIP, uint16_t srcPort, const char* identifier)
{
	// Update existing binding for this IP, or existing binding for this username
	for (auto& b : _stickBindings)
	{
		if (b.srcIP == srcIP || strcmp(b.username, identifier) == 0 ||
			strcmp(b.browserId, identifier) == 0) {
			b.srcIP = srcIP;
			b.srcPort = srcPort;
			strncpy(b.username, identifier, sizeof(b.username) - 1);
			strncpy(b.browserId, identifier, sizeof(b.browserId) - 1);
			b.lastInputUs = nowUs();
			return;
		}
	}
	StickBinding b = {};
	b.srcIP = srcIP;
	b.srcPort = srcPort;
	strncpy(b.username, identifier, sizeof(b.username) - 1);
	strncpy(b.browserId, identifier, sizeof(b.browserId) - 1);
	b.lastInputUs = nowUs();
	_stickBindings.push_back(b);
}

void unregisterStick(const char* identifier)
{
	_stickBindings.erase(std::remove_if(_stickBindings.begin(), _stickBindings.end(),
		[identifier](const StickBinding& b) {
			return strcmp(b.browserId, identifier) == 0 ||
			       strcmp(b.username, identifier) == 0;
		}),
		_stickBindings.end());
	printf("[input-server] Stick unregistered for %s\n", identifier);
}

bool isStickOnline(const char* username)
{
	int64_t now = nowUs();
	for (const auto& b : _stickBindings)
		if ((strcmp(b.username, username) == 0 || strcmp(b.browserId, username) == 0)
			&& (now - b.lastInputUs) < 10000000)  // 10 seconds
			return true;
	return false;
}

StickInfo getStickInfo(const char* username)
{
	StickInfo info = {};
	int64_t now = nowUs();
	for (const auto& b : _stickBindings)
	{
		if (strcmp(b.username, username) == 0 || strcmp(b.browserId, username) == 0)
		{
			info.registered = true;
			info.online = (now - b.lastInputUs) < 10000000;
			strncpy(info.username, b.username, sizeof(info.username) - 1);
			info.srcIP = b.srcIP;
			info.srcPort = b.srcPort;
			info.lastInputUs = b.lastInputUs;
			return info;
		}
	}
	return info;
}

int registeredStickCount()
{
	return (int)_stickBindings.size();
}

// --- Web registration ---

void startWebRegistration(const char* username)
{
	strncpy(_webRegisterUsername, username, sizeof(_webRegisterUsername) - 1);
	_webRegisterStartUs = nowUs();
	_webRegistering = true;
	printf("[input-server] Web registration started for '%s' — press any button\n", username);
}

void cancelWebRegistration()
{
	_webRegistering = false;
	_webRegisterUsername[0] = 0;
	printf("[input-server] Web registration cancelled\n");
}

bool isWebRegistering()
{
	// Auto-cancel after 30 seconds
	if (_webRegistering && (nowUs() - _webRegisterStartUs) > 30000000) {
		printf("[input-server] Web registration timed out for '%s'\n", _webRegisterUsername);
		_webRegistering = false;
	}
	return _webRegistering;
}

const char* webRegisteringUsername()
{
	return _webRegisterUsername;
}

bool isValidUsername(const char* name)
{
	if (!name) return false;
	int len = 0;
	for (const char* p = name; *p; p++, len++)
	{
		char c = *p;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_'))
			return false;
	}
	return len >= 4 && len <= 12;
}

// ==================== Input Buffer API ====================

void setBufferMs(int ms)
{
	if (ms < 0) ms = 0;
	if (ms > 16) ms = 16;  // cap at one frame
	int prev = _bufferMs.exchange(ms);
	_bufferActive.store(ms > 0, std::memory_order_relaxed);
	if (ms != prev)
		printf("[input-server] Input buffer: %dms (%s)\n", ms, ms > 0 ? "fairness" : "raw arcade");
}

int getBufferMs()
{
	return _bufferMs.load(std::memory_order_relaxed);
}

int getRecommendedBufferMs()
{
	// Compute from ping difference between connected players
	// Uses half-RTT approximation from lastPacketUs jitter
	if (!_players[0].connected || !_players[1].connected) return 0;

	// Use packets-per-second as proxy: higher PPS = lower latency
	// For actual ping, we'd need the WS pong RTT. For now, estimate
	// from input type: NOBD = ~0ms, BrowserWS = use WS ping from status
	int p1type = (_players[0].type == InputType::NobdUDP) ? 0 : 1;
	int p2type = (_players[1].type == InputType::NobdUDP) ? 0 : 1;

	// If both same type, small buffer. If mixed, larger buffer.
	if (p1type == 0 && p2type == 0) return 0;   // both NOBD, no buffer needed
	if (p1type == 1 && p2type == 1) return 3;   // both browser, small buffer
	return 5;  // mixed NOBD + browser
}

void proposeBuffer(int ms)
{
	_negotiation.pending = true;
	_negotiation.proposedMs = ms;
	_negotiation.p1Accepted = false;
	_negotiation.p2Accepted = false;
	printf("[input-server] Buffer proposed: %dms\n", ms);
}

bool acceptBuffer(int slot)
{
	if (!_negotiation.pending) return false;
	if (slot == 0) _negotiation.p1Accepted = true;
	if (slot == 1) _negotiation.p2Accepted = true;

	if (_negotiation.p1Accepted && _negotiation.p2Accepted) {
		setBufferMs(_negotiation.proposedMs);
		_negotiation.pending = false;
		printf("[input-server] Buffer ACCEPTED by both: %dms\n", _negotiation.proposedMs);
		return true;  // both accepted
	}
	return false;
}

void rejectBuffer(int slot)
{
	if (!_negotiation.pending) return;
	_negotiation.pending = false;
	setBufferMs(0);
	printf("[input-server] Buffer REJECTED by P%d — raw arcade mode\n", slot + 1);
}

bool isBufferPending() { return _negotiation.pending; }
int getProposedBufferMs() { return _negotiation.proposedMs; }

} // namespace maplecast_input
