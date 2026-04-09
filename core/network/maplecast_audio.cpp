/*
	MapleCast Audio Stream — raw PCM broadcast.

	ZERO encode latency. Raw 16-bit stereo PCM at the host AICA output rate
	(44.1 kHz on DC, same on Naomi). 512 samples per chunk = 2048 bytes
	= ~11.6 ms of audio at 44.1 kHz.

	Two transports (both piggyback the same packet format so the client
	doesn't care how the bytes arrived):

	1. WebSocket broadcast via maplecast_ws::broadcastBinary. Rides the
	   same pipe as the TA mirror video frames, which means it automatically
	   inherits the P2P spectator relay fan-out tree — the flycast server
	   only emits ONCE, the relay + browser P2P tree distribute it.

	2. WebRTC DataChannel "audio" {ordered: true, maxRetransmits: 0} —
	   legacy direct-to-peer path used by the old web/index.html client.
	   Ordered because audio must play in sequence, but unreliable because
	   a dropped chunk is better than a delayed one. Only active if a peer
	   has negotiated an audio DC.

	Browser: detect [0xAD][0x10] magic, feed PCM to an AudioWorklet which
	runs on a dedicated audio thread and converts int16 → float32 for the
	AudioContext output.
*/
#include "types.h"
#include "maplecast_audio.h"
#include "maplecast_webrtc.h"
#include "maplecast_ws_server.h"

#include <cstdio>
#include <cstring>
#include <atomic>

namespace maplecast_audio
{

// Audio chunk: 512 stereo samples = 2048 bytes
// Prefixed with 4-byte header [0xAD][0x10][seqHi][seqLo] so browser can
// identify audio vs video on the shared binary WebSocket.
static constexpr int CHUNK_SAMPLES = 512;
static constexpr int CHUNK_BYTES = CHUNK_SAMPLES * 4;  // 16-bit stereo = 4 bytes/sample
static constexpr int PACKET_SIZE = 4 + CHUNK_BYTES;     // [marker(2)][seqNum(2)][pcm_data]

static int16_t _buffer[CHUNK_SAMPLES * 2];  // interleaved L,R,L,R...
static int _writePos = 0;
static uint16_t _seqNum = 0;
static uint8_t _packet[PACKET_SIZE];
static std::atomic<bool> _active{false};

void pushSample(int16_t left, int16_t right)
{
	if (!_active.load(std::memory_order_relaxed)) return;

	_buffer[_writePos * 2]     = left;
	_buffer[_writePos * 2 + 1] = right;
	_writePos++;

	if (_writePos >= CHUNK_SAMPLES)
	{
		// Build packet: [0xAD][0x10][seqNum_hi][seqNum_lo][pcm_data...]
		_packet[0] = 0xAD;
		_packet[1] = 0x10;
		_packet[2] = (_seqNum >> 8) & 0xFF;
		_packet[3] = _seqNum & 0xFF;
		memcpy(_packet + 4, _buffer, CHUNK_BYTES);

		// Primary: WebSocket broadcast — rides the TA mirror pipe, so the
		// existing relay + P2P fan-out tree automatically distributes audio
		// to all connected spectators. Flycast server only emits once.
		if (maplecast_ws::active())
			maplecast_ws::broadcastBinary(_packet, PACKET_SIZE);

		// Legacy: WebRTC DataChannel — direct-to-peer for the old client
		// (web/index.html). Only fires if a peer has negotiated an audio DC.
#ifdef MAPLECAST_WEBRTC
		maplecast_webrtc::broadcastAudio(_packet, PACKET_SIZE);
#endif

		_writePos = 0;
		_seqNum++;
	}
}

void init()
{
	_writePos = 0;
	_seqNum = 0;
	_active = true;
	printf("[maplecast-audio] streaming raw PCM via WS + WebRTC: %d samples/chunk (%d bytes/packet, ~%.1f kB/s)\n",
		CHUNK_SAMPLES, PACKET_SIZE, (PACKET_SIZE * 44100.0 / CHUNK_SAMPLES) / 1024.0);
}

void shutdown()
{
	_active = false;
}

bool active()
{
	return _active.load(std::memory_order_relaxed);
}

} // namespace maplecast_audio
