/*
	MapleCast Stream — NVENC H.264 encode + WebSocket transport.

	Captures each frame via GetLastFrame(), NVENC encodes to H.264,
	sends NAL units over WebSocket to browser clients.
	Browser decodes with WebCodecs VideoDecoder.

	ws://host:7200 — binary WebSocket
	Server sends: H.264 NAL units (Annex B format)
	Client sends: 5 bytes {player_id, LT, RT, buttons_hi, buttons_lo}
*/
#pragma once

namespace maplecast_stream
{

bool init(int wsPort = 7200);
void shutdown();

// Called after each frame is rendered — captures, encodes, sends video + game state
void onFrameRendered();

// Called every frame even in headless/norend mode — sends game state only (240 bytes)
void onFrameAdvanced();

// Is streaming active?
bool active();

// Is headless mode (game state only, no video)?
bool headless();

}
