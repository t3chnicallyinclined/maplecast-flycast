/*
	MapleCast Input Server — single source of truth for all player input.

	Every input path (NOBD UDP, browser WebSocket, XDP, SDL) feeds through
	this server. One registry, one set of kcode[] writes, one place to
	track latency, jitter, and buffer depth per player.

	The game reads kcode[]/lt[]/rt[] at CMD9 time — always fresh.
*/
#pragma once
#include <cstdint>

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

// Called by WebSocket message handler to inject browser gamepad input
void injectInput(int slot, uint8_t lt, uint8_t rt, uint16_t buttons);

// Is the input server running?
bool active();

// Stick registration — bind a NOBD stick to a browser user
// Returns true when combo detected and registration complete
void startStickRegistration(const char* browserId);
void cancelStickRegistration();
bool isRegistering();

// Lookup: is this NOBD source registered to a browser user?
// Returns browser ID if registered, nullptr if not
const char* getRegisteredBrowserId(uint32_t srcIP, uint16_t srcPort);

// Bind a specific stick source to a browser user (called after combo detected)
void registerStick(uint32_t srcIP, uint16_t srcPort, const char* browserId);

// Unregister a stick by browser ID
void unregisterStick(const char* browserId);

// How many registered sticks?
int registeredStickCount();

}
