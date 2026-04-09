/*
	MapleCast Audio WebSocket Server — dedicated transport for PCM audio.

	Why a separate server from maplecast_ws:
	  The TA mirror WebSocket (maplecast_ws, default port 7200/7210) carries
	  ~4 Mbps of zstd-compressed video frames. Audio at 176 kB/s is a rounding
	  error on the bandwidth but was causing video lag because:

	    1. Both streams shared the SAME TCP socket per client. TCP enforces
	       strict byte ordering, so if a video frame was mid-send when an
	       audio packet arrived, the audio packet waited behind it — and a
	       video frame that arrived while audio was mid-send waited behind
	       THAT. Classic head-of-line blocking.
	    2. Both streams shared the SAME asio io_service thread. 146 sends/sec
	       (60 video + 86 audio) serialized through one event loop meant audio
	       sends were stealing event-loop time from the video path.

	The fix: put audio on its own WebSocket endpoint with its own io_service
	thread on its own port. No TCP ordering across streams. No event-loop
	contention. Video and audio only converge in human perception.

	This server is brutally minimal:
	  - accept connections
	  - broadcastBinary() posts the packet onto its own io_service thread
	  - no control messages, no JSON, no status
	  - clients get audio as soon as they connect; no handshake

	Wire format (unchanged from the old shared-WS path):
	  [0xAD][0x10][seqHi][seqLo][512 × int16 stereo PCM] = 2052 bytes
*/
#pragma once
#include <cstddef>
#include <cstdint>

namespace maplecast_audio_ws
{
bool init(int port = 7213);
void shutdown();
bool active();

// Fire-and-forget broadcast. Safe to call from any thread (the audio sender
// thread in particular). Internally posts to this server's own io_service
// so the caller never blocks on socket I/O.
void broadcastBinary(const void* data, size_t size);
}
