/*
	MapleCast — Flycast IS the server.

	One UDP port. Players auto-assigned P1/P2 by connection order.
	First stick to send a packet = P1. Second = P2.

	Input: 4-byte W3 packets (LT, RT, buttons_hi, buttons_lo)
	From: GP2040-CE W6100, pc_gamepad_sender.py, or browser Gamepad API
*/
#pragma once
#include <cstdint>

struct MapleInputState;

namespace maplecast
{

// Start listening for gamepad input on a single UDP port
bool init(int port = 7100);
void shutdown();

// Called from maple_DoDma() — writes latest received inputs into mapleInputState[]
void getInput(MapleInputState inputState[4]);

// Is MapleCast active?
bool active();

// Timestamp (us) of last getInput() call — for latency telemetry
int64_t lastInputTimeUs();

// Player stats for diagnostics
struct PlayerStats {
	uint32_t packetsPerSec;   // input packets received per second
	uint32_t changesPerSec;   // button state changes per second
	uint16_t buttons;         // current button state
	uint8_t  lt, rt;          // current triggers
	bool     connected;       // player assigned?
};
void getPlayerStats(PlayerStats& p1, PlayerStats& p2);

}
