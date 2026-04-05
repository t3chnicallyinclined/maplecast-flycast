/*
	MapleCast WebSocket Server — lightweight binary broadcast on port 7200.

	Split from maplecast_stream.cpp — this is JUST the WebSocket server.
	No CUDA, no NVENC, no JPEG, no H.264. Just WebSocket send/receive.

	On new client connect: sends full VRAM + PVR regs as initial sync.
	Then delta frames stream normally.
*/
#include "maplecast_ws_server.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <atomic>
#include <vector>

using WsServer = websocketpp::server<websocketpp::config::asio>;
using ConnHdl = websocketpp::connection_hdl;

namespace maplecast_ws
{

static WsServer _ws;
static std::thread _wsThread;
static std::set<ConnHdl, std::owner_less<ConnHdl>> _connections;
static std::mutex _connMutex;
static std::atomic<int> _clientCount{0};
static bool _active = false;

static void onOpen(ConnHdl hdl)
{
	{
		std::lock_guard<std::mutex> lock(_connMutex);
		_connections.insert(hdl);
		_clientCount++;
	}
	printf("[maplecast-ws] client connected (%d total)\n", _clientCount.load());

	// Send initial sync: VRAM (8MB) + PVR regs (32KB)
	// Format: "SYNC" magic (4 bytes) + VRAM_SIZE (4 bytes) + VRAM data + pvr_RegSize (4 bytes) + PVR data
	size_t syncSize = 4 + 4 + VRAM_SIZE + 4 + pvr_RegSize;
	std::vector<uint8_t> syncBuf(syncSize);
	uint8_t* dst = syncBuf.data();

	memcpy(dst, "SYNC", 4); dst += 4;
	uint32_t vs = VRAM_SIZE;
	memcpy(dst, &vs, 4); dst += 4;
	memcpy(dst, &vram[0], VRAM_SIZE); dst += VRAM_SIZE;
	uint32_t ps = pvr_RegSize;
	memcpy(dst, &ps, 4); dst += 4;
	memcpy(dst, pvr_regs, pvr_RegSize);

	try {
		_ws.send(hdl, syncBuf.data(), syncSize, websocketpp::frame::opcode::binary);
		printf("[maplecast-ws] sent initial sync: %.1f MB (VRAM + PVR regs)\n", syncSize / (1024.0 * 1024.0));
	} catch (...) {
		printf("[maplecast-ws] failed to send initial sync\n");
	}
}

static void onClose(ConnHdl hdl)
{
	std::lock_guard<std::mutex> lock(_connMutex);
	_connections.erase(hdl);
	_clientCount--;
	printf("[maplecast-ws] client disconnected (%d total)\n", _clientCount.load());
}

static void onMessage(ConnHdl, WsServer::message_ptr)
{
	// Client messages ignored for now — mirror is server→client only
}

bool init(int port)
{
	try {
		_ws.clear_access_channels(websocketpp::log::alevel::all);
		_ws.clear_error_channels(websocketpp::log::elevel::all);
		_ws.init_asio();
		_ws.set_reuse_addr(true);

		_ws.set_open_handler(&onOpen);
		_ws.set_close_handler(&onClose);
		_ws.set_message_handler(&onMessage);

		_ws.listen(port);
		_ws.start_accept();

		_wsThread = std::thread([&]() { _ws.run(); });

		_active = true;
		printf("[maplecast-ws] WebSocket server on ws://0.0.0.0:%d\n", port);
		return true;
	} catch (const std::exception& e) {
		printf("[maplecast-ws] Init failed: %s\n", e.what());
		return false;
	}
}

void shutdown()
{
	_active = false;
	try { _ws.stop(); } catch (...) {}
	if (_wsThread.joinable()) _wsThread.join();
}

bool active()
{
	return _active;
}

void broadcastBinary(const void* data, size_t size)
{
	if (!_active || _clientCount.load(std::memory_order_relaxed) == 0) return;

	std::lock_guard<std::mutex> lock(_connMutex);
	for (auto& conn : _connections) {
		try {
			_ws.send(conn, data, size, websocketpp::frame::opcode::binary);
		} catch (...) {}
	}
}

}  // namespace maplecast_ws
