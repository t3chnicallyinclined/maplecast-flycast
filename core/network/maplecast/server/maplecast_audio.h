/*
	MapleCast Audio Stream — raw PCM over DataChannel.

	Taps flycast's audio buffer (WriteSample), accumulates frames,
	sends raw 16-bit stereo PCM chunks to all connected peers.

	Zero encode latency. Browser decodes with AudioWorklet.
	2048 bytes per chunk (512 samples × 4 bytes) at ~48KHz.
*/
#pragma once
#include <cstdint>

namespace maplecast_audio
{

// Called from WriteSample() — accumulates audio samples
void pushSample(int16_t left, int16_t right);

// Initialize audio streaming
void init();

// Shutdown
void shutdown();

// Is audio streaming active?
bool active();

}
