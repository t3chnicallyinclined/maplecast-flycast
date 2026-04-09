/*
	MapleCast Audio Client — native mirror client side of the audio WS.

	The native flycast mirror client (build variant: not headless, not the
	server) connects to the server's video WS on port 7200/7210 and renders
	TA frames. Until this TU existed, that same client dropped every audio
	packet on the floor because:
	  - Originally audio rode the same WS as video; client just continue'd
	    past 0xAD 0x10 packets to avoid mis-parsing them as frames
	  - After the audio/video split, audio lives on its OWN WS on port
	    7213 and the client never connected to it at all

	This TU spawns a dedicated audio receive thread that:
	  1. Opens a raw TCP socket to server:7213
	  2. Speaks the minimal WebSocket handshake + framing (duplicated from
	     maplecast_mirror.cpp's client code — small enough to not warrant
	     factoring)
	  3. Reads 2052-byte binary frames, verifies the [0xAD][0x10] magic,
	     tracks seq-number drops
	  4. Pushes 512 int16 stereo sample chunks straight into the local
	     AudioBackend via currentBackend->push()

	On the client build the SH4 emu thread is dormant — WriteSample() never
	fires — so nothing contends with us for the backend's sample buffer.
	All we do is feed the same backend with remote PCM the user can hear.

	Exposed telemetry (atomic reads, safe from any thread) is consumed by
	the ImGui debug overlay so the user can see live packet/byte/drop
	counters without grepping logs.
*/
#pragma once
#include <cstdint>

namespace maplecast_audio_client
{

// Connect to host:port and start the receive thread. host is the same as
// the video WS host (MAPLECAST_SERVER_HOST). port is typically the video
// port + 3, matching the server-side default (maplecast_mirror.cpp hooks
// audioWsPort = wsPort + 3). Safe to call once during client init.
void init(const char* host, int audioPort);

// Graceful shutdown: flip the run flag, close the socket, join the thread.
void shutdown();

// ---- Runtime control surface used by the ImGui debug overlay ----

// Toggle audio receive. When disabled the socket is closed and no more
// PCM is pushed to the backend. Re-enabling triggers a reconnect on the
// next receive-loop iteration.
void setEnabled(bool enabled);
bool isEnabled();

// Force the current connection closed. The receiver thread sees the
// EOF and loops around to re-handshake. Used by the overlay's
// "Reconnect Audio" button.
void requestReconnect();

// ---- Telemetry snapshot ----

struct Stats {
	bool     connected;          // TCP + WS handshake succeeded and no read error
	uint64_t packetsReceived;    // total audio packets decoded + pushed
	uint64_t packetsDropped;     // seq-number gaps observed
	uint64_t bytesReceived;      // total bytes over the audio WS (payloads only)
	uint64_t pushFailures;       // AudioBackend->push() returned 0 (buffer full)
	int64_t  lastArrivalUs;      // wall-clock timestamp of the last packet (µs since epoch)
	int64_t  arrivalIntervalEmaUs;  // EMA of packet-to-packet arrival interval
	int64_t  arrivalIntervalMaxUs;  // peak arrival interval over the current window
	uint16_t lastSeq;            // most recent seq number (for quick "is stream alive" check)
};

// Returns a consistent-enough snapshot of all the atomics above. No locks.
// Values may skew by a packet or two in a tight race with the recv thread,
// which is perfectly fine for a debug overlay.
Stats getStats();

// Reset the arrivalIntervalMaxUs watermark — the overlay calls this when
// it wants to start a new measurement window. Other counters keep running.
void resetPeaks();

} // namespace maplecast_audio_client
