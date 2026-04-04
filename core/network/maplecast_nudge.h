/*
	MapleCast Nudge — minimum viable state correction.

	Server streams 58 bytes/frame: positions + health + frame counter.
	Client runs its own flycast with same ROM + same save state.
	Client writes server positions to RAM every frame.
	Characters stay in the right place. Game figures out the rest.

	MAPLECAST_NUDGE_SERVER=1 on server
	MAPLECAST_NUDGE_CLIENT=1 on client
*/
#pragma once

namespace maplecast_nudge
{
void initServer();
void initClient();
bool isServer();
bool isClient();

// Server: called every frame to publish positions
void serverTick();

// Client: called every frame to read and apply positions
void clientTick();
}
