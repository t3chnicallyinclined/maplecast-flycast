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
#else
#include <dlfcn.h>
#include <time.h>
#endif

#include "maplecast_stream.h"
#include "maplecast.h"
#include "maplecast_telemetry.h"
#include "maplecast_input_server.h"
#include "maplecast_webrtc.h"
#include "hw/pvr/Renderer_if.h"
#include "wsi/gl_context.h"

// CUDA driver API + GL interop
#include <cuda.h>
#include <cudaGL.h>
#include <cuda_runtime.h>

// NVENC direct API
#include <ffnvcodec/nvEncodeAPI.h>

// nvJPEG — CUDA-accelerated JPEG encode (alternative to H.264)
#include <nvjpeg.h>

// Embedded CUDA PTX kernels (RGBA→RGB strip for nvJPEG)
#include "rgba_to_nv12_ptx.h"

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

// Cross-platform microsecond timer
static inline int64_t nowUs()
{
#ifdef _WIN32
	static LARGE_INTEGER freq = {};
	if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
	LARGE_INTEGER pc;
	QueryPerformanceCounter(&pc);
	return pc.QuadPart * 1000000LL / freq.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
#endif
}

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

// nvJPEG encoder (MAPLECAST_JPEG=1 to enable)
static nvjpegHandle_t _jpegHandle = nullptr;
static nvjpegEncoderState_t _jpegState = nullptr;
static nvjpegEncoderParams_t _jpegParams = nullptr;
static cudaStream_t _jpegStream = nullptr;
static bool _useJpeg = false;
static CUdeviceptr _cudaRGBBuf = 0;
static size_t _cudaRGBPitch = 0;
static CUmodule _cudaModule = nullptr;
static CUfunction _kernelRGBAtoRGB = nullptr;

// NVENC — double-buffered for async pipeline
typedef NVENCSTATUS (NVENCAPI *PFN_NvEncodeAPICreateInstance)(NV_ENCODE_API_FUNCTION_LIST*);
static NV_ENCODE_API_FUNCTION_LIST _nvenc = {};
static void* _encoder = nullptr;

// Double buffer: encode frame N while frame N+1 renders
static constexpr int NUM_BUFS = 2;
static CUdeviceptr _cudaLinearBuf[NUM_BUFS] = {0, 0};
static size_t _cudaLinearPitch = 0;
static NV_ENC_REGISTERED_PTR _nvencRegisteredRes[NUM_BUFS] = {nullptr, nullptr};
static NV_ENC_OUTPUT_PTR _nvencOutputBuf[NUM_BUFS] = {nullptr, nullptr};
static int _curBuf = 0;          // which buffer to write into THIS frame
static bool _prevEncodeActive = false;  // is the previous frame still encoding?
static int64_t _prevCaptureTimeUs = 0;
static int64_t _prevInputTimeUs = 0;
static uint32_t _prevFrameNum = 0;

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
	std::string device;  // gamepad name or "NOBD Stick"
	int slot;            // 0=P1, 1=P2, -1=spectator
	ConnHdl conn;
};
static std::map<std::string, WsPlayer> _players;  // id → player
static std::string _slotOwner[2] = {"", ""};       // slot → player_id

// Reverse lookup: connection → assigned slot (-1 = spectator)
static std::map<void*, int> _connSlot;

static int getSlotForConn(ConnHdl hdl)
{
	try {
		void* key = (void*)_ws.get_con_from_hdl(hdl).get();
		auto it = _connSlot.find(key);
		return it != _connSlot.end() ? it->second : -1;
	} catch (...) { return -1; }
}

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
	auto slotInfo = [](int i) -> json {
		const auto& p = maplecast_input::getPlayer(i);
		if (!p.connected)
			return nullptr;
		const char* typeStr = (p.type == maplecast_input::InputType::NobdUDP) ? "hardware" : "browser";
		return {
			{"id", std::string(p.id).substr(0, 8)},
			{"name", std::string(p.name)},
			{"device", std::string(p.device)},
			{"connected", true},
			{"type", typeStr},
			{"pps", p.packetsPerSec},
			{"cps", p.changesPerSec}
		};
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
				// Remove connection → slot mapping
				try {
					void* key = (void*)_ws.get_con_from_hdl(hdl).get();
					_connSlot.erase(key);
				} catch (...) {}
#ifdef MAPLECAST_WEBRTC
				maplecast_webrtc::removePeer(id);
#endif
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
			// Forward to Flycast via UDP with player slot tag
			// 5-byte tagged packet: [slot(1)][LT][RT][btn_hi][btn_lo]
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

			// Look up this connection's assigned slot
			int slot = getSlotForConn(hdl);

			if (slot >= 0 && slot <= 1)
			{
				// Tagged 5-byte packet: slot + W3 data
				char tagged[5];
				tagged[0] = (char)slot;
				memcpy(tagged + 1, data.c_str(), 4);
				sendto(udpSock, tagged, 5, 0, (struct sockaddr*)&udpDest, sizeof(udpDest));
			}
			else
			{
				// Unassigned player — send raw 4-byte (legacy auto-assign)
				sendto(udpSock, data.c_str(), 4, 0, (struct sockaddr*)&udpDest, sizeof(udpDest));
			}
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
				std::string device = ctrl.value("device", "Browser");

				// Register through unified input server
				int slot = maplecast_input::registerPlayer(
					playerId.c_str(), name.c_str(), device.c_str(),
					maplecast_input::InputType::BrowserWS);
				_players[playerId] = {playerId, name, device, slot, hdl};

				// Register connection → slot mapping for input routing
				if (slot >= 0) {
					try {
						void* key = (void*)_ws.get_con_from_hdl(hdl).get();
						_connSlot[key] = slot;
					} catch (...) {}
				}

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
#ifdef MAPLECAST_WEBRTC
				// WebRTC signaling: SDP offer from browser
				else if (ctrl["type"] == "offer")
				{
					std::string playerId = ctrl.value("id", "");
					std::string sdp = ctrl.value("sdp", "");
					if (!playerId.empty() && !sdp.empty())
					{
						int slot = -1;
						auto it = _players.find(playerId);
						if (it != _players.end()) slot = it->second.slot;
						// Register connection for signaling even if not "joined"
						if (it == _players.end())
							_players[playerId] = {playerId, "spectator", "Browser", -1, hdl};
						else
							it->second.conn = hdl;
						maplecast_webrtc::handleOffer(playerId, sdp, slot);
					}
				}
				// WebRTC signaling: ICE candidate from browser
				else if (ctrl["type"] == "ice-candidate")
				{
					std::string playerId = ctrl.value("id", "");
					std::string candidate = ctrl.value("candidate", "");
					std::string sdpMid = ctrl.value("sdpMid", "");
					if (!playerId.empty())
						maplecast_webrtc::handleIceCandidate(playerId, candidate, sdpMid);
				}
#endif
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

	// Load CUDA PTX kernels (RGBA→RGB for nvJPEG)
	CUresult kr = cuModuleLoadData(&_cudaModule, _ptxRGBAtoNV12);
	if (kr != CUDA_SUCCESS)
		printf("[maplecast-stream] PTX load failed: %d (JPEG mode unavailable)\n", kr);
	else
	{
		cuModuleGetFunction(&_kernelRGBAtoRGB, _cudaModule, "rgba_to_rgb");
		printf("[maplecast-stream] CUDA kernels loaded (RGBA→RGB ready)\n");
	}

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
	encConfig.rcParams.averageBitRate = 30000000;   // 30 Mbps — crisp 480p IDR
	encConfig.rcParams.maxBitRate = 40000000;      // 40 Mbps peak
	encConfig.rcParams.vbvBufferSize = 30000000 / 60;  // 1 frame buffer at 30Mbps
	encConfig.rcParams.vbvInitialDelay = encConfig.rcParams.vbvBufferSize;
	encConfig.rcParams.zeroReorderDelay = 1;
	encConfig.encodeCodecConfig.h264Config.idrPeriod = 1;
	encConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
	encConfig.encodeCodecConfig.h264Config.chromaFormatIDC = 1;
	encConfig.encodeCodecConfig.h264Config.disableDeblockingFilterIDC = 0;  // enable deblock — better quality
	encConfig.encodeCodecConfig.h264Config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;  // CABAC — sharper at same bitrate
	encConfig.encodeCodecConfig.h264Config.sliceMode = 0;       // single slice
	encConfig.encodeCodecConfig.h264Config.sliceModeData = 0;
	encConfig.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;  // single pass only
	encConfig.rcParams.enableLookahead = 0;                     // no lookahead
	encConfig.rcParams.lowDelayKeyFrameScale = 1;

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

	// Create double output bitstream buffers for async encode
	for (int i = 0; i < NUM_BUFS; i++)
	{
		NV_ENC_CREATE_BITSTREAM_BUFFER bitstreamParams = {};
		bitstreamParams.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
		if (_nvenc.nvEncCreateBitstreamBuffer(_encoder, &bitstreamParams) != NV_ENC_SUCCESS) return false;
		_nvencOutputBuf[i] = bitstreamParams.bitstreamBuffer;
	}

	printf("[maplecast-stream] NVENC: %dx%d H.264 P1/ULL GOP=1 8Mbps (double-buffered)\n", _width, _height);
	return true;
}

static bool initCudaLinearBuffer()
{
	// Allocate double CUDA buffers + register both with NVENC
	for (int i = 0; i < NUM_BUFS; i++)
	{
		size_t pitch;
		if (cuMemAllocPitch(&_cudaLinearBuf[i], &pitch, _width * 4, _height, 16) != CUDA_SUCCESS) return false;
		if (i == 0) _cudaLinearPitch = pitch;

		NV_ENC_REGISTER_RESOURCE regRes = {};
		regRes.version = NV_ENC_REGISTER_RESOURCE_VER;
		regRes.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
		regRes.resourceToRegister = (void*)_cudaLinearBuf[i];
		regRes.width = _width;
		regRes.height = _height;
		regRes.pitch = (uint32_t)_cudaLinearPitch;
		regRes.bufferFormat = NV_ENC_BUFFER_FORMAT_ABGR;
		regRes.bufferUsage = NV_ENC_INPUT_IMAGE;
		if (_nvenc.nvEncRegisterResource(_encoder, &regRes) != NV_ENC_SUCCESS) return false;
		_nvencRegisteredRes[i] = regRes.registeredResource;
	}

	printf("[maplecast-stream] CUDA double buffer: %dx%d pitch=%zu\n", _width, _height, _cudaLinearPitch);
	return true;
}

static void broadcastBinary(const void* data, size_t size)
{
#ifdef MAPLECAST_WEBRTC
	// Send via DataChannel to peers that have it (P2P, no TCP overhead)
	maplecast_webrtc::broadcastFrame(data, size);
#endif

	// Send via WebSocket to peers that don't have DataChannel (fallback)
	std::lock_guard<std::mutex> lock(_connMutex);
	for (auto& conn : _connections)
	{
		// Skip peers with active DataChannel — they already got the frame
#ifdef MAPLECAST_WEBRTC
		bool hasDc = false;
		for (auto& [id, p] : _players)
		{
			try {
				if (!(p.conn.owner_before(conn) || conn.owner_before(p.conn)))
				{
					hasDc = maplecast_webrtc::peerHasDataChannel(id);
					break;
				}
			} catch (...) {}
		}
		if (hasDc) continue;
#endif
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

	// JPEG mode: MAPLECAST_JPEG=1 (or MAPLECAST_JPEG=85 for quality 85)
	const char* jpegEnv = std::getenv("MAPLECAST_JPEG");
	if (jpegEnv)
	{
		int quality = std::atoi(jpegEnv);
		if (quality < 1 || quality > 100) quality = 75;

		nvjpegStatus_t js = nvjpegCreateSimple(&_jpegHandle);
		if (js == NVJPEG_STATUS_SUCCESS)
		{
			nvjpegEncoderStateCreate(_jpegHandle, &_jpegState, nullptr);
			nvjpegEncoderParamsCreate(_jpegHandle, &_jpegParams, nullptr);
			nvjpegEncoderParamsSetQuality(_jpegParams, quality, nullptr);
			nvjpegEncoderParamsSetSamplingFactors(_jpegParams, NVJPEG_CSS_420, nullptr);
			cudaStreamCreate(&_jpegStream);

			size_t rgbPitch;
			cuMemAllocPitch(&_cudaRGBBuf, &rgbPitch, _width * 3, _height, 16);
			_cudaRGBPitch = rgbPitch;

			_useJpeg = true;
			printf("[maplecast-stream] *** JPEG MODE *** quality=%d, RGB pitch=%zu\n", quality, _cudaRGBPitch);
		}
		else
		{
			printf("[maplecast-stream] nvJPEG init failed (%d) — using H.264\n", js);
		}
	}

	_active = true;
	_frameCount = 0;

	// Init WebRTC DataChannel transport (P2P, NAT hole-punching)
#ifdef MAPLECAST_WEBRTC
	maplecast_webrtc::init([](const std::string& playerId, const std::string& type, const std::string& data) {
		// Signaling callback: send SDP/ICE to browser via WebSocket
		std::lock_guard<std::mutex> lock(_connMutex);
		auto it = _players.find(playerId);
		if (it == _players.end()) return;

		json msg;
		if (type == "answer") {
			msg = {{"type", "answer"}, {"sdp", data}};
		} else if (type == "ice-candidate") {
			// data format: "candidate|sdpMid"
			auto sep = data.find('|');
			msg = {
				{"type", "ice-candidate"},
				{"candidate", data.substr(0, sep)},
				{"sdpMid", sep != std::string::npos ? data.substr(sep + 1) : "0"}
			};
		}

		try { _ws.send(it->second.conn, msg.dump(), websocketpp::frame::opcode::text); }
		catch (...) {}
	});
#endif

	printf("[maplecast-stream] ZERO-COPY + DIRECT WebSocket ready\n");
	maplecast_telemetry::send("[maplecast-stream] ZERO-COPY + WS ready");
	return true;
}

void shutdown()
{
	_active = false;
#ifdef MAPLECAST_WEBRTC
	maplecast_webrtc::shutdown();
#endif
	try { _ws.stop(); } catch (...) {}
	if (_wsThread.joinable()) _wsThread.join();

	for (int i = 0; i < NUM_BUFS; i++)
	{
		if (_nvencRegisteredRes[i] && _encoder)
			_nvenc.nvEncUnregisterResource(_encoder, _nvencRegisteredRes[i]);
		if (_nvencOutputBuf[i] && _encoder)
			_nvenc.nvEncDestroyBitstreamBuffer(_encoder, _nvencOutputBuf[i]);
		if (_cudaLinearBuf[i]) cuMemFree(_cudaLinearBuf[i]);
	}
	if (_encoder) _nvenc.nvEncDestroyEncoder(_encoder);
	if (_cuGLResource) cuGraphicsUnregisterResource(_cuGLResource);
	if (_cuCtx) cuCtxDestroy(_cuCtx);

	free(_sendBuf); _sendBuf = nullptr;
	printf("[maplecast-stream] shutdown\n");
}

void onFrameRendered()
{
	if (!_active || !_encoder) return;
	if (_clientCount.load(std::memory_order_relaxed) == 0) return;

	int64_t pc0, pc1, pc2, pc3;
	pc0 = nowUs();
	int64_t captureTimeUs = pc0;
	int64_t inputTimeUs = ::maplecast::lastInputTimeUs();

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
		{ cuCtxPopCurrent(nullptr); return; }
		_registeredTexID = texID;
		printf("[maplecast-stream] GL tex %u registered (%dx%d)\n", texID, w, h);
		cuCtxPopCurrent(nullptr);
	}

	pc1 = nowUs();

	// Capture: GL texture → CUDA array → linear buffer (GPU→GPU)
	cuCtxPushCurrent(_cuCtx);

	if (cuGraphicsMapResources(1, &_cuGLResource, 0) != CUDA_SUCCESS)
	{ cuCtxPopCurrent(nullptr); return; }

	CUarray cuArray;
	if (cuGraphicsSubResourceGetMappedArray(&cuArray, _cuGLResource, 0, 0) != CUDA_SUCCESS)
	{ cuGraphicsUnmapResources(1, &_cuGLResource, 0); cuCtxPopCurrent(nullptr); return; }

	CUDA_MEMCPY2D cp = {};
	cp.srcMemoryType = CU_MEMORYTYPE_ARRAY;
	cp.srcArray = cuArray;
	cp.dstMemoryType = CU_MEMORYTYPE_DEVICE;
	cp.dstDevice = _cudaLinearBuf[0];
	cp.dstPitch = _cudaLinearPitch;
	cp.WidthInBytes = _width * 4;
	cp.Height = _height;
	cuMemcpy2D(&cp);
	cuGraphicsUnmapResources(1, &_cuGLResource, 0);

	pc2 = nowUs();

	// JPEG path: RGBA → RGB strip (CUDA kernel) → nvJPEG encode
	if (_useJpeg)
	{
		// Step 1: strip alpha channel on GPU
		if (_kernelRGBAtoRGB && _cudaRGBBuf)
		{
			int rgbaPitch = (int)_cudaLinearPitch;
			int rgbPitch = (int)_cudaRGBPitch;
			void* args[] = {
				(void*)&_cudaLinearBuf[0], (void*)&rgbaPitch,
				(void*)&_cudaRGBBuf, (void*)&rgbPitch,
				(void*)&_width, (void*)&_height
			};
			cuLaunchKernel(_kernelRGBAtoRGB,
				(_width + 15) / 16, (_height + 15) / 16, 1,
				16, 16, 1, 0, 0, args, nullptr);
		}

		// Step 2: nvJPEG encode from RGB buffer
		nvjpegImage_t imgDesc;
		memset(&imgDesc, 0, sizeof(imgDesc));
		imgDesc.channel[0] = (unsigned char*)_cudaRGBBuf;
		imgDesc.pitch[0] = _cudaRGBPitch;

		nvjpegStatus_t js = nvjpegEncodeImage(_jpegHandle, _jpegState, _jpegParams,
			&imgDesc, NVJPEG_INPUT_RGBI, _width, _height, _jpegStream);
		cudaStreamSynchronize(_jpegStream);

		if (js == NVJPEG_STATUS_SUCCESS)
		{
			size_t jpegSize = 0;
			nvjpegEncodeRetrieveBitstream(_jpegHandle, _jpegState, NULL, &jpegSize, _jpegStream);
			cudaStreamSynchronize(_jpegStream);

			pc3 = nowUs();
			uint32_t pipelineUs = (uint32_t)(pc3 - pc0);
			uint32_t copyUs = (uint32_t)(pc2 - pc1);
			uint32_t encodeUs = (uint32_t)(pc3 - pc2);

			const auto& p1 = maplecast_input::getPlayer(0);
			const auto& p2 = maplecast_input::getPlayer(1);

			uint32_t headerSize = 16 + 8 + 8;
			uint32_t totalPayload = headerSize + (uint32_t)jpegSize;

			if (totalPayload > _sendBufSize)
			{ free(_sendBuf); _sendBufSize = totalPayload + 65536; _sendBuf = (uint8_t*)malloc(_sendBufSize); }

			uint32_t frameNum = (uint32_t)_frameCount;
			uint32_t off = 0;
			memcpy(_sendBuf + off, &pipelineUs, 4); off += 4;
			memcpy(_sendBuf + off, &copyUs, 4); off += 4;
			memcpy(_sendBuf + off, &encodeUs, 4); off += 4;
			memcpy(_sendBuf + off, &frameNum, 4); off += 4;
			uint16_t tmp;
			tmp = (uint16_t)p1.packetsPerSec; memcpy(_sendBuf + off, &tmp, 2); off += 2;
			tmp = (uint16_t)p1.changesPerSec; memcpy(_sendBuf + off, &tmp, 2); off += 2;
			tmp = p1.buttons; memcpy(_sendBuf + off, &tmp, 2); off += 2;
			_sendBuf[off++] = p1.lt; _sendBuf[off++] = p1.rt;
			tmp = (uint16_t)p2.packetsPerSec; memcpy(_sendBuf + off, &tmp, 2); off += 2;
			tmp = (uint16_t)p2.changesPerSec; memcpy(_sendBuf + off, &tmp, 2); off += 2;
			tmp = p2.buttons; memcpy(_sendBuf + off, &tmp, 2); off += 2;
			_sendBuf[off++] = p2.lt; _sendBuf[off++] = p2.rt;

			nvjpegEncodeRetrieveBitstream(_jpegHandle, _jpegState, _sendBuf + off, &jpegSize, _jpegStream);
			cudaStreamSynchronize(_jpegStream);

			broadcastBinary(_sendBuf, totalPayload);

			if (frameNum % 300 == 0)
			{
				printf("[maplecast-stream] F:%u JPEG | copy:%uus enc:%uus total:%uus | %zuB\n",
					frameNum, copyUs, encodeUs, pipelineUs, jpegSize);
				maplecast_telemetry::send("[maplecast-stream] F:%u JPEG | copy:%uus enc:%uus total:%uus | %zuB",
					frameNum, copyUs, encodeUs, pipelineUs, jpegSize);
			}
		}

		cuCtxPopCurrent(nullptr);
		_frameCount++;
		return;
	}

	// Encode: NVENC from CUDA device memory → H.264 bitstream
	NV_ENC_MAP_INPUT_RESOURCE mapRes = {};
	mapRes.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
	mapRes.registeredResource = _nvencRegisteredRes[0];
	if (_nvenc.nvEncMapInputResource(_encoder, &mapRes) != NV_ENC_SUCCESS)
	{ cuCtxPopCurrent(nullptr); return; }

	NV_ENC_PIC_PARAMS picParams = {};
	picParams.version = NV_ENC_PIC_PARAMS_VER;
	picParams.inputBuffer = mapRes.mappedResource;
	picParams.bufferFmt = mapRes.mappedBufferFmt;
	picParams.inputWidth = _width;
	picParams.inputHeight = _height;
	picParams.outputBitstream = _nvencOutputBuf[0];
	picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
	picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
	picParams.inputTimeStamp = _frameCount;

	NVENCSTATUS st = _nvenc.nvEncEncodePicture(_encoder, &picParams);
	_nvenc.nvEncUnmapInputResource(_encoder, mapRes.mappedResource);

	if (st != NV_ENC_SUCCESS && st != NV_ENC_ERR_NEED_MORE_INPUT)
	{ cuCtxPopCurrent(nullptr); return; }

	// Lock bitstream and send
	NV_ENC_LOCK_BITSTREAM lockParams = {};
	lockParams.version = NV_ENC_LOCK_BITSTREAM_VER;
	lockParams.outputBitstream = _nvencOutputBuf[0];

	st = _nvenc.nvEncLockBitstream(_encoder, &lockParams);
	if (st == NV_ENC_SUCCESS)
	{
		pc3 = nowUs();

		// Pre-compute pipeline times (microseconds, fits in uint32)
		uint32_t pipelineUs = (uint32_t)(pc3 - pc0);
		uint32_t copyUs = (uint32_t)(pc2 - pc1);
		uint32_t encodeUs = (uint32_t)(pc3 - pc2);

		const auto& p1 = maplecast_input::getPlayer(0);
		const auto& p2 = maplecast_input::getPlayer(1);

		uint32_t h264Size = lockParams.bitstreamSizeInBytes;
		// Header: [pipelineUs(4)][copyUs(4)][encodeUs(4)][frameNum(4)] + p1stats(8) + p2stats(8) = 32 bytes
		uint32_t headerSize = 16 + 8 + 8;  // 32 bytes
		uint32_t totalPayload = headerSize + h264Size;

		if (totalPayload > _sendBufSize)
		{ free(_sendBuf); _sendBufSize = totalPayload + 65536; _sendBuf = (uint8_t*)malloc(_sendBufSize); }

		uint32_t frameNum = (uint32_t)_frameCount;
		uint32_t off = 0;
		memcpy(_sendBuf + off, &pipelineUs, 4); off += 4;
		memcpy(_sendBuf + off, &copyUs, 4); off += 4;
		memcpy(_sendBuf + off, &encodeUs, 4); off += 4;
		memcpy(_sendBuf + off, &frameNum, 4); off += 4;
		uint16_t tmp;
		tmp = (uint16_t)p1.packetsPerSec; memcpy(_sendBuf + off, &tmp, 2); off += 2;
		tmp = (uint16_t)p1.changesPerSec; memcpy(_sendBuf + off, &tmp, 2); off += 2;
		tmp = p1.buttons; memcpy(_sendBuf + off, &tmp, 2); off += 2;
		_sendBuf[off++] = p1.lt; _sendBuf[off++] = p1.rt;
		tmp = (uint16_t)p2.packetsPerSec; memcpy(_sendBuf + off, &tmp, 2); off += 2;
		tmp = (uint16_t)p2.changesPerSec; memcpy(_sendBuf + off, &tmp, 2); off += 2;
		tmp = p2.buttons; memcpy(_sendBuf + off, &tmp, 2); off += 2;
		_sendBuf[off++] = p2.lt; _sendBuf[off++] = p2.rt;
		memcpy(_sendBuf + off, lockParams.bitstreamBufferPtr, h264Size);

		broadcastBinary(_sendBuf, totalPayload);

		if (frameNum % 300 == 0)
		{
			long long mapUs = pc1 - pc0;
			long long copyUs = pc2 - pc1;
			long long encUs = pc3 - pc2;
			long long totalUs = pc3 - pc0;
			printf("[maplecast-stream] F:%u | map:%lldus copy:%lldus enc:%lldus total:%lldus | %uB\n",
				frameNum, mapUs, copyUs, encUs, totalUs, h264Size);
			maplecast_telemetry::send("[maplecast-stream] F:%u | map:%lldus copy:%lldus enc:%lldus total:%lldus | %uB",
				frameNum, mapUs, copyUs, encUs, totalUs, h264Size);

			try {
				std::string status = getStatus().dump();
				std::lock_guard<std::mutex> lock(_connMutex);
				for (auto& conn : _connections)
					try { _ws.send(conn, status, websocketpp::frame::opcode::text); } catch (...) {}
			} catch (...) {}
		}

		_nvenc.nvEncUnlockBitstream(_encoder, _nvencOutputBuf[0]);
	}

	cuCtxPopCurrent(nullptr);
	_frameCount++;
}

bool active()
{
	return _active;
}

}  // namespace maplecast_stream
