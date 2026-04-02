/*
	MapleCast — Flycast IS the server.

	One UDP port. Players auto-assigned P1/P2 by connection order.
	First stick to send a packet = P1. Second = P2.

	Input: 4-byte W3 packets (LT, RT, buttons_hi, buttons_lo)
	From: GP2040-CE W6100, pc_gamepad_sender.py, or browser Gamepad API
*/
#pragma once

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

}
