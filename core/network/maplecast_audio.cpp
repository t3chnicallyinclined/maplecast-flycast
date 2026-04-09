/*
	MapleCast Audio Stream — raw PCM broadcast, SPSC handoff.

	ZERO encode latency. Raw 16-bit stereo PCM at the host AICA output rate
	(44.1 kHz on DC, same on Naomi). 512 samples per chunk = 2048 bytes
	= ~11.6 ms of audio at 44.1 kHz.

	THREADING:
	- pushSample() runs on the SH4/Flycast-emu thread, called from
	  WriteSample() → AICA_Sample(). It is on a hot, latency-sensitive
	  path: it MUST NOT block. It writes to a lock-free SPSC ring buffer
	  and returns. Never touches a mutex, never calls into the WebSocket
	  stack.
	- A dedicated sender thread (`audio_sender`) drains the ring and calls
	  maplecast_ws::broadcastBinary / maplecast_webrtc::broadcastAudio on
	  its own time. This thread can block on the connection mutex without
	  impacting the emu thread at all.

	This decoupling is why audio and video no longer fight for the
	broadcastBinary mutex from the SH4 thread's perspective. The mutex
	contention still exists between the sender thread and the mirror
	publish path, but the SH4/emu thread is never one of the two
	contenders, so its frame pacing is not affected.

	Packet wire format (unchanged):
	  [0xAD][0x10][seqHi][seqLo][512 × int16 stereo PCM] = 2052 bytes
*/
#include "types.h"
#include "maplecast_audio.h"
#include "maplecast_audio_ws.h"
#include "maplecast_webrtc.h"
#include "maplecast_ws_server.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>

namespace maplecast_audio
{

// Audio chunk constants
static constexpr int CHUNK_SAMPLES = 512;
static constexpr int CHUNK_BYTES = CHUNK_SAMPLES * 4;  // 16-bit stereo = 4 bytes/sample
static constexpr int PACKET_SIZE = 4 + CHUNK_BYTES;     // [marker(2)][seqNum(2)][pcm_data]

// SPSC ring buffer — power of 2 slot count for wrap via mask.
// 16 slots * 11.6 ms = ~186 ms of audio buffered if the sender thread
// ever falls that far behind. Ample headroom; in practice the sender
// drains within microseconds of pushSample writing a slot.
static constexpr int RING_SLOTS = 16;
static constexpr int RING_MASK  = RING_SLOTS - 1;

struct AudioChunk {
	uint8_t packet[PACKET_SIZE];
};

static AudioChunk _ring[RING_SLOTS];
static std::atomic<uint64_t> _writeIdx{0};  // producer (pushSample)
static std::atomic<uint64_t> _readIdx{0};   // consumer (sender thread)

// Accumulator used by pushSample while filling the next chunk. Lives in
// producer-thread-only state so no synchronization is needed.
static int16_t _accBuffer[CHUNK_SAMPLES * 2];
static int _accWritePos = 0;
static uint16_t _seqNum = 0;

static std::atomic<bool> _active{false};
static std::atomic<bool> _senderRunning{false};
static std::thread _senderThread;

// Telemetry
static std::atomic<uint64_t> _droppedChunks{0};  // ring was full, producer overwrote oldest

void pushSample(int16_t left, int16_t right)
{
	if (!_active.load(std::memory_order_relaxed)) return;

	_accBuffer[_accWritePos * 2]     = left;
	_accBuffer[_accWritePos * 2 + 1] = right;
	_accWritePos++;

	if (_accWritePos < CHUNK_SAMPLES) return;

	// Full chunk — commit to the ring.
	const uint64_t wIdx = _writeIdx.load(std::memory_order_relaxed);
	const uint64_t rIdx = _readIdx.load(std::memory_order_acquire);

	// Check for overflow: if the ring is full (16 slots ahead), we drop
	// the OLDEST slot by advancing the read index. This is fire-and-forget
	// audio — a dropped chunk is better than a stalled emu thread.
	if (wIdx - rIdx >= RING_SLOTS) {
		_readIdx.store(rIdx + 1, std::memory_order_release);
		_droppedChunks.fetch_add(1, std::memory_order_relaxed);
	}

	// Ring write re-enabled for test B: sender thread drains normally
	// but the broadcast call itself is bypassed. Isolates the _ws.send
	// cost from the ring/thread overhead.
	AudioChunk& slot = _ring[wIdx & RING_MASK];
	slot.packet[0] = 0xAD;
	slot.packet[1] = 0x10;
	slot.packet[2] = (_seqNum >> 8) & 0xFF;
	slot.packet[3] = _seqNum & 0xFF;
	memcpy(slot.packet + 4, _accBuffer, CHUNK_BYTES);
	_writeIdx.store(wIdx + 1, std::memory_order_release);

	_accWritePos = 0;
	_seqNum++;
}

static void senderThreadMain()
{
	// Pin the sender thread to a name for profiling.
	while (_senderRunning.load(std::memory_order_relaxed))
	{
		const uint64_t rIdx = _readIdx.load(std::memory_order_relaxed);
		const uint64_t wIdx = _writeIdx.load(std::memory_order_acquire);

		if (rIdx == wIdx) {
			// Ring empty — sleep briefly. 2 ms is much less than 11.6 ms
			// (our packet cadence) so we never fall behind.
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			continue;
		}

		// Drain all pending chunks. Audio now rides its OWN WebSocket
		// server (maplecast_audio_ws) on a dedicated port with its own
		// io_service thread and its own TCP sockets per client. It
		// cannot head-of-line-block the TA mirror socket, and its send
		// work cannot steal event-loop time from the video path.
		//
		// broadcastBinary() is non-blocking: it posts onto the audio
		// server's private io_service thread and returns immediately.
		while (_readIdx.load(std::memory_order_relaxed) != _writeIdx.load(std::memory_order_acquire))
		{
			const uint64_t idx = _readIdx.load(std::memory_order_relaxed);
			AudioChunk& slot = _ring[idx & RING_MASK];

			if (maplecast_audio_ws::active())
				maplecast_audio_ws::broadcastBinary(slot.packet, PACKET_SIZE);
#ifdef MAPLECAST_WEBRTC
			maplecast_webrtc::broadcastAudio(slot.packet, PACKET_SIZE);
#endif

			_readIdx.store(idx + 1, std::memory_order_release);
		}
	}
}

void init()
{
	_accWritePos = 0;
	_seqNum = 0;
	_writeIdx.store(0);
	_readIdx.store(0);
	_droppedChunks.store(0);
	_active = true;

	if (!_senderRunning.exchange(true)) {
		_senderThread = std::thread(senderThreadMain);
	}

	printf("[maplecast-audio] SPSC ring: %d slots × %d bytes, sender thread running — "
		"emu thread never blocks on network (~%.1f kB/s audio stream)\n",
		RING_SLOTS, PACKET_SIZE,
		(PACKET_SIZE * 44100.0 / CHUNK_SAMPLES) / 1024.0);
}

void shutdown()
{
	_active = false;
	if (_senderRunning.exchange(false)) {
		if (_senderThread.joinable())
			_senderThread.join();
	}
}

bool active()
{
	return _active.load(std::memory_order_relaxed);
}

} // namespace maplecast_audio
