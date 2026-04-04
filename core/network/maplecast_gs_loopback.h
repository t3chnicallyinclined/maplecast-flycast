/*
	Game State Loopback Test — read → serialize → deserialize → write back.
	If the game keeps running identically, the pipeline is proven.
	MAPLECAST_GS_LOOPBACK=1 to enable.
*/
#pragma once

namespace maplecast_gs_loopback
{
void init();
void tick();  // called every frame
bool active();
}
