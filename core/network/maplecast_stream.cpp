/*
	MapleCast Stream — NVENC H.264 + raw TCP/WebSocket.

	Pipeline: GetLastFrame() → RGB→NV12 → NVENC H.264 → NAL units → TCP send

	V1: Raw TCP with simple framing. Browser connects via WebSocket proxy
	    or we add proper WebSocket handshake.
	V2: Direct WebSocket with websocketpp (already in Flycast deps).

	At 640x480, NVENC preset P1 ultra-low-latency:
	  Encode: ~0.5ms
	  Bitrate: ~3-5 Mbps
	  Every frame is an IDR (keyframe) — no decode dependencies
*/
#include "maplecast_stream.h"
#include "maplecast.h"
#include "hw/pvr/Renderer_if.h"

// Access to the global renderer for GetLastFrame()
extern Renderer* renderer;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swscale.lib")
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#define closesocket close
typedef int SOCKET;
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#endif

#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

namespace maplecast_stream
{

static std::atomic<bool> _active{false};
static std::thread _acceptThread;

// NVENC encoder state
static const AVCodec* _codec = nullptr;
static AVCodecContext* _codecCtx = nullptr;
static AVFrame* _frame = nullptr;
static AVPacket* _pkt = nullptr;
static SwsContext* _swsCtx = nullptr;
static int _width = 640;
static int _height = 480;

// Network
static SOCKET _listenSock = INVALID_SOCKET;
static constexpr int MAX_CLIENTS = 4;
static SOCKET _clients[MAX_CLIENTS] = {INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET};
static std::mutex _clientMutex;

static bool initEncoder()
{
	// Find NVENC H.264 encoder
	_codec = avcodec_find_encoder_by_name("h264_nvenc");
	if (!_codec)
	{
		printf("[maplecast-stream] NVENC not available, falling back to libx264\n");
		_codec = avcodec_find_encoder_by_name("libx264");
	}
	if (!_codec)
	{
		printf("[maplecast-stream] no H.264 encoder found!\n");
		return false;
	}
	printf("[maplecast-stream] using encoder: %s\n", _codec->name);

	_codecCtx = avcodec_alloc_context3(_codec);
	if (!_codecCtx) return false;

	// Resolution
	_codecCtx->width = _width;
	_codecCtx->height = _height;

	// Timebase = 60fps
	_codecCtx->time_base = {1, 60};
	_codecCtx->framerate = {60, 1};

	// Pixel format — NVENC wants NV12
	_codecCtx->pix_fmt = AV_PIX_FMT_NV12;

	// Bitrate — 5 Mbps is plenty for 480p
	_codecCtx->bit_rate = 5000000;

	// ULTRA LOW LATENCY settings
	_codecCtx->max_b_frames = 0;        // No B-frames — zero reordering delay
	_codecCtx->gop_size = 1;            // Every frame is IDR — independently decodable
	_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;

	// NVENC-specific options
	if (strcmp(_codec->name, "h264_nvenc") == 0)
	{
		av_opt_set(_codecCtx->priv_data, "preset", "p1", 0);         // Fastest preset
		av_opt_set(_codecCtx->priv_data, "tune", "ull", 0);          // Ultra low latency
		av_opt_set(_codecCtx->priv_data, "zerolatency", "1", 0);     // No pipeline delay
		av_opt_set(_codecCtx->priv_data, "rc", "cbr", 0);            // Constant bitrate
		av_opt_set(_codecCtx->priv_data, "delay", "0", 0);           // Zero frame delay
		av_opt_set(_codecCtx->priv_data, "forced-idr", "1", 0);      // Force all IDR
	}
	else if (strcmp(_codec->name, "libx264") == 0)
	{
		av_opt_set(_codecCtx->priv_data, "preset", "ultrafast", 0);
		av_opt_set(_codecCtx->priv_data, "tune", "zerolatency", 0);
	}

	// Constrained Baseline profile — hardware decoded everywhere
	_codecCtx->profile = 66; // AV_PROFILE_H264_BASELINE = 66

	if (avcodec_open2(_codecCtx, _codec, nullptr) < 0)
	{
		printf("[maplecast-stream] failed to open encoder\n");
		avcodec_free_context(&_codecCtx);
		return false;
	}

	// Allocate frame (NV12)
	_frame = av_frame_alloc();
	_frame->format = AV_PIX_FMT_NV12;
	_frame->width = _width;
	_frame->height = _height;
	av_frame_get_buffer(_frame, 0);

	// Allocate packet
	_pkt = av_packet_alloc();

	// RGB24 → NV12 converter
	_swsCtx = sws_getContext(
		_width, _height, AV_PIX_FMT_RGB24,
		_width, _height, AV_PIX_FMT_NV12,
		SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

	printf("[maplecast-stream] NVENC encoder ready: %dx%d @ %lld bps, GOP=1 (all IDR)\n",
		_width, _height, (long long)_codecCtx->bit_rate);
	return true;
}

static void cleanupEncoder()
{
	if (_swsCtx) { sws_freeContext(_swsCtx); _swsCtx = nullptr; }
	if (_pkt) { av_packet_free(&_pkt); }
	if (_frame) { av_frame_free(&_frame); }
	if (_codecCtx) { avcodec_free_context(&_codecCtx); }
}

// Send encoded packet to all connected clients
// Frame format: [4-byte LE length][H.264 NAL data]
// Non-blocking: if send would block, drop the frame for that client.
// NEVER stall the render thread.
static void broadcastPacket(const uint8_t* data, int size)
{
	// Build complete message: [length][data] so we send atomically
	std::vector<u8> msg(4 + size);
	uint32_t len = (uint32_t)size;
	memcpy(msg.data(), &len, 4);
	memcpy(msg.data() + 4, data, size);

	std::lock_guard<std::mutex> lock(_clientMutex);
	for (int i = 0; i < MAX_CLIENTS; i++)
	{
		if (_clients[i] == INVALID_SOCKET) continue;

		// Non-blocking send — drop frame if it would block
		int sent = send(_clients[i], (const char*)msg.data(), (int)msg.size(), 0);
		if (sent <= 0)
		{
#ifdef _WIN32
			int err = WSAGetLastError();
			if (err == WSAEWOULDBLOCK)
				continue; // Drop this frame, try next one
#else
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
#endif
			printf("[maplecast-stream] client %d disconnected\n", i);
			closesocket(_clients[i]);
			_clients[i] = INVALID_SOCKET;
		}
	}
}

// Accept loop
static void acceptLoop(int port)
{
	_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (_listenSock == INVALID_SOCKET) return;

	int opt = 1;
	setsockopt(_listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons((u16)port);

	if (bind(_listenSock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
	{
		printf("[maplecast-stream] failed to bind port %d\n", port);
		closesocket(_listenSock);
		_listenSock = INVALID_SOCKET;
		return;
	}

	listen(_listenSock, 4);
	printf("[maplecast-stream] listening on port %d (raw H.264 framed TCP)\n", port);

	// Set non-blocking
#ifdef _WIN32
	u_long mode = 1;
	ioctlsocket(_listenSock, FIONBIO, &mode);
#else
	fcntl(_listenSock, F_SETFL, fcntl(_listenSock, F_GETFL, 0) | O_NONBLOCK);
#endif

	while (_active)
	{
		struct sockaddr_in clientAddr;
		socklen_t clientLen = sizeof(clientAddr);
		SOCKET client = accept(_listenSock, (struct sockaddr*)&clientAddr, &clientLen);

		if (client != INVALID_SOCKET)
		{
			// Disable Nagle — send frames immediately
			int nodelay = 1;
			setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

			// Large send buffer — queue up to ~10 frames without blocking
			int sndbuf = 512 * 1024;
			setsockopt(client, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));

			// Non-blocking for client too
#ifdef _WIN32
			u_long cmode = 1;
			ioctlsocket(client, FIONBIO, &cmode);
#else
			fcntl(client, F_SETFL, fcntl(client, F_GETFL, 0) | O_NONBLOCK);
#endif

			std::lock_guard<std::mutex> lock(_clientMutex);
			for (int i = 0; i < MAX_CLIENTS; i++)
			{
				if (_clients[i] == INVALID_SOCKET)
				{
					_clients[i] = client;
					printf("[maplecast-stream] client %d connected\n", i);
					break;
				}
			}
		}
		else
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}

	closesocket(_listenSock);
	_listenSock = INVALID_SOCKET;
}

static int64_t _frameCount = 0;

bool init(int wsPort)
{
#ifdef _WIN32
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

	if (!initEncoder())
		return false;

	_active = true;
	_frameCount = 0;
	_acceptThread = std::thread(acceptLoop, wsPort);

	printf("[maplecast-stream] initialized\n");
	return true;
}

void shutdown()
{
	_active = false;
	if (_acceptThread.joinable())
		_acceptThread.join();

	{
		std::lock_guard<std::mutex> lock(_clientMutex);
		for (int i = 0; i < MAX_CLIENTS; i++)
		{
			if (_clients[i] != INVALID_SOCKET)
			{
				closesocket(_clients[i]);
				_clients[i] = INVALID_SOCKET;
			}
		}
	}

	cleanupEncoder();
	printf("[maplecast-stream] shutdown\n");
}

void onFrameRendered()
{
	if (!_active || !_codecCtx) return;

	// Check if anyone is connected
	{
		std::lock_guard<std::mutex> lock(_clientMutex);
		bool hasClients = false;
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (_clients[i] != INVALID_SOCKET) { hasClients = true; break; }
		if (!hasClients) return;
	}

	// Capture frame from Flycast renderer
	std::vector<u8> rgbData;
	int w = 0, h = 0;  // 0,0 = use native framebuffer resolution
	try {
		if (!renderer || !renderer->GetLastFrame(rgbData, w, h))
			return;
	} catch (...) {
		// GPU readback can fail during swapchain transitions
		return;
	}

	if (rgbData.empty()) return;

	// Handle resolution change — rebuild sws scaler if source size changed
	if (w != _width || h != _height)
	{
		printf("[maplecast-stream] resolution changed: %dx%d → %dx%d\n", _width, _height, w, h);
		_width = w;
		_height = h;

		// Rebuild sws context for new source resolution
		if (_swsCtx) sws_freeContext(_swsCtx);
		_swsCtx = sws_getContext(
			_width, _height, AV_PIX_FMT_RGB24,
			_codecCtx->width, _codecCtx->height, AV_PIX_FMT_NV12,
			SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
		if (!_swsCtx) return;
	}

	// Convert RGB24 → NV12 for NVENC
	const uint8_t* srcSlice[1] = { rgbData.data() };
	int srcStride[1] = { w * 3 };
	sws_scale(_swsCtx, srcSlice, srcStride, 0, h,
		_frame->data, _frame->linesize);

	_frame->pts = _frameCount++;

	// Encode
	int ret = avcodec_send_frame(_codecCtx, _frame);
	if (ret < 0) return;

	while (ret >= 0)
	{
		ret = avcodec_receive_packet(_codecCtx, _pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0) break;

		// Broadcast H.264 NAL units to all connected clients
		broadcastPacket(_pkt->data, _pkt->size);
		av_packet_unref(_pkt);
	}
}

bool active()
{
	return _active;
}

}  // namespace maplecast_stream
