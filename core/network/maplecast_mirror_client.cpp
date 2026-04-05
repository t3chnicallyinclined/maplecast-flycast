/*
	MapleCast Mirror Client — connects to server via WebSocket, receives delta frames, renders.

	Spawns a WebSocket client thread that connects to ws://host:port (default localhost:7200).
	Frames queue up; render thread pops the latest and decodes + renders.

	Env vars:
	  MAPLECAST_SERVER_HOST (default: localhost)
	  MAPLECAST_SERVER_PORT (default: 7200)
*/
#include "types.h"
#include "maplecast_mirror.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/aica/aica_if.h"
#include "rend/TexCache.h"
#include "hw/mem/mem_watch.h"
#include "rend/texconv.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <deque>

#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>
using WsClient = websocketpp::client<websocketpp::config::asio_client>;

extern Renderer* renderer;
extern bool pal_needs_update;

namespace maplecast_mirror
{

static const size_t MEM_PAGE_SIZE = 4096;
static bool _isClient = false;

static std::mutex _frameMutex;
static std::deque<std::vector<uint8_t>> _frameQueue;
static std::atomic<uint32_t> _framesReceived{0};
static std::atomic<uint32_t> _framesRendered{0};
static std::vector<uint8_t> _clientTA;
static bool _clientHasFullFrame = false;

static WsClient _wsClient;
static std::thread _wsThread;

static void wsClientRun(std::string url)
{
	try {
		printf("[MIRROR-WS] Thread started, connecting to %s\n", url.c_str());
		_wsClient.clear_access_channels(websocketpp::log::alevel::all);
		_wsClient.clear_error_channels(websocketpp::log::elevel::all);
		_wsClient.init_asio();
		_wsClient.set_open_handler([](websocketpp::connection_hdl) {
			printf("[MIRROR-WS] Connected to server\n");
			fflush(stdout);
		});
		_wsClient.set_close_handler([](websocketpp::connection_hdl) {
			printf("[MIRROR-WS] Disconnected\n");
			fflush(stdout);
		});
		_wsClient.set_fail_handler([](websocketpp::connection_hdl hdl) {
			auto con = _wsClient.get_con_from_hdl(hdl);
			printf("[MIRROR-WS] Connection FAILED: %s (HTTP %d)\n",
				con->get_ec().message().c_str(), con->get_response_code());
			fflush(stdout);
		});
		_wsClient.set_message_handler([](websocketpp::connection_hdl, WsClient::message_ptr msg) {
			if (msg->get_opcode() != websocketpp::frame::opcode::binary) return;
			const std::string& payload = msg->get_payload();
			if (payload.size() < 80) return;

			std::vector<uint8_t> frame(payload.begin(), payload.end());
			{
				std::lock_guard<std::mutex> lock(_frameMutex);
				while (_frameQueue.size() >= 2)
					_frameQueue.pop_front();
				_frameQueue.push_back(std::move(frame));
			}
			uint32_t n = _framesReceived.fetch_add(1, std::memory_order_relaxed);
			if (n == 0) { printf("[MIRROR-WS] First frame received (%zu bytes)\n", payload.size()); fflush(stdout); }
		});

		websocketpp::lib::error_code ec;
		printf("[MIRROR-WS] Creating connection...\n"); fflush(stdout);
		auto con = _wsClient.get_connection(url, ec);
		if (ec) { printf("[MIRROR-WS] get_connection error: %s\n", ec.message().c_str()); fflush(stdout); return; }
		printf("[MIRROR-WS] Connecting...\n"); fflush(stdout);
		_wsClient.connect(con);
		_wsClient.run();
	} catch (const std::exception& e) {
		printf("[MIRROR-WS] Exception: %s\n", e.what());
	}
}

void initClient()
{
	_isClient = true;

	const char* host = std::getenv("MAPLECAST_SERVER_HOST");
	if (!host) host = "127.0.0.1";
	const char* port = std::getenv("MAPLECAST_SERVER_PORT");
	if (!port) port = "7200";

	std::string url = std::string("ws://") + host + ":" + port;
	printf("[MIRROR] === CLIENT MODE === WebSocket to %s\n", url.c_str());

	_wsThread = std::thread(wsClientRun, url);
	_wsThread.detach();
}

bool isClient() { return _isClient; }

// Server stubs — client build doesn't use these
void initServer() {}
bool isServer() { return false; }
void serverPublish(TA_context*) {}

bool clientReceive(rend_context& rc, bool& vramDirty)
{
	vramDirty = false;
	if (!_isClient) return false;

	std::vector<uint8_t> frame;
	{
		std::lock_guard<std::mutex> lock(_frameMutex);
		if (_frameQueue.empty()) return false;
		frame = std::move(_frameQueue.back());
		_frameQueue.clear();
	}

	uint8_t* src = frame.data();

	uint32_t frameSize; memcpy(&frameSize, src, 4); src += 4;
	uint32_t frameNum; memcpy(&frameNum, src, 4); src += 4;

	uint32_t pvr_snapshot[16];
	memcpy(pvr_snapshot, src, sizeof(pvr_snapshot)); src += sizeof(pvr_snapshot);

	uint32_t taSize; memcpy(&taSize, src, 4); src += 4;
	uint32_t deltaPayloadSize; memcpy(&deltaPayloadSize, src, 4); src += 4;

	if (deltaPayloadSize == taSize)
	{
		_clientTA.assign(src, src + taSize);
		src += taSize;
		_clientHasFullFrame = true;
	}
	else if (!_clientHasFullFrame)
	{
		src += deltaPayloadSize;
		src += 4;
		return false;
	}
	else
	{
		if (_clientTA.size() < taSize) _clientTA.resize(taSize, 0);
		else if (_clientTA.size() > taSize) _clientTA.resize(taSize);

		uint8_t* dd = src;
		uint8_t* de = src + deltaPayloadSize;
		while (dd + 4 <= de) {
			uint32_t off; memcpy(&off, dd, 4); dd += 4;
			if (off == 0xFFFFFFFF) break;
			uint16_t runLen; memcpy(&runLen, dd, 2); dd += 2;
			if (off + runLen <= taSize && dd + runLen <= de)
				memcpy(_clientTA.data() + off, dd, runLen);
			dd += runLen;
		}
		src += deltaPayloadSize;
	}

	uint32_t serverChecksum; memcpy(&serverChecksum, src, 4); src += 4;
	uint8_t* taData = _clientTA.data();

	uint32_t clientChecksum = 0;
	for (uint32_t i = 0; i + 3 < taSize; i += 4)
		clientChecksum ^= *(uint32_t*)(taData + i);
	static uint32_t checksumFails = 0;
	if (clientChecksum != serverChecksum) {
		checksumFails++;
		if (checksumFails <= 10) printf("[DELTA] CHECKSUM MISMATCH frame %u\n", frameNum);
	}

	// Memory diffs
	uint32_t dirtyPages; memcpy(&dirtyPages, src, 4); src += 4;
	for (uint32_t d = 0; d < dirtyPages; d++) {
		uint8_t regionId = *src++;
		uint32_t pageIdx; memcpy(&pageIdx, src, 4); src += 4;
		size_t pageOff = pageIdx * MEM_PAGE_SIZE;

		if (regionId == 0 && pageOff + MEM_PAGE_SIZE <= 16 * 1024 * 1024)
			memcpy(&mem_b[pageOff], src, MEM_PAGE_SIZE);
		else if (regionId == 1 && pageOff + MEM_PAGE_SIZE <= VRAM_SIZE) {
			// Unprotect BEFORE writing — texture cache may have mprotect'd this page
			VramLockedWriteOffset(pageOff);
			memcpy(&vram[pageOff], src, MEM_PAGE_SIZE);
			vramDirty = true;
		}
		else if (regionId == 2 && pageOff + MEM_PAGE_SIZE <= 2 * 1024 * 1024)
			memcpy(&aica::aica_ram[pageOff], src, MEM_PAGE_SIZE);
		else if (regionId == 3 && pageOff + MEM_PAGE_SIZE <= (size_t)pvr_RegSize)
			memcpy(pvr_regs + pageOff, src, MEM_PAGE_SIZE);
		src += MEM_PAGE_SIZE;
	}

	// Build TA context and render
	if (taSize > 0) {
		static TA_context clientCtx;
		static bool ctxAlloced = false;
		if (!ctxAlloced) { clientCtx.Alloc(); ctxAlloced = true; }

		clientCtx.rend.Clear();
		clientCtx.tad.Clear();
		memcpy(clientCtx.tad.thd_root, taData, taSize);
		clientCtx.tad.thd_data = clientCtx.tad.thd_root + taSize;

		TA_GLOB_TILE_CLIP.full = pvr_snapshot[0];
		SCALER_CTL.full = pvr_snapshot[1];
		FB_X_CLIP.full = pvr_snapshot[2];
		FB_Y_CLIP.full = pvr_snapshot[3];
		FB_W_LINESTRIDE.full = pvr_snapshot[4];
		FB_W_SOF1 = pvr_snapshot[5];
		FB_W_CTRL.full = pvr_snapshot[6];
		FOG_CLAMP_MIN.full = pvr_snapshot[7];
		FOG_CLAMP_MAX.full = pvr_snapshot[8];

		clientCtx.rend.isRTT = pvr_snapshot[13] != 0;
		clientCtx.rend.fb_W_SOF1 = pvr_snapshot[5];
		clientCtx.rend.fb_W_CTRL.full = pvr_snapshot[6];
		clientCtx.rend.ta_GLOB_TILE_CLIP.full = pvr_snapshot[0];
		clientCtx.rend.scaler_ctl.full = pvr_snapshot[1];
		clientCtx.rend.fb_X_CLIP.full = pvr_snapshot[2];
		clientCtx.rend.fb_Y_CLIP.full = pvr_snapshot[3];
		clientCtx.rend.fb_W_LINESTRIDE = pvr_snapshot[4];
		clientCtx.rend.fog_clamp_min.full = pvr_snapshot[7];
		clientCtx.rend.fog_clamp_max.full = pvr_snapshot[8];
		clientCtx.rend.framebufferWidth = pvr_snapshot[9];
		clientCtx.rend.framebufferHeight = pvr_snapshot[10];
		clientCtx.rend.clearFramebuffer = pvr_snapshot[11] != 0;
		float fz; memcpy(&fz, &pvr_snapshot[12], 4);
		clientCtx.rend.fZ_max = fz;

		if (vramDirty) renderer->resetTextureCache = true;

		::pal_needs_update = true;
		palette_update();
		renderer->updatePalette = true;
		renderer->updateFogTable = true;

		renderer->Process(&clientCtx);
		rc = clientCtx.rend;
	}

	_framesRendered.fetch_add(1, std::memory_order_relaxed);

	if (frameNum % 600 == 0)
		printf("[MIRROR] Client frame %u | rendered %u / received %u | dirty=%u\n",
			frameNum, _framesRendered.load(), _framesReceived.load(), dirtyPages);

	return taSize > 0;
}

}  // namespace maplecast_mirror
