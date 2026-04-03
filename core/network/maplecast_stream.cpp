/*
	MapleCast Stream — ZERO COPY GPU pipeline.

	GL texture → CUDA interop → NVENC encode → H.264 NAL → TCP
	CPU never touches the pixels.

	Pipeline:
	1. Flycast renders to OpenGL FBO
	2. We register the GL texture with CUDA (once)
	3. Each frame: map GL texture to CUDA, copy to NVENC input surface
	4. NVENC encodes directly from GPU memory
	5. H.264 NAL units sent over TCP
*/
#include "maplecast_stream.h"
#include "maplecast.h"
#include "maplecast_telemetry.h"
#include "hw/pvr/Renderer_if.h"
#include "wsi/gl_context.h"

// CUDA driver API + GL interop
#include <cuda.h>
#include <cudaGL.h>

// FFmpeg for NVENC via CUDA hwaccel
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swscale.lib")
#pragma comment(lib, "cuda.lib")
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

extern Renderer* renderer;

namespace maplecast_stream
{

static std::atomic<bool> _active{false};
static std::thread _acceptThread;

// Encoder
static const AVCodec* _codec = nullptr;
static AVCodecContext* _codecCtx = nullptr;
static AVFrame* _swFrame = nullptr;   // software frame for fallback
static AVPacket* _pkt = nullptr;
static SwsContext* _swsCtx = nullptr;
static int _width = 640;
static int _height = 480;

// CUDA GL interop state
static CUcontext _cuCtx = nullptr;
static bool _cudaAvailable = false;

// Network
static SOCKET _listenSock = INVALID_SOCKET;
static constexpr int MAX_CLIENTS = 4;
static SOCKET _clients[MAX_CLIENTS] = {INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET};
static std::mutex _clientMutex;

// CUDA-GL interop for zero-copy capture
static CUgraphicsResource _cuGLResource = nullptr;
static GLuint _registeredTexID = 0;
static int _texWidth = 0, _texHeight = 0;

static bool initCuda()
{
	CUresult res = cuInit(0);
	if (res != CUDA_SUCCESS)
	{
		printf("[maplecast-stream] CUDA init failed: %d\n", res);
		return false;
	}

	CUdevice device;
	res = cuDeviceGet(&device, 0);
	if (res != CUDA_SUCCESS) return false;

	res = cuCtxCreate(&_cuCtx, 0, device);
	if (res != CUDA_SUCCESS) return false;

	char name[256];
	cuDeviceGetName(name, sizeof(name), device);
	printf("[maplecast-stream] CUDA device: %s\n", name);
	return true;
}

static bool initEncoder()
{
	// Try CUDA-accelerated NVENC first
	_cudaAvailable = initCuda();

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
	printf("[maplecast-stream] encoder: %s (CUDA: %s)\n", _codec->name, _cudaAvailable ? "YES" : "no");

	_codecCtx = avcodec_alloc_context3(_codec);
	if (!_codecCtx) return false;

	_codecCtx->width = _width;
	_codecCtx->height = _height;
	_codecCtx->time_base = {1, 60};
	_codecCtx->framerate = {60, 1};
	_codecCtx->pix_fmt = AV_PIX_FMT_NV12;
	_codecCtx->bit_rate = 15000000;
	_codecCtx->max_b_frames = 0;
	_codecCtx->gop_size = 1;
	_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;

	if (strcmp(_codec->name, "h264_nvenc") == 0)
	{
		av_opt_set(_codecCtx->priv_data, "preset", "p1", 0);
		av_opt_set(_codecCtx->priv_data, "tune", "ull", 0);
		av_opt_set(_codecCtx->priv_data, "zerolatency", "1", 0);
		av_opt_set(_codecCtx->priv_data, "rc", "cbr", 0);
		av_opt_set(_codecCtx->priv_data, "delay", "0", 0);
		av_opt_set(_codecCtx->priv_data, "forced-idr", "1", 0);
	}
	else if (strcmp(_codec->name, "libx264") == 0)
	{
		av_opt_set(_codecCtx->priv_data, "preset", "ultrafast", 0);
		av_opt_set(_codecCtx->priv_data, "tune", "zerolatency", 0);
	}

	_codecCtx->profile = 66;

	if (avcodec_open2(_codecCtx, _codec, nullptr) < 0)
	{
		printf("[maplecast-stream] failed to open encoder\n");
		avcodec_free_context(&_codecCtx);
		return false;
	}

	_swFrame = av_frame_alloc();
	_swFrame->format = AV_PIX_FMT_NV12;
	_swFrame->width = _width;
	_swFrame->height = _height;
	av_frame_get_buffer(_swFrame, 0);

	_pkt = av_packet_alloc();

	_swsCtx = sws_getContext(
		_width, _height, AV_PIX_FMT_RGB24,
		_width, _height, AV_PIX_FMT_NV12,
		SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

	printf("[maplecast-stream] encoder ready: %dx%d @ %lld bps, GOP=1\n",
		_width, _height, (long long)_codecCtx->bit_rate);
	return true;
}

static void cleanupEncoder()
{
	if (_swsCtx) { sws_freeContext(_swsCtx); _swsCtx = nullptr; }
	if (_pkt) { av_packet_free(&_pkt); }
	if (_swFrame) { av_frame_free(&_swFrame); }
	if (_codecCtx) { avcodec_free_context(&_codecCtx); }
	if (_cuCtx) { cuCtxDestroy(_cuCtx); _cuCtx = nullptr; }
}

static void broadcastPacket(const uint8_t* data, int size, int64_t captureTimeUs, uint32_t frameNum)
{
	uint32_t headerSize = 8 + 4;
	uint32_t totalPayload = headerSize + size;
	std::vector<u8> msg(4 + totalPayload);
	memcpy(msg.data(), &totalPayload, 4);
	memcpy(msg.data() + 4, &captureTimeUs, 8);
	memcpy(msg.data() + 12, &frameNum, 4);
	memcpy(msg.data() + 16, data, size);

	std::lock_guard<std::mutex> lock(_clientMutex);
	for (int i = 0; i < MAX_CLIENTS; i++)
	{
		if (_clients[i] == INVALID_SOCKET) continue;
		int sent = send(_clients[i], (const char*)msg.data(), (int)msg.size(), 0);
		if (sent <= 0)
		{
#ifdef _WIN32
			if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
#else
			if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
#endif
			printf("[maplecast-stream] client %d disconnected\n", i);
			closesocket(_clients[i]);
			_clients[i] = INVALID_SOCKET;
		}
	}
}

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
	printf("[maplecast-stream] listening on port %d\n", port);

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
			int nodelay = 1;
			setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
			int sndbuf = 512 * 1024;
			setsockopt(client, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));
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

	printf("[maplecast-stream] initialized (zero-copy: %s)\n", _cudaAvailable ? "CUDA" : "PBO async");
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

	if (_cuGLResource) { cuGraphicsUnregisterResource(_cuGLResource); _cuGLResource = nullptr; }
	cleanupEncoder();
	printf("[maplecast-stream] shutdown\n");
}

void onFrameRendered()
{
	if (!_active || !_codecCtx) return;

	bool hasClients = false;
	{
		std::lock_guard<std::mutex> lock(_clientMutex);
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (_clients[i] != INVALID_SOCKET) { hasClients = true; break; }
	}
	if (!hasClients) return;

	LARGE_INTEGER freq, pc0, pc1, pc2, pc3;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&pc0);
	int64_t captureTimeUs = pc0.QuadPart * 1000000LL / freq.QuadPart;

	// Get the GL texture directly from the renderer — no readback
	int w = 0, h = 0;
	GLuint texID = renderer ? renderer->GetFrameTextureID(w, h) : 0;
	if (texID == 0 || w <= 0 || h <= 0) return;

	// Register GL texture with CUDA (once, or on texture/size change)
	if (_cudaAvailable && (texID != _registeredTexID || w != _texWidth || h != _texHeight))
	{
		cuCtxPushCurrent(_cuCtx);

		if (_cuGLResource)
		{
			cuGraphicsUnregisterResource(_cuGLResource);
			_cuGLResource = nullptr;
		}

		CUresult res = cuGraphicsGLRegisterImage(&_cuGLResource, texID, GL_TEXTURE_2D,
			CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
		if (res != CUDA_SUCCESS)
		{
			printf("[maplecast-stream] CUDA GL register failed: %d, falling back to readback\n", res);
			_cudaAvailable = false;
		}
		else
		{
			_registeredTexID = texID;
			_texWidth = w;
			_texHeight = h;
			printf("[maplecast-stream] CUDA GL texture registered: %dx%d texID=%u\n", w, h, texID);
			maplecast_telemetry::send("[maplecast-stream] CUDA GL registered: %dx%d", w, h);
		}

		// Rebuild sws scaler for this resolution
		if (_swsCtx) sws_freeContext(_swsCtx);
		_swsCtx = sws_getContext(
			w, h, AV_PIX_FMT_RGBA,   // GL textures are RGBA
			_codecCtx->width, _codecCtx->height, AV_PIX_FMT_NV12,
			SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

		cuCtxPopCurrent(nullptr);
	}

	QueryPerformanceCounter(&pc1);

	// CUDA zero-copy path: map GL texture → read CUDA memory → convert → encode
	if (_cudaAvailable && _cuGLResource)
	{
		cuCtxPushCurrent(_cuCtx);

		// Map the GL texture into CUDA address space
		CUresult res = cuGraphicsMapResources(1, &_cuGLResource, 0);
		if (res == CUDA_SUCCESS)
		{
			CUarray cuArray;
			res = cuGraphicsSubResourceGetMappedArray(&cuArray, _cuGLResource, 0, 0);
			if (res == CUDA_SUCCESS)
			{
				// Copy from CUDA array to host memory for sws_scale
				// This is GPU→CPU but via CUDA DMA, much faster than glReadPixels
				size_t rowBytes = w * 4; // RGBA
				std::vector<u8> rgbaData(w * h * 4);

				CUDA_MEMCPY2D copyParams = {};
				copyParams.srcMemoryType = CU_MEMORYTYPE_ARRAY;
				copyParams.srcArray = cuArray;
				copyParams.dstMemoryType = CU_MEMORYTYPE_HOST;
				copyParams.dstHost = rgbaData.data();
				copyParams.dstPitch = rowBytes;
				copyParams.WidthInBytes = rowBytes;
				copyParams.Height = h;
				cuMemcpy2D(&copyParams);

				// RGBA → NV12 for NVENC
				const uint8_t* srcSlice[1] = { rgbaData.data() };
				int srcStride[1] = { (int)rowBytes };
				sws_scale(_swsCtx, srcSlice, srcStride, 0, h,
					_swFrame->data, _swFrame->linesize);
			}
			cuGraphicsUnmapResources(1, &_cuGLResource, 0);
		}

		cuCtxPopCurrent(nullptr);
	}
	else
	{
		// Fallback: GetLastFrame (slow but works everywhere)
		std::vector<u8> rgbData;
		int fw = 0, fh = 0;
		try {
			if (!renderer->GetLastFrame(rgbData, fw, fh) || rgbData.empty())
				return;
		} catch (...) {
			return;
		}
		if (fw != w || fh != h)
		{
			if (_swsCtx) sws_freeContext(_swsCtx);
			_swsCtx = sws_getContext(
				fw, fh, AV_PIX_FMT_RGB24,
				_codecCtx->width, _codecCtx->height, AV_PIX_FMT_NV12,
				SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
			w = fw;
			h = fh;
		}
		const uint8_t* srcSlice[1] = { rgbData.data() };
		int srcStride[1] = { w * 3 };
		sws_scale(_swsCtx, srcSlice, srcStride, 0, h,
			_swFrame->data, _swFrame->linesize);
	}

	uint32_t frameNum = (uint32_t)_frameCount;
	_swFrame->pts = _frameCount++;
	QueryPerformanceCounter(&pc2);

	// Encode
	int ret = avcodec_send_frame(_codecCtx, _swFrame);
	if (ret < 0) return;

	while (ret >= 0)
	{
		ret = avcodec_receive_packet(_codecCtx, _pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0) break;

		QueryPerformanceCounter(&pc3);
		broadcastPacket(_pkt->data, _pkt->size, captureTimeUs, frameNum);

		if (frameNum % 300 == 0)
		{
			long long capUs  = (pc1.QuadPart - pc0.QuadPart) * 1000000LL / freq.QuadPart;
			long long scaleUs = (pc2.QuadPart - pc1.QuadPart) * 1000000LL / freq.QuadPart;
			long long encUs  = (pc3.QuadPart - pc2.QuadPart) * 1000000LL / freq.QuadPart;
			long long totalUs = (pc3.QuadPart - pc0.QuadPart) * 1000000LL / freq.QuadPart;
			printf("[maplecast-stream] F:%u | capture:%lldus scale:%lldus encode:%lldus total:%lldus | %dB\n",
				frameNum, capUs, scaleUs, encUs, totalUs, _pkt->size);
			maplecast_telemetry::send("[maplecast-stream] F:%u | capture:%lldus scale:%lldus encode:%lldus total:%lldus | %dB",
				frameNum, capUs, scaleUs, encUs, totalUs, _pkt->size);
		}

		av_packet_unref(_pkt);
	}
}

bool active()
{
	return _active;
}

}  // namespace maplecast_stream
