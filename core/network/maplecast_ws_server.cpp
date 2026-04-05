/*
	MapleCast WebSocket Server — lightweight binary broadcast on port 7200.

	Split from maplecast_stream.cpp — this is JUST the WebSocket server.
	No CUDA, no NVENC, no JPEG, no H.264. Just WebSocket send/receive.

	Used by the mirror server to broadcast delta frames to clients.
*/
#include "maplecast_ws_server.h"

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <cstdio>
#include <mutex>
#include <set>
#include <thread>
#include <atomic>

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
	std::lock_guard<std::mutex> lock(_connMutex);
	_connections.insert(hdl);
	_clientCount++;
	printf("[maplecast-ws] client connected (%d total)\n", _clientCount.load());
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
