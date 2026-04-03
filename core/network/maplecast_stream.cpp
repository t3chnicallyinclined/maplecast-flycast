/*
	MapleCast Stream — TRUE ZERO-COPY GPU pipeline + direct WebSocket.

	GL texture → CUDA map → NVENC encodes directly from GPU → H.264 out.
	WebSocket server built in — NO PROXY NEEDED. Browser connects directly.

	Binary messages: video frames (server→client), W3 gamepad (client→server)
	Text messages: JSON control (join/assign/status)

	No FFmpeg. No proxy. No sws_scale. No glReadPixels.
	Pure NVIDIA hardware: CUDA + NVENC on RTX 3090.
*/

// Winsock MUST be before anything that includes windows.h (CUDA does)
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "cuda.lib")
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

// WebSocket server — built into Flycast deps, no external proxy needed
// ASIO/WSPP macros already defined on command line by Flycast's CMake
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <atomic>
#include <set>
#include <string>

// JSON helpers (minimal — no library needed for our simple protocol)
#include "json/json.hpp"
using json = nlohmann::json;

extern Renderer* renderer;

namespace maplecast_stream
{

using WsServer = websocketpp::server<websocketpp::config::asio>;
using ConnHdl = websocketpp::connection_hdl;

static std::atomic<bool> _active{false};
static int _width = 640;
static int _height = 480;

// CUDA
static CUcontext _cuCtx = nullptr;

// CUDA-GL interop
static CUgraphicsResource _cuGLResource = nullptr;
static GLuint _registeredTexID = 0;
static CUdeviceptr _cudaLinearBuf = 0;
static size_t _cudaLinearPitch = 0;

// NVENC
typedef NVENCSTATUS (NVENCAPI *PFN_NvEncodeAPICreateInstance)(NV_ENCODE_API_FUNCTION_LIST*);
static NV_ENCODE_API_FUNCTION_LIST _nvenc = {};
static void* _encoder = nullptr;
static NV_ENC_REGISTERED_PTR _nvencRegisteredRes = nullptr;
static NV_ENC_OUTPUT_PTR _nvencOutputBuf = nullptr;

// WebSocket server
static WsServer _ws;
static std::thread _wsThread;
static std::set<ConnHdl, std::owner_less<ConnHdl>> _connections;
static std::mutex _connMutex;
static std::atomic<int> _clientCount{0};

// Player registry (moved from proxy.py to C++)
struct WsPlayer {
	std::string id;
	std::string name;
	int slot;        // 0=P1, 1=P2, -1=spectator
	ConnHdl conn;
};
static std::map<std::string, WsPlayer> _players;  // id → player
static std::string _slotOwner[2] = {"", ""};       // slot → player_id

// Pre-allocated send buffer
static uint8_t* _sendBuf = nullptr;
static size_t _sendBufSize = 0;

// Hardware player detection
static constexpr uint32_t HW_THRESHOLD = 1000;

static int assignSlot(const std::string& playerId, const std::string& name)
{
	// Already assigned?
	auto it = _players.find(playerId);
	if (it != _players.end() && it->second.slot >= 0)
	{
		printf("[maplecast-ws] %s reconnected as P%d\n", name.c_str(), it->second.slot + 1);
		return it->second.slot;
	}

	// Find open slot — skip hardware-occupied slots
	maplecast::PlayerStats p1s, p2s;
	maplecast::getPlayerStats(p1s, p2s);

	for (int i = 0; i < 2; i++)
	{
		uint32_t hwPps = (i == 0) ? p1s.packetsPerSec : p2s.packetsPerSec;
		if (_slotOwner[i].empty() && hwPps < HW_THRESHOLD)
		{
			_slotOwner[i] = playerId;
			printf("[maplecast-ws] %s assigned P%d\n", name.c_str(), i + 1);
			return i;
		}
	}

	printf("[maplecast-ws] %s — no slots available\n", name.c_str());
	return -1;
}

static json getStatus()
{
	maplecast::PlayerStats p1s, p2s;
	maplecast::getPlayerStats(p1s, p2s);

	auto slotInfo = [&](int i) -> json {
		uint32_t hwPps = (i == 0) ? p1s.packetsPerSec : p2s.packetsPerSec;
		if (!_slotOwner[i].empty())
		{
			auto it = _players.find(_slotOwner[i]);
			if (it != _players.end())
				return {{"id", it->second.id.substr(0,8)}, {"name", it->second.name}, {"connected", true}, {"type", "browser"}};
		}
		if (hwPps >= HW_THRESHOLD)
			return {{"id", "hardware"}, {"name", "Stick (" + std::to_string(hwPps) + "/s)"}, {"connected", true}, {"type", "hardware"}};
		return nullptr;
	};

	return {{"type", "status"}, {"p1", slotInfo(0)}, {"p2", slotInfo(1)}};
}

static void onWsOpen(ConnHdl hdl)
{
	std::lock_guard<std::mutex> lock(_connMutex);
	_connections.insert(hdl);
	_clientCount++;
	printf("[maplecast-ws] client connected (%d total)\n", _clientCount.load());

	// Send current status
	try {
		_ws.send(hdl, getStatus().dump(), websocketpp::frame::opcode::text);
	} catch (...) {}
}

static void onWsClose(ConnHdl hdl)
{
	std::lock_guard<std::mutex> lock(_connMutex);
	_connections.erase(hdl);
	_clientCount--;
	printf("[maplecast-ws] client disconnected (%d total)\n", _clientCount.load());

	// Find and release player slot (keep reservation)
	for (auto& [id, p] : _players)
	{
		try {
			if (!(p.conn.owner_before(hdl) || hdl.owner_before(p.conn)))
			{
				// Same connection — mark as disconnected but keep slot
				printf("[maplecast-ws] P%d (%s) disconnected, slot reserved\n", p.slot + 1, p.name.c_str());
				break;
			}
		} catch (...) {}
	}
}

static void onWsMessage(ConnHdl hdl, WsServer::message_ptr msg)
{
	if (msg->get_opcode() == websocketpp::frame::opcode::binary)
	{
		// Binary = W3 gamepad input (4 bytes)
		const std::string& data = msg->get_payload();
		if (data.size() >= 4)
		{
			// Forward to Flycast via UDP (same as proxy did)
			static int udpSock = -1;
			static struct sockaddr_in udpDest;
			if (udpSock < 0)
			{
				udpSock = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
				memset(&udpDest, 0, sizeof(udpDest));
				udpDest.sin_family = AF_INET;
				udpDest.sin_port = htons(7100);
				udpDest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			}
			sendto(udpSock, data.c_str(), 4, 0, (struct sockaddr*)&udpDest, sizeof(udpDest));
		}
	}
	else if (msg->get_opcode() == websocketpp::frame::opcode::text)
	{
		// Text = JSON control
		try {
			auto ctrl = json::parse(msg->get_payload());
			if (ctrl["type"] == "join")
			{
				std::string playerId = ctrl.value("id", "");
				std::string name = ctrl.value("name", "Player");

				int slot = assignSlot(playerId, name);
				_players[playerId] = {playerId, name, slot, hdl};

				json resp = {{"type", "assigned"}, {"slot", slot}, {"id", playerId.substr(0,8)}, {"name", name}};
				_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);

				// Broadcast status to all
				std::string status = getStatus().dump();
				std::lock_guard<std::mutex> lock(_connMutex);
				for (auto& conn : _connections)
				{
					try { _ws.send(conn, status, websocketpp::frame::opcode::text); }
					catch (...) {}
				}
			}
		} catch (...) {}
	}
}

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
#ifdef _WIN32
	HMODULE hNvenc = LoadLibraryA("nvEncodeAPI64.dll");
	if (!hNvenc) { printf("[maplecast-stream] nvEncodeAPI64.dll not found\n"); return false; }
	auto createInstance = (PFN_NvEncodeAPICreateInstance)GetProcAddress(hNvenc, "NvEncodeAPICreateInstance");
#else
	void* hNvenc = dlopen("libnvidia-encode.so.1", RTLD_LAZY);
	if (!hNvenc) return false;
	auto createInstance = (PFN_NvEncodeAPICreateInstance)dlsym(hNvenc, "NvEncodeAPICreateInstance");
#endif
	if (!createInstance) return false;

	memset(&_nvenc, 0, sizeof(_nvenc));
	_nvenc.version = NV_ENCODE_API_FUNCTION_LIST_VER;
	if (createInstance(&_nvenc) != NV_ENC_SUCCESS) return false;

	NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = {};
	sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
	sessionParams.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
	sessionParams.device = _cuCtx;
	sessionParams.apiVersion = NVENCAPI_VERSION;
	if (_nvenc.nvEncOpenEncodeSessionEx(&sessionParams, &_encoder) != NV_ENC_SUCCESS) return false;

	NV_ENC_PRESET_CONFIG presetConfig = {};
	presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
	presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
	_nvenc.nvEncGetEncodePresetConfigEx(_encoder, NV_ENC_CODEC_H264_GUID,
		NV_ENC_PRESET_P1_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &presetConfig);

	NV_ENC_CONFIG encConfig = presetConfig.presetCfg;
	encConfig.gopLength = 1;
	encConfig.frameIntervalP = 1;
	encConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
	encConfig.rcParams.averageBitRate = 15000000;
	encConfig.rcParams.maxBitRate = 20000000;
	encConfig.rcParams.vbvBufferSize = 15000000 / 60;
	encConfig.rcParams.vbvInitialDelay = encConfig.rcParams.vbvBufferSize;
	encConfig.rcParams.zeroReorderDelay = 1;
	encConfig.encodeCodecConfig.h264Config.idrPeriod = 1;
	encConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
	encConfig.encodeCodecConfig.h264Config.chromaFormatIDC = 1;

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
	initParams.enablePTD = 1;
	initParams.encodeConfig = &encConfig;
	if (_nvenc.nvEncInitializeEncoder(_encoder, &initParams) != NV_ENC_SUCCESS) return false;

	NV_ENC_CREATE_BITSTREAM_BUFFER bitstreamParams = {};
	bitstreamParams.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
	if (_nvenc.nvEncCreateBitstreamBuffer(_encoder, &bitstreamParams) != NV_ENC_SUCCESS) return false;
	_nvencOutputBuf = bitstreamParams.bitstreamBuffer;

	printf("[maplecast-stream] NVENC: %dx%d H.264 P1/ULL GOP=1 15Mbps\n", _width, _height);
	return true;
}

static bool initCudaLinearBuffer()
{
	size_t pitch;
	if (cuMemAllocPitch(&_cudaLinearBuf, &pitch, _width * 4, _height, 16) != CUDA_SUCCESS) return false;
	_cudaLinearPitch = pitch;

	NV_ENC_REGISTER_RESOURCE regRes = {};
	regRes.version = NV_ENC_REGISTER_RESOURCE_VER;
	regRes.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
	regRes.resourceToRegister = (void*)_cudaLinearBuf;
	regRes.width = _width;
	regRes.height = _height;
	regRes.pitch = (uint32_t)_cudaLinearPitch;
	regRes.bufferFormat = NV_ENC_BUFFER_FORMAT_ABGR;
	regRes.bufferUsage = NV_ENC_INPUT_IMAGE;
	if (_nvenc.nvEncRegisterResource(_encoder, &regRes) != NV_ENC_SUCCESS) return false;
	_nvencRegisteredRes = regRes.registeredResource;

	printf("[maplecast-stream] CUDA buffer: %dx%d pitch=%zu\n", _width, _height, _cudaLinearPitch);
	return true;
}

static void broadcastBinary(const void* data, size_t size)
{
	std::lock_guard<std::mutex> lock(_connMutex);
	for (auto& conn : _connections)
	{
		try {
			_ws.send(conn, data, size, websocketpp::frame::opcode::binary);
		} catch (...) {}
	}
}

static int64_t _frameCount = 0;

bool init(int port)
{
#ifdef _WIN32
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

	_sendBufSize = 256 * 1024;
	_sendBuf = (uint8_t*)malloc(_sendBufSize);

	if (!initCuda()) return false;
	if (!initNvenc()) return false;
	if (!initCudaLinearBuffer()) return false;

	// Start WebSocket server
	try {
		_ws.clear_access_channels(websocketpp::log::alevel::all);
		_ws.clear_error_channels(websocketpp::log::elevel::all);
		_ws.init_asio();
		_ws.set_reuse_addr(true);

		_ws.set_open_handler(&onWsOpen);
		_ws.set_close_handler(&onWsClose);
		_ws.set_message_handler(&onWsMessage);

		_ws.listen(port);
		_ws.start_accept();

		_wsThread = std::thread([&]() { _ws.run(); });

		printf("[maplecast-stream] WebSocket server on ws://0.0.0.0:%d (NO PROXY)\n", port);
	} catch (const std::exception& e) {
		printf("[maplecast-stream] WebSocket init failed: %s\n", e.what());
		return false;
	}

	_active = true;
	_frameCount = 0;

	printf("[maplecast-stream] ZERO-COPY + DIRECT WebSocket ready\n");
	maplecast_telemetry::send("[maplecast-stream] ZERO-COPY + WS ready");
	return true;
}

void shutdown()
{
	_active = false;
	try { _ws.stop(); } catch (...) {}
	if (_wsThread.joinable()) _wsThread.join();

	if (_nvencRegisteredRes && _encoder)
		_nvenc.nvEncUnregisterResource(_encoder, _nvencRegisteredRes);
	if (_nvencOutputBuf && _encoder)
		_nvenc.nvEncDestroyBitstreamBuffer(_encoder, _nvencOutputBuf);
	if (_encoder)
		_nvenc.nvEncDestroyEncoder(_encoder);
	if (_cudaLinearBuf) cuMemFree(_cudaLinearBuf);
	if (_cuGLResource) cuGraphicsUnregisterResource(_cuGLResource);
	if (_cuCtx) cuCtxDestroy(_cuCtx);

	free(_sendBuf); _sendBuf = nullptr;
	printf("[maplecast-stream] shutdown\n");
}

void onFrameRendered()
{
	if (!_active || !_encoder) return;
	if (_clientCount.load(std::memory_order_relaxed) == 0) return;

	static LARGE_INTEGER freq = {};
	if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
	LARGE_INTEGER pc0, pc1, pc2, pc3;
	QueryPerformanceCounter(&pc0);
	int64_t captureTimeUs = pc0.QuadPart * 1000000LL / freq.QuadPart;

	int w = 0, h = 0;
	GLuint texID = renderer ? renderer->GetFrameTextureID(w, h) : 0;
	if (texID == 0 || w <= 0 || h <= 0)
	{
		static bool logged = false;
		if (!logged) { printf("[maplecast-stream] GetFrameTextureID=0 — set pvr.rend=0\n"); logged = true; }
		return;
	}

	if (texID != _registeredTexID)
	{
		cuCtxPushCurrent(_cuCtx);
		if (_cuGLResource) { cuGraphicsUnregisterResource(_cuGLResource); _cuGLResource = nullptr; }
		if (cuGraphicsGLRegisterImage(&_cuGLResource, texID, GL_TEXTURE_2D,
			CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY) != CUDA_SUCCESS)
		{
			cuCtxPopCurrent(nullptr);
			return;
		}
		_registeredTexID = texID;
		printf("[maplecast-stream] GL tex %u registered (%dx%d)\n", texID, w, h);
		cuCtxPopCurrent(nullptr);
	}

	QueryPerformanceCounter(&pc1);

	cuCtxPushCurrent(_cuCtx);

	if (cuGraphicsMapResources(1, &_cuGLResource, 0) != CUDA_SUCCESS)
	{
		cuCtxPopCurrent(nullptr);
		return;
	}

	CUarray cuArray;
	if (cuGraphicsSubResourceGetMappedArray(&cuArray, _cuGLResource, 0, 0) != CUDA_SUCCESS)
	{
		cuGraphicsUnmapResources(1, &_cuGLResource, 0);
		cuCtxPopCurrent(nullptr);
		return;
	}

	CUDA_MEMCPY2D cp = {};
	cp.srcMemoryType = CU_MEMORYTYPE_ARRAY;
	cp.srcArray = cuArray;
	cp.dstMemoryType = CU_MEMORYTYPE_DEVICE;
	cp.dstDevice = _cudaLinearBuf;
	cp.dstPitch = _cudaLinearPitch;
	cp.WidthInBytes = _width * 4;
	cp.Height = _height;
	cuMemcpy2D(&cp);

	cuGraphicsUnmapResources(1, &_cuGLResource, 0);

	QueryPerformanceCounter(&pc2);

	NV_ENC_MAP_INPUT_RESOURCE mapRes = {};
	mapRes.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
	mapRes.registeredResource = _nvencRegisteredRes;
	if (_nvenc.nvEncMapInputResource(_encoder, &mapRes) != NV_ENC_SUCCESS)
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

	NVENCSTATUS st = _nvenc.nvEncEncodePicture(_encoder, &picParams);
	_nvenc.nvEncUnmapInputResource(_encoder, mapRes.mappedResource);

	if (st != NV_ENC_SUCCESS && st != NV_ENC_ERR_NEED_MORE_INPUT)
	{
		cuCtxPopCurrent(nullptr);
		return;
	}

	NV_ENC_LOCK_BITSTREAM lockParams = {};
	lockParams.version = NV_ENC_LOCK_BITSTREAM_VER;
	lockParams.outputBitstream = _nvencOutputBuf;

	st = _nvenc.nvEncLockBitstream(_encoder, &lockParams);
	if (st == NV_ENC_SUCCESS)
	{
		QueryPerformanceCounter(&pc3);
		int64_t encodeDoneTimeUs = pc3.QuadPart * 1000000LL / freq.QuadPart;
		int64_t inputTimeUs = ::maplecast::lastInputTimeUs();

		// Build frame header + H.264 payload
		maplecast::PlayerStats p1s, p2s;
		maplecast::getPlayerStats(p1s, p2s);

		uint32_t h264Size = lockParams.bitstreamSizeInBytes;
		uint32_t headerSize = 28 + 8 + 8;  // timing + p1stats + p2stats = 44 bytes
		uint32_t totalPayload = headerSize + h264Size;

		if (totalPayload > _sendBufSize)
		{
			free(_sendBuf);
			_sendBufSize = totalPayload + 65536;
			_sendBuf = (uint8_t*)malloc(_sendBufSize);
		}

		uint32_t frameNum = (uint32_t)_frameCount;
		uint32_t off = 0;
		memcpy(_sendBuf + off, &inputTimeUs, 8); off += 8;
		memcpy(_sendBuf + off, &captureTimeUs, 8); off += 8;
		memcpy(_sendBuf + off, &encodeDoneTimeUs, 8); off += 8;
		memcpy(_sendBuf + off, &frameNum, 4); off += 4;
		// P1 stats
		uint16_t tmp;
		tmp = (uint16_t)p1s.packetsPerSec; memcpy(_sendBuf + off, &tmp, 2); off += 2;
		tmp = (uint16_t)p1s.changesPerSec; memcpy(_sendBuf + off, &tmp, 2); off += 2;
		tmp = p1s.buttons; memcpy(_sendBuf + off, &tmp, 2); off += 2;
		_sendBuf[off++] = p1s.lt; _sendBuf[off++] = p1s.rt;
		// P2 stats
		tmp = (uint16_t)p2s.packetsPerSec; memcpy(_sendBuf + off, &tmp, 2); off += 2;
		tmp = (uint16_t)p2s.changesPerSec; memcpy(_sendBuf + off, &tmp, 2); off += 2;
		tmp = p2s.buttons; memcpy(_sendBuf + off, &tmp, 2); off += 2;
		_sendBuf[off++] = p2s.lt; _sendBuf[off++] = p2s.rt;
		// H.264
		memcpy(_sendBuf + off, lockParams.bitstreamBufferPtr, h264Size);

		// Broadcast to all WebSocket clients
		broadcastBinary(_sendBuf, totalPayload);

		// Telemetry every 300 frames
		if (frameNum % 300 == 0)
		{
			long long mapUs = (pc1.QuadPart - pc0.QuadPart) * 1000000LL / freq.QuadPart;
			long long copyUs = (pc2.QuadPart - pc1.QuadPart) * 1000000LL / freq.QuadPart;
			long long encUs = (pc3.QuadPart - pc2.QuadPart) * 1000000LL / freq.QuadPart;
			long long totalUs = (pc3.QuadPart - pc0.QuadPart) * 1000000LL / freq.QuadPart;
			printf("[maplecast-stream] F:%u | map:%lldus copy:%lldus enc:%lldus total:%lldus | %uB\n",
				frameNum, mapUs, copyUs, encUs, totalUs, h264Size);
			maplecast_telemetry::send("[maplecast-stream] F:%u | map:%lldus copy:%lldus enc:%lldus total:%lldus | %uB",
				frameNum, mapUs, copyUs, encUs, totalUs, h264Size);

			// Periodic status broadcast
			try {
				std::string status = getStatus().dump();
				std::lock_guard<std::mutex> lock(_connMutex);
				for (auto& conn : _connections)
					try { _ws.send(conn, status, websocketpp::frame::opcode::text); } catch (...) {}
			} catch (...) {}
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
