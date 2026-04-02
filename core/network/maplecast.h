/*
	MapleCast — server-clocked lockstep netplay for Dreamcast.

	Flycast doesn't know it's online. This module:
	1. Connects to a MapleCast relay server
	2. Sends local input as CMD9 to the server
	3. Blocks until the server tick arrives (BOTH players' inputs)
	4. Writes both inputs into mapleInputState[]
	5. Flycast advances one frame on that data

	No rollback. No speculation. No desync. The server is the clock.
*/
#pragma once
#include <string>

struct MapleInputState;

namespace maplecast
{

struct Config
{
	std::string serverAddr = "127.0.0.1";
	int serverPort = 7100;
	int matchId = 0;
	int localPlayer = 0;     // 0 = P1, 1 = P2
	bool tournament = false;
};

// Lifecycle
bool init(const Config& cfg);
void shutdown();

// Called BEFORE getSh4Executor()->Run() every frame.
// Sends local input to server, blocks until server tick arrives.
// Writes synced inputs into mapleInputState[].
// Returns false if connection lost.
bool waitForTick(MapleInputState inputState[4]);

// Is MapleCast active?
bool active();

// Stats for HUD overlay
float currentFps();
float rttMs();
int currentFrame();

}
