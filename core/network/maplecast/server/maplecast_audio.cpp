/*
	MapleCast Audio Stream — raw PCM over DataChannel.

	ZERO encode latency. Raw 16-bit stereo PCM at 48KHz.
	512 samples per chunk = 2048 bytes = 10.67ms of audio.

	Sent on DataChannel "audio" {ordered: true, maxRetransmits: 0}.
	Ordered because audio must play in sequence, but unreliable
	because a dropped chunk is better than a delayed one.

	Browser: AudioWorklet receives PCM, feeds to AudioContext.
*/
#include "types.h"
#include "maplecast_audio.h"
#include "maplecast_webrtc.h"

#include <cstdio>
#include <cstring>
#include <atomic>

namespace maplecast_audio
{

// Audio chunk: 512 stereo samples = 2048 bytes
// Prefixed with 2-byte marker [0xAU][0xD1] so browser can identify audio vs video
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

		// Send via WebRTC DataChannel "audio"
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
	printf("[maplecast-audio] streaming raw PCM: 48KHz 16-bit stereo, %d samples/chunk (%d bytes)\n",
		CHUNK_SAMPLES, PACKET_SIZE);
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
