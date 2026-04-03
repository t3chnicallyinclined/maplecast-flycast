/*
	MapleCast Stream — TRUE ZERO-COPY GPU pipeline.

	GL texture → CUDA map → NVENC encodes directly from GPU → H.264 out.
	CPU NEVER touches the pixel data. Only the final encoded H.264 bitstream
	(~30KB per frame) reaches CPU for TCP send.

	No FFmpeg. No sws_scale. No glReadPixels. No std::vector<u8> allocations.
	Pure NVIDIA hardware: CUDA + NVENC on RTX 3090.
*/

// Winsock MUST be before anything that includes windows.h (CUDA does)
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "cuda.lib")
typedef int socklen_t;
#endif

#include "maplecast_stream.h"
#include "maplecast.h"
#include "maplecast_telemetry.h"
#include "hw/pvr/Renderer_if.h"
#include "wsi/gl_context.h"

// CUDA driver API + GL interop
#include <cuda.h>
#include <cudaGL.h>

// NVENC direct API
#include <ffnvcodec/nvEncodeAPI.h>

#ifndef _WIN32
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
#include <mutex>
#include <thread>
#include <atomic>

extern Renderer* renderer;

namespace maplecast_stream
{

static std::atomic<bool> _active{false};
static std::thread _acceptThread;
static int _width = 640;
static int _height = 480;

// CUDA
static CUcontext _cuCtx = nullptr;

// CUDA-GL interop
static CUgraphicsResource _cuGLResource = nullptr;
static GLuint _registeredTexID = 0;
static CUdeviceptr _cudaLinearBuf = 0;   // linear CUDA buffer for NVENC input
static size_t _cudaLinearPitch = 0;

// NVENC
typedef NVENCSTATUS (NVENCAPI *PFN_NvEncodeAPICreateInstance)(NV_ENCODE_API_FUNCTION_LIST*);
static NV_ENCODE_API_FUNCTION_LIST _nvenc = {};
static void* _encoder = nullptr;
static NV_ENC_REGISTERED_PTR _nvencRegisteredRes = nullptr;
static NV_ENC_OUTPUT_PTR _nvencOutputBuf = nullptr;

// Network
static SOCKET _listenSock = INVALID_SOCKET;
static constexpr int MAX_CLIENTS = 4;
static SOCKET _clients[MAX_CLIENTS] = {INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET};
static std::mutex _clientMutex;
static std::atomic<int> _clientCount{0};

// Pre-allocated send buffer (reused every frame, no heap alloc)
static uint8_t* _sendBuf = nullptr;
static size_t _sendBufSize = 0;

static bool initCuda()
{
	CUresult res = cuInit(0);
	if (res != CUDA_SUCCESS) { printf("[maplecast-stream] CUDA init failed: %d\n", res); return false; }

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

static bool initNvenc()
{
	// Load NVENC DLL
#ifdef _WIN32
	HMODULE hNvenc = LoadLibraryA("nvEncodeAPI64.dll");
	if (!hNvenc) { printf("[maplecast-stream] nvEncodeAPI64.dll not found\n"); return false; }
	auto createInstance = (PFN_NvEncodeAPICreateInstance)GetProcAddress(hNvenc, "NvEncodeAPICreateInstance");
#else
	void* hNvenc = dlopen("libnvidia-encode.so.1", RTLD_LAZY);
	if (!hNvenc) return false;
	auto createInstance = (PFN_NvEncodeAPICreateInstance)dlsym(hNvenc, "NvEncodeAPICreateInstance");
#endif
	if (!createInstance) { printf("[maplecast-stream] NvEncodeAPICreateInstance not found\n"); return false; }

	// Get function pointers
	memset(&_nvenc, 0, sizeof(_nvenc));
	_nvenc.version = NV_ENCODE_API_FUNCTION_LIST_VER;
	NVENCSTATUS st = createInstance(&_nvenc);
	if (st != NV_ENC_SUCCESS) { printf("[maplecast-stream] NvEncodeAPICreateInstance failed: %d\n", st); return false; }

	// Open encode session on CUDA context
	NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = {};
	sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
	sessionParams.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
	sessionParams.device = _cuCtx;
	sessionParams.apiVersion = NVENCAPI_VERSION;

	st = _nvenc.nvEncOpenEncodeSessionEx(&sessionParams, &_encoder);
	if (st != NV_ENC_SUCCESS) { printf("[maplecast-stream] nvEncOpenEncodeSessionEx failed: %d\n", st); return false; }

	// Configure encoder
	NV_ENC_INITIALIZE_PARAMS initParams = {};
	initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
	initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
	initParams.presetGUID = NV_ENC_PRESET_P1_GUID;
	initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
	initParams.encodeWidth = _width;
	initParams.encodeHeight = _height;
	initParams.darWidth = _width;
	initParams.darHeight = _height;
	initParams.frameRateNum = 60;
	initParams.frameRateDen = 1;
	initParams.enablePTD = 1;  // picture type decision by encoder

	// Get preset config
	NV_ENC_PRESET_CONFIG presetConfig = {};
	presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
	presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
	st = _nvenc.nvEncGetEncodePresetConfigEx(_encoder, NV_ENC_CODEC_H264_GUID,
		NV_ENC_PRESET_P1_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &presetConfig);
	if (st != NV_ENC_SUCCESS) { printf("[maplecast-stream] nvEncGetEncodePresetConfigEx failed: %d\n", st); return false; }

	NV_ENC_CONFIG encConfig = presetConfig.presetCfg;

	// Ultra low latency overrides
	encConfig.gopLength = 1;                          // Every frame is IDR
	encConfig.frameIntervalP = 1;                     // No B-frames
	encConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
	encConfig.rcParams.averageBitRate = 15000000;     // 15 Mbps
	encConfig.rcParams.maxBitRate = 20000000;         // 20 Mbps peak
	encConfig.rcParams.vbvBufferSize = 15000000 / 60; // 1 frame buffer
	encConfig.rcParams.vbvInitialDelay = encConfig.rcParams.vbvBufferSize;
	encConfig.rcParams.zeroReorderDelay = 1;          // No reorder delay
	encConfig.rcParams.lowDelayKeyFrameScale = 1;

	// H.264 specific
	encConfig.encodeCodecConfig.h264Config.idrPeriod = 1;  // Every frame IDR
	encConfig.encodeCodecConfig.h264Config.sliceMode = 0;  // One slice per frame
	encConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1; // SPS/PPS with every IDR
	encConfig.encodeCodecConfig.h264Config.chromaFormatIDC = 1; // YUV420

	initParams.encodeConfig = &encConfig;

	st = _nvenc.nvEncInitializeEncoder(_encoder, &initParams);
	if (st != NV_ENC_SUCCESS) { printf("[maplecast-stream] nvEncInitializeEncoder failed: %d\n", st); return false; }

	// Create output bitstream buffer
	NV_ENC_CREATE_BITSTREAM_BUFFER bitstreamParams = {};
	bitstreamParams.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
	st = _nvenc.nvEncCreateBitstreamBuffer(_encoder, &bitstreamParams);
	if (st != NV_ENC_SUCCESS) { printf("[maplecast-stream] nvEncCreateBitstreamBuffer failed: %d\n", st); return false; }
	_nvencOutputBuf = bitstreamParams.bitstreamBuffer;

	printf("[maplecast-stream] NVENC initialized: %dx%d, H.264 Baseline, P1/ULL, GOP=1, 15Mbps CBR\n", _width, _height);
	return true;
}

static bool initCudaLinearBuffer()
{
	// Allocate a linear CUDA buffer for NVENC input (ABGR, _width x _height)
	// NVENC can't read from CUDA arrays directly — needs a linear pitched allocation
	size_t pitch;
	CUresult res = cuMemAllocPitch(&_cudaLinearBuf, &pitch, _width * 4, _height, 16);
	if (res != CUDA_SUCCESS) { printf("[maplecast-stream] cuMemAllocPitch failed: %d\n", res); return false; }
	_cudaLinearPitch = pitch;

	// Register with NVENC
	NV_ENC_REGISTER_RESOURCE regRes = {};
	regRes.version = NV_ENC_REGISTER_RESOURCE_VER;
	regRes.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
	regRes.resourceToRegister = (void*)_cudaLinearBuf;
	regRes.width = _width;
	regRes.height = _height;
	regRes.pitch = (uint32_t)_cudaLinearPitch;
	regRes.bufferFormat = NV_ENC_BUFFER_FORMAT_ABGR;
	regRes.bufferUsage = NV_ENC_INPUT_IMAGE;

	NVENCSTATUS st = _nvenc.nvEncRegisterResource(_encoder, &regRes);
	if (st != NV_ENC_SUCCESS) { printf("[maplecast-stream] nvEncRegisterResource failed: %d\n", st); return false; }
	_nvencRegisteredRes = regRes.registeredResource;

	printf("[maplecast-stream] CUDA linear buffer: %dx%d, pitch=%zu, registered with NVENC\n",
		_width, _height, _cudaLinearPitch);
	return true;
}

static void broadcastPacket(const uint8_t* data, uint32_t size,
	int64_t inputTimeUs, int64_t captureTimeUs, int64_t encodeDoneTimeUs, uint32_t frameNum)
{
	// Get player stats
	maplecast::PlayerStats p1s, p2s;
	maplecast::getPlayerStats(p1s, p2s);

	// Header: [total_len(4)] + timing(28) + p1stats(8) + p2stats(8) + [h264 data]
	// Timing:  [inputTimeUs(8)][captureTimeUs(8)][encodeDoneTimeUs(8)][frame_num(4)] = 28
	// P1 stats: [pktPerSec(2)][chgPerSec(2)][buttons(2)][lt(1)][rt(1)] = 8
	// P2 stats: same = 8
	// Total header: 28 + 8 + 8 = 44 bytes
	uint32_t headerSize = 28 + 8 + 8;  // 44 bytes
	uint32_t totalPayload = headerSize + size;
	uint32_t totalMsg = 4 + totalPayload;

	if (totalMsg > _sendBufSize)
	{
		free(_sendBuf);
		_sendBufSize = totalMsg + 65536;
		_sendBuf = (uint8_t*)malloc(_sendBufSize);
	}

	uint32_t off = 0;
	memcpy(_sendBuf + off, &totalPayload, 4); off += 4;
	// Timing
	memcpy(_sendBuf + off, &inputTimeUs, 8); off += 8;
	memcpy(_sendBuf + off, &captureTimeUs, 8); off += 8;
	memcpy(_sendBuf + off, &encodeDoneTimeUs, 8); off += 8;
	memcpy(_sendBuf + off, &frameNum, 4); off += 4;
	// P1 stats
	uint16_t p1pps = (uint16_t)p1s.packetsPerSec;
	uint16_t p1cps = (uint16_t)p1s.changesPerSec;
	uint16_t p1btn = p1s.buttons;
	memcpy(_sendBuf + off, &p1pps, 2); off += 2;
	memcpy(_sendBuf + off, &p1cps, 2); off += 2;
	memcpy(_sendBuf + off, &p1btn, 2); off += 2;
	_sendBuf[off++] = p1s.lt;
	_sendBuf[off++] = p1s.rt;
	// P2 stats
	uint16_t p2pps = (uint16_t)p2s.packetsPerSec;
	uint16_t p2cps = (uint16_t)p2s.changesPerSec;
	uint16_t p2btn = p2s.buttons;
	memcpy(_sendBuf + off, &p2pps, 2); off += 2;
	memcpy(_sendBuf + off, &p2cps, 2); off += 2;
	memcpy(_sendBuf + off, &p2btn, 2); off += 2;
	_sendBuf[off++] = p2s.lt;
	_sendBuf[off++] = p2s.rt;
	// H.264 payload
	memcpy(_sendBuf + off, data, size);

	std::lock_guard<std::mutex> lock(_clientMutex);
	for (int i = 0; i < MAX_CLIENTS; i++)
	{
		if (_clients[i] == INVALID_SOCKET) continue;
		int sent = send(_clients[i], (const char*)_sendBuf, (int)totalMsg, 0);
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
			_clientCount--;
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
#endif
			std::lock_guard<std::mutex> lock(_clientMutex);
			for (int i = 0; i < MAX_CLIENTS; i++)
			{
				if (_clients[i] == INVALID_SOCKET)
				{
					_clients[i] = client;
					_clientCount++;
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

	// Pre-allocate send buffer (256KB, grows if needed)
	_sendBufSize = 256 * 1024;
	_sendBuf = (uint8_t*)malloc(_sendBufSize);

	if (!initCuda()) return false;
	if (!initNvenc()) return false;
	if (!initCudaLinearBuffer()) return false;

	_active = true;
	_frameCount = 0;
	_acceptThread = std::thread(acceptLoop, wsPort);

	printf("[maplecast-stream] ZERO-COPY pipeline ready: GL texture → CUDA → NVENC → wire\n");
	maplecast_telemetry::send("[maplecast-stream] ZERO-COPY ready");
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

	if (_nvencRegisteredRes && _encoder)
		_nvenc.nvEncUnregisterResource(_encoder, _nvencRegisteredRes);
	if (_nvencOutputBuf && _encoder)
		_nvenc.nvEncDestroyBitstreamBuffer(_encoder, _nvencOutputBuf);
	if (_encoder)
		_nvenc.nvEncDestroyEncoder(_encoder);
	if (_cudaLinearBuf)
		cuMemFree(_cudaLinearBuf);
	if (_cuGLResource)
		cuGraphicsUnregisterResource(_cuGLResource);
	if (_cuCtx)
		cuCtxDestroy(_cuCtx);

	free(_sendBuf);
	_sendBuf = nullptr;

	printf("[maplecast-stream] shutdown\n");
}

void onFrameRendered()
{
	if (!_active || !_encoder) return;
	if (_clientCount.load(std::memory_order_relaxed) == 0) return;

	static LARGE_INTEGER freq = {}; // cached — never changes
	if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
	LARGE_INTEGER pc0, pc1, pc2, pc3;
	QueryPerformanceCounter(&pc0);
	int64_t captureTimeUs = pc0.QuadPart * 1000000LL / freq.QuadPart;

	// Get GL texture from renderer
	int w = 0, h = 0;
	GLuint texID = renderer ? renderer->GetFrameTextureID(w, h) : 0;
	if (texID == 0 || w <= 0 || h <= 0)
	{
		static bool logged = false;
		if (!logged)
		{
			printf("[maplecast-stream] WARNING: GetFrameTextureID returned 0 — renderer may not be OpenGL!\n");
			printf("[maplecast-stream] Set 'pvr.rend=0' in emu.cfg to force OpenGL\n");
			maplecast_telemetry::send("[maplecast-stream] GetFrameTextureID=0 — NOT OpenGL");
			logged = true;
		}
		return;
	}

	// Register GL texture with CUDA (once, or on change)
	if (texID != _registeredTexID)
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
			printf("[maplecast-stream] cuGraphicsGLRegisterImage failed: %d\n", res);
			cuCtxPopCurrent(nullptr);
			return;
		}
		_registeredTexID = texID;
		printf("[maplecast-stream] GL texture %u registered with CUDA (%dx%d)\n", texID, w, h);
		cuCtxPopCurrent(nullptr);
	}

	QueryPerformanceCounter(&pc1);

	// Map GL texture → CUDA array → copy to linear buffer (GPU→GPU, no CPU)
	cuCtxPushCurrent(_cuCtx);

	CUresult res = cuGraphicsMapResources(1, &_cuGLResource, 0);
	if (res != CUDA_SUCCESS) { cuCtxPopCurrent(nullptr); return; }

	CUarray cuArray;
	res = cuGraphicsSubResourceGetMappedArray(&cuArray, _cuGLResource, 0, 0);
	if (res != CUDA_SUCCESS)
	{
		cuGraphicsUnmapResources(1, &_cuGLResource, 0);
		cuCtxPopCurrent(nullptr);
		return;
	}

	// GPU-to-GPU copy: CUDA array → linear pitched buffer
	// This stays entirely on the GPU. CPU never sees the pixels.
	CUDA_MEMCPY2D cp = {};
	cp.srcMemoryType = CU_MEMORYTYPE_ARRAY;
	cp.srcArray = cuArray;
	cp.dstMemoryType = CU_MEMORYTYPE_DEVICE;
	cp.dstDevice = _cudaLinearBuf;
	cp.dstPitch = _cudaLinearPitch;
	cp.WidthInBytes = _width * 4;  // RGBA = 4 bytes per pixel
	cp.Height = _height;
	cuMemcpy2D(&cp);

	cuGraphicsUnmapResources(1, &_cuGLResource, 0);

	QueryPerformanceCounter(&pc2);

	// NVENC encode directly from CUDA device memory
	NV_ENC_MAP_INPUT_RESOURCE mapRes = {};
	mapRes.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
	mapRes.registeredResource = _nvencRegisteredRes;
	NVENCSTATUS st = _nvenc.nvEncMapInputResource(_encoder, &mapRes);
	if (st != NV_ENC_SUCCESS)
	{
		cuCtxPopCurrent(nullptr);
		return;
	}

	NV_ENC_PIC_PARAMS picParams = {};
	picParams.version = NV_ENC_PIC_PARAMS_VER;
	picParams.inputBuffer = mapRes.mappedResource;
	picParams.bufferFmt = mapRes.mappedBufferFmt;
	picParams.inputWidth = _width;
	picParams.inputHeight = _height;
	picParams.outputBitstream = _nvencOutputBuf;
	picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
	picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
	picParams.inputTimeStamp = _frameCount;

	st = _nvenc.nvEncEncodePicture(_encoder, &picParams);
	_nvenc.nvEncUnmapInputResource(_encoder, mapRes.mappedResource);

	if (st != NV_ENC_SUCCESS && st != NV_ENC_ERR_NEED_MORE_INPUT)
	{
		cuCtxPopCurrent(nullptr);
		return;
	}

	// Lock bitstream — this is the ONLY data that touches CPU (~30KB encoded H.264)
	NV_ENC_LOCK_BITSTREAM lockParams = {};
	lockParams.version = NV_ENC_LOCK_BITSTREAM_VER;
	lockParams.outputBitstream = _nvencOutputBuf;

	st = _nvenc.nvEncLockBitstream(_encoder, &lockParams);
	if (st == NV_ENC_SUCCESS)
	{
		QueryPerformanceCounter(&pc3);
		int64_t encodeDoneTimeUs = pc3.QuadPart * 1000000LL / freq.QuadPart;
		int64_t inputTimeUs = ::maplecast::lastInputTimeUs();

		uint32_t frameNum = (uint32_t)_frameCount;
		broadcastPacket((const uint8_t*)lockParams.bitstreamBufferPtr,
			lockParams.bitstreamSizeInBytes, inputTimeUs, captureTimeUs, encodeDoneTimeUs, frameNum);

		// Telemetry every 300 frames
		if (frameNum % 300 == 0)
		{
			long long mapUs   = (pc1.QuadPart - pc0.QuadPart) * 1000000LL / freq.QuadPart;
			long long copyUs  = (pc2.QuadPart - pc1.QuadPart) * 1000000LL / freq.QuadPart;
			long long encUs   = (pc3.QuadPart - pc2.QuadPart) * 1000000LL / freq.QuadPart;
			long long totalUs = (pc3.QuadPart - pc0.QuadPart) * 1000000LL / freq.QuadPart;
			printf("[maplecast-stream] F:%u | map:%lldus copy:%lldus encode:%lldus total:%lldus | %uB\n",
				frameNum, mapUs, copyUs, encUs, totalUs, lockParams.bitstreamSizeInBytes);
			maplecast_telemetry::send("[maplecast-stream] F:%u | map:%lldus copy:%lldus encode:%lldus total:%lldus | %uB",
				frameNum, mapUs, copyUs, encUs, totalUs, lockParams.bitstreamSizeInBytes);
		}

		_nvenc.nvEncUnlockBitstream(_encoder, _nvencOutputBuf);
	}

	cuCtxPopCurrent(nullptr);
	_frameCount++;
}

bool active()
{
	return _active;
}

}  // namespace maplecast_stream
