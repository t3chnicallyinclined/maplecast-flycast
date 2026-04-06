/*
	MapleCast Input Server — single source of truth for all player input.

	Every input path (NOBD UDP, browser WebSocket, XDP, SDL) feeds through
	this server. One registry, one set of kcode[] writes, one place to
	track latency, jitter, and buffer depth per player.

	The game reads kcode[]/lt[]/rt[] at CMD9 time — always fresh.
*/
#pragma once
#include <cstdint>
#include <vector>

namespace maplecast_input
{

enum class InputType { None, NobdUDP, BrowserWS, XDP, LocalUSB };

struct PlayerInfo {
	bool connected;
	InputType type;
	char id[32];          // NOBD code, browser UUID, etc.
	char name[32];        // display name
	char device[48];      // "NOBD-7X4K", "PS4 Controller", etc.
	uint32_t srcIP;       // source IP (network byte order)
	uint16_t srcPort;     // source port

	// Live button state
	uint16_t buttons;     // DC active-low
	uint8_t lt, rt;       // triggers 0-255

	// Latency tracking
	int64_t lastPacketUs; // timestamp of last received packet
	int64_t lastChangeUs; // timestamp of last button-state change (idle detection)
	int64_t avgE2eUs;     // rolling average E2E latency
	int64_t avgJitterUs;  // rolling average jitter
	uint32_t packetsPerSec;
	uint32_t changesPerSec;

	// Stats accumulators (reset every second)
	uint32_t _pktAccum;
	uint32_t _chgAccum;
	int64_t _lastRateTime;
	uint16_t _prevButtons;
};

// Initialize the input server — starts UDP listener thread
bool init(int udpPort = 7100);

// Shutdown all input threads
void shutdown();

// Register a player from WebSocket join (returns assigned slot, -1 if full)
int registerPlayer(const char* id, const char* name, const char* device, InputType type);

// Unregister / disconnect a player
void disconnectPlayer(int slot);

// Get player info for slot (0=P1, 1=P2)
const PlayerInfo& getPlayer(int slot);

// Get number of connected players
int connectedCount();

// Idle detection — returns the lowest connected slot that hasn't sent a
// button-state CHANGE in `thresholdUs` microseconds, or -1 if everyone
// is fresh. Pps (gamepad poll rate) is ignored — we only care about
// changes, so a player holding nothing still counts as idle.
int findIdlePlayer(int64_t thresholdUs);

// Called by WebSocket message handler to inject browser gamepad input
void injectInput(int slot, uint8_t lt, uint8_t rt, uint16_t buttons);

// Is the input server running?
bool active();

// Stick registration — bind a NOBD stick to a username
// Rhythm mode: tap any button 5x, pause, 5x again
void startStickRegistration(const char* browserId);
void cancelStickRegistration();
bool isRegistering();

// Web registration — first unregistered stick to send any input gets bound
void startWebRegistration(const char* username);
void cancelWebRegistration();
bool isWebRegistering();
const char* webRegisteringUsername();  // who's waiting to register

// Lookup: is this NOBD source registered?
// Returns username if registered, nullptr if not
const char* getRegisteredUsername(uint32_t srcIP, uint16_t srcPort);

// Legacy: returns browser ID (same as username now)
const char* getRegisteredBrowserId(uint32_t srcIP, uint16_t srcPort);

// Bind a specific stick source to a username
void registerStick(uint32_t srcIP, uint16_t srcPort, const char* username);

// Unregister a stick by username
void unregisterStick(const char* username);

// Check if a registered stick is online (sent input recently)
bool isStickOnline(const char* username);

// Get stick info for a username
struct StickInfo {
    bool registered;
    bool online;
    char username[16];
    uint32_t srcIP;
    uint16_t srcPort;
    int64_t lastInputUs;
};
StickInfo getStickInfo(const char* username);

// How many registered sticks?
int registeredStickCount();

// Validate username: 4-12 chars, [a-zA-Z0-9_] only
bool isValidUsername(const char* name);

// ==================== Stick persistence ====================
// Bindings live in RAM in this process. Persistence flows out through
// the WebSocket layer to the Rust collector, which mirrors them to
// SurrealDB. On boot we also rehydrate from a local JSON hot-cache so
// flycast survives a restart even if the collector is briefly down.

enum class StickEventKind { Register, Unregister, Online, Offline };
struct StickEvent {
    StickEventKind kind;
    char username[16];
    uint32_t srcIP;       // network byte order
    uint16_t srcPort;     // network byte order
    int64_t  ts;          // unix seconds
};
// Returns and clears the pending event queue. Called by the ws server
// each time it has a place to push events out (immediately on registration
// since rare, or on the periodic status broadcast).
std::vector<StickEvent> drainStickEvents();

// Snapshot all current bindings (for hot-cache writer / debugging).
struct StickSnapshot {
    char username[16];
    uint32_t srcIP;
    uint16_t srcPort;
    int64_t lastInputUs;
};
std::vector<StickSnapshot> snapshotStickBindings();

// Bulk install bindings from local cache or from a `stick_load` push by
// the collector. Existing bindings for the same username are overwritten.
// Does NOT emit StickEvents — these are already persisted upstream.
void installStickBindings(const std::vector<StickSnapshot>& snapshots);

// Local hot-cache I/O. Path: ~/.maplecast/sticks.json. Rewritten on every
// register/unregister so a flycast crash loses at most the in-flight
// transition. loadStickCache() is safe to call before init().
bool loadStickCache();
bool saveStickCache();

}
