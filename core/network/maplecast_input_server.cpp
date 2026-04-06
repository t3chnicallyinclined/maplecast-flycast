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
#include <unordered_set>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <time.h>
#include <fstream>
#include <sstream>

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

// Stick registration — username-based.
// Persistence: WS server drains _pendingStickEvents and forwards to the
// collector for SurrealDB writes; saveStickCache() also mirrors current
// state to ~/.maplecast/sticks.json so flycast survives a restart even
// when the collector is briefly unreachable.
struct StickBinding {
	uint32_t srcIP;
	uint16_t srcPort;
	char username[16];       // 4-12 chars, [a-zA-Z0-9_]
	char browserId[64];      // legacy compat (same as username for new registrations)
	int64_t lastInputUs;     // for online detection
	bool wasOnline;          // last reported online state — for edge detection
};
static std::vector<StickBinding> _stickBindings;
static std::mutex _stickMutex;
static std::vector<StickEvent> _pendingStickEvents;

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

// Forensics: every (srcIP, srcPort) we've ever received a UDP packet from,
// hashed to a single uint64. First-appearance is logged so we can identify
// rogue clients sending input. Lifetime = process lifetime; cleared on
// flycast restart. Memory cost is ~24 bytes per unique source — bounded by
// the number of distinct IP:port pairs that ever talk to us.
static std::unordered_set<uint64_t> _seenUdpSources;
static std::mutex _seenUdpSourcesMutex;
static inline uint64_t makeSourceKey(uint32_t ip, uint16_t port)
{
	return ((uint64_t)ip << 16) | (uint64_t)port;
}

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

	// Track state changes (also drives idle-kick — see ws_server checkIdleKick)
	if (buttons != p._prevButtons)
	{
		p._chgAccum++;
		p._prevButtons = buttons;
		p.lastChangeUs = now;
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

		// Forensics: first packet ever from this (ip, port) gets logged with
		// full context. Subsequent packets from the same source are silent.
		// Loopback (WS-forwarder) packets are skipped because they're our own
		// traffic and the WS server already logs join/leave for those.
		if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
		{
			uint64_t key = makeSourceKey(from.sin_addr.s_addr, from.sin_port);
			bool firstTime = false;
			{
				std::lock_guard<std::mutex> lock(_seenUdpSourcesMutex);
				firstTime = _seenUdpSources.insert(key).second;
			}
			if (firstTime)
			{
				uint32_t ip = ntohl(from.sin_addr.s_addr);
				uint16_t btnState = ((uint16_t)w3[2] << 8) | w3[3];
				printf("[input-server] FORENSIC: new UDP source %u.%u.%u.%u:%u — first packet (buttons=0x%04X len=%d)\n",
					(ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
					ntohs(from.sin_port), btnState, n);
			}
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
		{
			std::lock_guard<std::mutex> lock(_stickMutex);
			for (auto& b : _stickBindings)
			{
				if (b.srcIP == from.sin_addr.s_addr) {
					b.lastInputUs = nowUs();
					break;
				}
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

	// Rehydrate stick bindings from local hot-cache (~/.maplecast/sticks.json)
	// before threads start so the very first UDP packet from a previously-
	// registered stick routes correctly. Collector will overwrite this with
	// authoritative DB state when it next connects.
	loadStickCache();

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
			// Reset the idle-kick clock so a reconnect always gets a fresh
			// grace period (otherwise a long-disconnected player would be
			// kicked the moment they come back).
			_players[i].lastChangeUs = nowUs();
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
			// Seed lastChangeUs so the idle-kick clock starts from join time,
			// not from whatever stale value the slot had previously.
			_players[i].lastChangeUs = nowUs();
			// Clear any leftover source binding from a prior tenant — the
			// new player must explicitly bind a stick (via NOBD rhythm or
			// web-register) to get hardware input routed to this slot. This
			// is the second half of the disconnect-clears-srcIP fix: a fresh
			// join must NOT inherit a ghost stick binding from whoever held
			// the slot before us.
			_players[i].srcIP = 0;
			_players[i].srcPort = 0;
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
		printf("[input-server] P%d DISCONNECTED: %s (was %u.%u.%u.%u:%u)\n",
			slot + 1, _players[slot].name,
			(ntohl(_players[slot].srcIP) >> 24) & 0xFF,
			(ntohl(_players[slot].srcIP) >> 16) & 0xFF,
			(ntohl(_players[slot].srcIP) >>  8) & 0xFF,
			(ntohl(_players[slot].srcIP)      ) & 0xFF,
			ntohs(_players[slot].srcPort));

		_players[slot].connected = false;
		_players[slot].type = InputType::None;

		// CRITICAL: clear the source binding so a NOBD stick that was routing
		// to this slot via findSlotByIP() stops doing so the moment the
		// owning WebSocket disconnects. Without this, a stale srcIP from a
		// previous session keeps the stick → slot binding alive forever and
		// any browser that re-joins under a matching id silently inherits
		// the orphan. This is the root of the "drifter ghost" class of bugs.
		_players[slot].srcIP = 0;
		_players[slot].srcPort = 0;

		// Also clear the id so a stale getRegisteredBrowserId() match against
		// the now-empty slot can't re-bind a stick by accident. The id is
		// rewritten on the next registerPlayer() call from a real join.
		_players[slot].id[0] = '\0';

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

int findIdlePlayer(int64_t thresholdUs)
{
	int64_t now = nowUs();
	std::lock_guard<std::mutex> lock(_registryMutex);
	for (int i = 0; i < 2; i++)
	{
		if (!_players[i].connected) continue;
		// Skip slots that haven't established a baseline yet (lastChangeUs==0
		// would otherwise look like decades-stale).
		if (_players[i].lastChangeUs == 0) continue;
		if (now - _players[i].lastChangeUs > thresholdUs)
			return i;
	}
	return -1;
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
	// Called from the UDP thread which is the only writer, so the
	// pointer it returns is stable for the duration of one packet's
	// processing. The lock guards against concurrent installs from
	// the WS thread (collector pushes / cache loads).
	std::lock_guard<std::mutex> lock(_stickMutex);
	for (const auto& b : _stickBindings)
		if (b.srcIP == srcIP)  // match by IP only, port can change
			return b.browserId;
	return nullptr;
}

const char* getRegisteredUsername(uint32_t srcIP, uint16_t srcPort)
{
	std::lock_guard<std::mutex> lock(_stickMutex);
	for (const auto& b : _stickBindings)
		if (b.srcIP == srcIP)
			return b.username[0] ? b.username : b.browserId;
	return nullptr;
}

// Append a StickEvent to the pending queue. Caller must hold _stickMutex.
static void enqueueStickEventLocked(StickEventKind kind, const char* username,
                                    uint32_t srcIP, uint16_t srcPort)
{
	StickEvent ev = {};
	ev.kind = kind;
	strncpy(ev.username, username, sizeof(ev.username) - 1);
	ev.srcIP = srcIP;
	ev.srcPort = srcPort;
	ev.ts = (int64_t)time(nullptr);
	_pendingStickEvents.push_back(ev);
}

void registerStick(uint32_t srcIP, uint16_t srcPort, const char* identifier)
{
	bool created = false;
	{
		std::lock_guard<std::mutex> lock(_stickMutex);
		bool updated = false;
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
				b.wasOnline = true;
				updated = true;
				break;
			}
		}
		if (!updated) {
			StickBinding b = {};
			b.srcIP = srcIP;
			b.srcPort = srcPort;
			strncpy(b.username, identifier, sizeof(b.username) - 1);
			strncpy(b.browserId, identifier, sizeof(b.browserId) - 1);
			b.lastInputUs = nowUs();
			b.wasOnline = true;
			_stickBindings.push_back(b);
			created = true;
		}
		enqueueStickEventLocked(StickEventKind::Register, identifier, srcIP, srcPort);
	}
	(void)created;
	saveStickCache();   // hot-cache survives flycast restart even if collector is down
}

void unregisterStick(const char* identifier)
{
	bool removed = false;
	{
		std::lock_guard<std::mutex> lock(_stickMutex);
		auto it = std::remove_if(_stickBindings.begin(), _stickBindings.end(),
			[identifier](const StickBinding& b) {
				return strcmp(b.browserId, identifier) == 0 ||
				       strcmp(b.username, identifier) == 0;
			});
		if (it != _stickBindings.end()) {
			removed = true;
			_stickBindings.erase(it, _stickBindings.end());
			enqueueStickEventLocked(StickEventKind::Unregister, identifier, 0, 0);
		}
	}
	if (removed) {
		printf("[input-server] Stick unregistered for %s\n", identifier);
		saveStickCache();
	}
}

bool isStickOnline(const char* username)
{
	std::lock_guard<std::mutex> lock(_stickMutex);
	int64_t now = nowUs();
	for (const auto& b : _stickBindings)
		if ((strcmp(b.username, username) == 0 || strcmp(b.browserId, username) == 0)
			&& (now - b.lastInputUs) < 10000000)  // 10 seconds
			return true;
	return false;
}

StickInfo getStickInfo(const char* username)
{
	std::lock_guard<std::mutex> lock(_stickMutex);
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
	std::lock_guard<std::mutex> lock(_stickMutex);
	return (int)_stickBindings.size();
}

// ==================== Stick persistence — public API ====================

std::vector<StickEvent> drainStickEvents()
{
	std::lock_guard<std::mutex> lock(_stickMutex);

	// Synthesize Online/Offline edge events from lastInputUs vs wasOnline.
	// Cheap: at most 2 entries on the typical 2-stick install.
	int64_t now = nowUs();
	for (auto& b : _stickBindings)
	{
		bool onlineNow = (now - b.lastInputUs) < 10000000;
		if (onlineNow != b.wasOnline)
		{
			enqueueStickEventLocked(
				onlineNow ? StickEventKind::Online : StickEventKind::Offline,
				b.username, b.srcIP, b.srcPort);
			b.wasOnline = onlineNow;
		}
	}

	std::vector<StickEvent> out;
	out.swap(_pendingStickEvents);
	return out;
}

std::vector<StickSnapshot> snapshotStickBindings()
{
	std::lock_guard<std::mutex> lock(_stickMutex);
	std::vector<StickSnapshot> out;
	out.reserve(_stickBindings.size());
	for (const auto& b : _stickBindings) {
		StickSnapshot s = {};
		strncpy(s.username, b.username, sizeof(s.username) - 1);
		s.srcIP = b.srcIP;
		s.srcPort = b.srcPort;
		s.lastInputUs = b.lastInputUs;
		out.push_back(s);
	}
	return out;
}

void installStickBindings(const std::vector<StickSnapshot>& snapshots)
{
	{
		std::lock_guard<std::mutex> lock(_stickMutex);
		for (const auto& s : snapshots) {
			if (!s.username[0]) continue;
			bool found = false;
			for (auto& b : _stickBindings) {
				if (strcmp(b.username, s.username) == 0) {
					b.srcIP = s.srcIP;
					b.srcPort = s.srcPort;
					if (s.lastInputUs > b.lastInputUs)
						b.lastInputUs = s.lastInputUs;
					found = true;
					break;
				}
			}
			if (!found) {
				StickBinding b = {};
				strncpy(b.username, s.username, sizeof(b.username) - 1);
				strncpy(b.browserId, s.username, sizeof(b.browserId) - 1);
				b.srcIP = s.srcIP;
				b.srcPort = s.srcPort;
				b.lastInputUs = s.lastInputUs;
				b.wasOnline = false;   // remote install — wait for first packet to flip
				_stickBindings.push_back(b);
			}
		}
	}
	printf("[input-server] Installed %zu stick binding(s) from cache/collector\n",
		snapshots.size());
}

// --- Local hot-cache (~/.maplecast/sticks.json) ---

static std::string stickCachePath()
{
	const char* home = getenv("HOME");
	if (!home || !*home) {
		struct passwd* pw = getpwuid(getuid());
		if (pw) home = pw->pw_dir;
	}
	if (!home || !*home) return std::string();
	std::string dir = std::string(home) + "/.maplecast";
	mkdir(dir.c_str(), 0700);   // EEXIST ignored
	return dir + "/sticks.json";
}

bool saveStickCache()
{
	std::string path = stickCachePath();
	if (path.empty()) return false;

	auto snaps = snapshotStickBindings();   // takes its own lock

	// Atomic write: temp + rename so a crash mid-write can't corrupt the cache.
	std::string tmp = path + ".tmp";
	std::ofstream f(tmp, std::ios::trunc);
	if (!f) {
		printf("[input-server] saveStickCache: cannot open %s\n", tmp.c_str());
		return false;
	}
	f << "[\n";
	for (size_t i = 0; i < snaps.size(); i++) {
		const auto& s = snaps[i];
		struct in_addr ia;
		ia.s_addr = s.srcIP;
		f << "  {\"username\":\"" << s.username
		  << "\",\"ip\":\"" << inet_ntoa(ia)
		  << "\",\"port\":" << ntohs(s.srcPort)
		  << ",\"last_input_us\":" << s.lastInputUs
		  << "}" << (i + 1 < snaps.size() ? "," : "") << "\n";
	}
	f << "]\n";
	f.close();
	if (rename(tmp.c_str(), path.c_str()) != 0) {
		printf("[input-server] saveStickCache: rename failed: %s\n", strerror(errno));
		return false;
	}
	return true;
}

bool loadStickCache()
{
	std::string path = stickCachePath();
	if (path.empty()) return false;

	std::ifstream f(path);
	if (!f) return false;   // no cache yet — first run

	std::stringstream ss;
	ss << f.rdbuf();
	std::string text = ss.str();

	// Tiny hand-rolled parser to avoid pulling in nlohmann::json here. The
	// file we just wrote ourselves is well-formed; if a user hand-edits it
	// and breaks it, we silently fall through and start clean.
	std::vector<StickSnapshot> loaded;
	size_t pos = 0;
	while ((pos = text.find('{', pos)) != std::string::npos) {
		size_t end = text.find('}', pos);
		if (end == std::string::npos) break;
		std::string obj = text.substr(pos, end - pos + 1);
		pos = end + 1;

		auto extractStr = [&](const char* key, std::string& out) {
			std::string needle = std::string("\"") + key + "\":\"";
			size_t k = obj.find(needle);
			if (k == std::string::npos) return false;
			k += needle.size();
			size_t e = obj.find('"', k);
			if (e == std::string::npos) return false;
			out = obj.substr(k, e - k);
			return true;
		};
		auto extractNum = [&](const char* key, long long& out) {
			std::string needle = std::string("\"") + key + "\":";
			size_t k = obj.find(needle);
			if (k == std::string::npos) return false;
			k += needle.size();
			out = strtoll(obj.c_str() + k, nullptr, 10);
			return true;
		};

		std::string username, ip;
		long long port = 0, lastInputUs = 0;
		if (!extractStr("username", username) || !extractStr("ip", ip)) continue;
		extractNum("port", port);
		extractNum("last_input_us", lastInputUs);

		StickSnapshot s = {};
		strncpy(s.username, username.c_str(), sizeof(s.username) - 1);
		s.srcIP = inet_addr(ip.c_str());
		s.srcPort = htons((uint16_t)port);
		s.lastInputUs = (int64_t)lastInputUs;
		loaded.push_back(s);
	}

	if (!loaded.empty())
		installStickBindings(loaded);
	printf("[input-server] Loaded %zu stick binding(s) from %s\n",
		loaded.size(), path.c_str());
	return true;
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

} // namespace maplecast_input
