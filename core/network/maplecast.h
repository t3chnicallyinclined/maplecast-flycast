/*
	MapleCast — Flycast IS the server.

	Two gamepad senders connect over UDP (one per player).
	Flycast receives button state, runs the game, renders.
	One state. One screen. Zero desync.

	Input: pc_gamepad_sender.py sends 4-byte W3 packets (LT, RT, buttons_hi, buttons_lo)
	P1 → UDP port 7101
	P2 → UDP port 7102
*/
#pragma once

struct MapleInputState;

namespace maplecast
{

// Start listening for gamepad input on UDP ports
bool init(int p1Port = 7101, int p2Port = 7102);
void shutdown();

// Called from maple_DoDma() — writes latest received inputs into mapleInputState[]
void getInput(MapleInputState inputState[4]);

// Is MapleCast active?
bool active();

}
