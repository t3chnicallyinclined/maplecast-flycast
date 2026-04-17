/*
	MapleCast Audio Client — see header for rationale.

	Implementation notes:

	- The WebSocket framing code (wsHandshake + wsReadFrame) is duplicated
	  from maplecast_mirror.cpp's client block rather than factored into
	  a shared header. Both copies are ~50 lines of RFC 6455 minimalism
	  that neither file has ever needed to evolve. Sharing would force a
	  new util header + a link dependency from here onto mirror.cpp just
	  to save ~30 lines, and I'd rather keep this TU standalone.

	- The receive thread loop is:
	    while (_run) {
	        if (!connect()) { sleep 2s; continue; }
	        if (!handshake()) { close; sleep 2s; continue; }
	        drain loop: read frame → validate → push PCM
	    }
	  Identical shape to the video client loop, and very forgiving of
	  transient server restarts.

	- AudioBackend::push() is called with wait=false. Client build has no
	  SH4 thread running, so nothing else is writing to the backend; we
	  just want to drop on overflow rather than block this thread.

	- All telemetry counters are std::atomic<T>. The overlay reads them
	  without taking any lock.
*/
#include "maplecast_audio_client.h"
#include "audio/audiostream.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#endif

namespace maplecast_audio_client
{

// ---- Configuration ----
static std::string _host;
static int         _port = 7213;

// ---- Thread state ----
static std::thread        _recvThread;
static std::atomic<bool>  _run{false};
static std::atomic<bool>  _enabled{true};
static std::atomic<int>   _sockFd{-1};  // -1 when disconnected
static std::atomic<bool>  _reconnectRequested{false};

// ---- Pre-buffering ----
//
// SDL2's audio backend callback drains 1024 frames at a time (~23.2 ms at
// 44.1 kHz) while we push 512 frames per network packet (~11.6 ms). If
// playback starts with an empty ring, the first SDL callback underruns
// because we don't have 1024 frames queued yet. Even after steady state,
// any network jitter >= 11.6 ms causes the ring to dip below the callback
// threshold → underrun → audible click/chop.
//
// The browser's AudioWorklet solves this with a 5-chunk (~53 ms) pre-buffer
// before enabling playback. We do the same here: accumulate N packets in
// a local queue, then burst-push them once the queue fills. After that,
// steady-state pushes go straight through.
//
// PREBUFFER_PACKETS = 6 × 11.6 ms = ~70 ms of headroom. Slightly more than
// the browser to cover Linux scheduler jitter (render thread can stall the
// audio thread for up to ~18 ms while it runs TA Process).
static constexpr int  PREBUFFER_PACKETS = 6;
static constexpr int  PACKET_FRAMES     = 512;
static std::vector<std::vector<int16_t>> _preBuffer;
static bool _preBufferFilled = false;

// ---- Telemetry (atomic, lock-free reads from the overlay) ----
static std::atomic<bool>     _connected{false};
static std::atomic<uint64_t> _packetsReceived{0};
static std::atomic<uint64_t> _packetsDropped{0};
static std::atomic<uint64_t> _bytesReceived{0};
static std::atomic<uint64_t> _pushFailures{0};
static std::atomic<int64_t>  _lastArrivalUs{0};
static std::atomic<int64_t>  _arrivalIntervalEmaUs{0};
static std::atomic<int64_t>  _arrivalIntervalMaxUs{0};
static std::atomic<uint32_t> _lastSeq{0xFFFFFFFF};  // -1 sentinel: no packets yet

// ---- Helpers ----

static int64_t nowUs()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void closeSocket()
{
	int fd = _sockFd.exchange(-1);
	if (fd >= 0) {
#ifdef _WIN32
		closesocket(fd);
#else
		close(fd);
#endif
	}
	_connected.store(false, std::memory_order_release);
}

// Minimal WebSocket handshake — same implementation as
// maplecast_mirror.cpp's wsHandshake(). See the note at the top of this
// file for why it's duplicated rather than shared.
static bool wsHandshake(int fd, const char* host, int port)
{
	char req[512];
	int len = snprintf(req, sizeof(req),
		"GET / HTTP/1.1\r\n"
		"Host: %s:%d\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: bWFwbGVjYXN0LWF1ZGlvLWM=\r\n"
		"Sec-WebSocket-Version: 13\r\n"
		"\r\n", host, port);
	if (send(fd, req, len, 0) != len) return false;

	char resp[1024];
	int total = 0;
	while (total < (int)sizeof(resp) - 1) {
		int n = recv(fd, resp + total, 1, 0);
		if (n <= 0) return false;
		total += n;
		if (total >= 4 && memcmp(resp + total - 4, "\r\n\r\n", 4) == 0) break;
	}
	resp[total] = 0;
	return strstr(resp, "101") != nullptr;
}

// Minimal WebSocket frame reader — same shape as maplecast_mirror's
// wsReadFrame(). Returns true if a valid binary frame landed in `out`.
// False on close / error. Ping/text frames are treated as "keep reading".
static bool wsReadFrame(int fd, std::vector<uint8_t>& out)
{
	uint8_t hdr[2];
	if (recv(fd, hdr, 2, MSG_WAITALL) != 2) return false;

	int opcode = hdr[0] & 0x0F;
	bool masked = (hdr[1] & 0x80) != 0;
	uint64_t payloadLen = hdr[1] & 0x7F;

	if (payloadLen == 126) {
		uint8_t ext[2];
		if (recv(fd, ext, 2, MSG_WAITALL) != 2) return false;
		payloadLen = (ext[0] << 8) | ext[1];
	} else if (payloadLen == 127) {
		uint8_t ext[8];
		if (recv(fd, ext, 8, MSG_WAITALL) != 8) return false;
		payloadLen = 0;
		for (int i = 0; i < 8; i++) payloadLen = (payloadLen << 8) | ext[i];
	}

	if (masked) {
		uint8_t mask[4];
		if (recv(fd, mask, 4, MSG_WAITALL) != 4) return false;
	}

	out.resize(payloadLen);
	size_t rd = 0;
	while (rd < payloadLen) {
		ssize_t n = recv(fd, out.data() + rd, payloadLen - rd, 0);
		if (n <= 0) return false;
		rd += n;
	}

	if (opcode == 0x8) return false;               // close
	if (opcode == 0x9) { out.clear(); return true; }  // ping
	if (opcode == 0x1) { out.clear(); return true; }  // text
	return opcode == 0x2;                          // binary
}

// ---- Receive thread ----

static void recvLoop()
{
	printf("[audio-client] thread started, target ws://%s:%d/\n", _host.c_str(), _port);

	std::vector<uint8_t> frame;
	frame.reserve(2100);

	while (_run.load(std::memory_order_relaxed))
	{
		if (!_enabled.load(std::memory_order_relaxed)) {
			closeSocket();
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			continue;
		}

		// Resolve hostname (inet_pton only handles IP literals)
		struct addrinfo hints = {}, *res = nullptr;
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		char portBuf[16];
		snprintf(portBuf, sizeof(portBuf), "%d", _port);
		if (getaddrinfo(_host.c_str(), portBuf, &hints, &res) != 0 || !res) {
			printf("[audio-client] getaddrinfo %s:%d failed\n", _host.c_str(), _port);
			std::this_thread::sleep_for(std::chrono::seconds(2));
			continue;
		}

		int fd = (int)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (fd < 0) {
			freeaddrinfo(res);
			std::this_thread::sleep_for(std::chrono::seconds(2));
			continue;
		}

		// Disable Nagle to match the server side — small frequent PCM
		// chunks shouldn't wait for coalescing.
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));

		if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
			printf("[audio-client] connect %s:%d failed: %s\n",
				_host.c_str(), _port, strerror(errno));
			freeaddrinfo(res);
#ifdef _WIN32
			closesocket(fd);
#else
			close(fd);
#endif
			std::this_thread::sleep_for(std::chrono::seconds(2));
			continue;
		}
		freeaddrinfo(res);

		if (!wsHandshake(fd, _host.c_str(), _port)) {
			printf("[audio-client] ws handshake failed\n");
#ifdef _WIN32
			closesocket(fd);
#else
			close(fd);
#endif
			std::this_thread::sleep_for(std::chrono::seconds(2));
			continue;
		}

		_sockFd.store(fd, std::memory_order_release);
		_connected.store(true, std::memory_order_release);
		// Fresh connection → fresh pre-buffer. Drop whatever was queued
		// before a disconnect and start accumulating anew so the very
		// first audio the user hears after reconnect is glitch-free.
		_preBuffer.clear();
		_preBufferFilled = false;
		printf("[audio-client] connected, streaming PCM (pre-buffering %d packets)\n",
			PREBUFFER_PACKETS);

		// Drain loop — runs until the socket breaks or shutdown is requested.
		while (_run.load(std::memory_order_relaxed)
		    && _enabled.load(std::memory_order_relaxed)
		    && !_reconnectRequested.exchange(false))
		{
			if (!wsReadFrame(fd, frame)) break;
			if (frame.empty()) continue;  // ping / text — ignore

			// Audio packet format: [0xAD][0x10][seqHi][seqLo][512 × int16 stereo]
			// = 4 + 2048 = 2052 bytes.
			if (frame.size() != 2052) continue;
			if (frame[0] != 0xAD || frame[1] != 0x10) continue;

			const uint16_t seq = (uint16_t(frame[2]) << 8) | uint16_t(frame[3]);
			const uint32_t prev = _lastSeq.load(std::memory_order_relaxed);
			if (prev != 0xFFFFFFFF) {
				const uint16_t expected = uint16_t(prev + 1);
				if (seq != expected) {
					// seq gap — count the skipped packets as drops
					const uint16_t gap = uint16_t(seq - expected);
					_packetsDropped.fetch_add(gap, std::memory_order_relaxed);
				}
			}
			_lastSeq.store(seq, std::memory_order_relaxed);

			// Arrival-interval telemetry
			const int64_t now = nowUs();
			const int64_t prevArrival = _lastArrivalUs.exchange(now, std::memory_order_relaxed);
			if (prevArrival != 0) {
				const int64_t delta = now - prevArrival;
				// EMA with alpha=1/16 — same shape as the server's frame period EMA
				const int64_t emaPrev = _arrivalIntervalEmaUs.load(std::memory_order_relaxed);
				const int64_t ema = emaPrev + ((delta - emaPrev) >> 4);
				_arrivalIntervalEmaUs.store(ema, std::memory_order_relaxed);
				// Peak watermark
				int64_t mx = _arrivalIntervalMaxUs.load(std::memory_order_relaxed);
				while (delta > mx
				    && !_arrivalIntervalMaxUs.compare_exchange_weak(mx, delta,
				        std::memory_order_relaxed)) {}
			}

			_packetsReceived.fetch_add(1, std::memory_order_relaxed);
			_bytesReceived.fetch_add(frame.size(), std::memory_order_relaxed);

			// Pre-buffer the first PREBUFFER_PACKETS packets before
			// starting playback. See the PREBUFFER_PACKETS comment at
			// the top of this file for the underrun/jitter rationale.
			const int16_t* pcm16 = reinterpret_cast<const int16_t*>(frame.data() + 4);
			if (!_preBufferFilled) {
				_preBuffer.emplace_back(pcm16, pcm16 + PACKET_FRAMES * 2);
				if ((int)_preBuffer.size() >= PREBUFFER_PACKETS) {
					// Burst-drain the pre-buffer into the backend in one shot,
					// then switch to direct-push mode for subsequent packets.
					for (auto& p : _preBuffer) {
						uint32_t pushed = PushExternalAudio(p.data(), PACKET_FRAMES);
						if (pushed == 0)
							_pushFailures.fetch_add(1, std::memory_order_relaxed);
					}
					_preBuffer.clear();
					_preBuffer.shrink_to_fit();
					_preBufferFilled = true;
					printf("[audio-client] pre-buffer filled (%d packets, ~%d ms), playback active\n",
						PREBUFFER_PACKETS, PREBUFFER_PACKETS * 1000 * PACKET_FRAMES / 44100);
				}
			} else {
				// Steady state: push directly to the backend.
				const void* pcm = frame.data() + 4;
				uint32_t pushed = PushExternalAudio(pcm, PACKET_FRAMES);
				if (pushed == 0)
					_pushFailures.fetch_add(1, std::memory_order_relaxed);
			}
		}

		// Fell out of the drain loop — socket busted or told to reconnect.
		closeSocket();
		if (_run.load(std::memory_order_relaxed)) {
			printf("[audio-client] disconnected, retrying in 2s\n");
			std::this_thread::sleep_for(std::chrono::seconds(2));
		}
	}

	closeSocket();
	printf("[audio-client] thread exit\n");
}

// ---- Public API ----

void init(const char* host, int audioPort)
{
	if (_run.exchange(true)) return;  // already running

	_host = host ? host : "127.0.0.1";
	_port = audioPort;
	_enabled.store(true, std::memory_order_release);

	// Detach rather than keep the thread joinable. The recv thread has
	// process-long lifetime — there is no orderly shutdown point where we
	// could join it. Without detach, the file-static std::thread destructor
	// sees joinable()==true at process exit and calls std::terminate(),
	// which is what caused the "terminate called without an active
	// exception" abort on Ctrl+C. Matches the pattern the video WS client
	// uses in maplecast_mirror.cpp (_wsThread.detach()).
	_recvThread = std::thread(recvLoop);
	_recvThread.detach();
}

void shutdown()
{
	// No-op: the recv thread is detached (see init above). We set _run to
	// false so a future init() call can restart it, but the currently-
	// running thread will exit on its own the next time it checks the
	// flag. At process exit nobody calls this anyway.
	_run.store(false, std::memory_order_release);
	closeSocket();
}

void setEnabled(bool enabled)
{
	_enabled.store(enabled, std::memory_order_release);
	if (!enabled) closeSocket();
}

bool isEnabled()
{
	return _enabled.load(std::memory_order_relaxed);
}

void requestReconnect()
{
	_reconnectRequested.store(true, std::memory_order_release);
	closeSocket();
}

Stats getStats()
{
	Stats s;
	s.connected            = _connected.load(std::memory_order_relaxed);
	s.packetsReceived      = _packetsReceived.load(std::memory_order_relaxed);
	s.packetsDropped       = _packetsDropped.load(std::memory_order_relaxed);
	s.bytesReceived        = _bytesReceived.load(std::memory_order_relaxed);
	s.pushFailures         = _pushFailures.load(std::memory_order_relaxed);
	s.lastArrivalUs        = _lastArrivalUs.load(std::memory_order_relaxed);
	s.arrivalIntervalEmaUs = _arrivalIntervalEmaUs.load(std::memory_order_relaxed);
	s.arrivalIntervalMaxUs = _arrivalIntervalMaxUs.load(std::memory_order_relaxed);
	const uint32_t seq     = _lastSeq.load(std::memory_order_relaxed);
	s.lastSeq              = (seq == 0xFFFFFFFF) ? 0 : (uint16_t)seq;
	return s;
}

void resetPeaks()
{
	_arrivalIntervalMaxUs.store(0, std::memory_order_relaxed);
}

} // namespace maplecast_audio_client
