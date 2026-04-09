/*
	MapleCast Audio WebSocket Server — see maplecast_audio_ws.h for rationale.

	Isolated from maplecast_ws on purpose: separate listener, separate asio
	io_service, separate worker thread, separate TCP sockets per client.
	Nothing about this server can block the TA mirror path.
*/
#include "maplecast_audio_ws.h"

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <cstdio>
#include <mutex>
#include <set>
#include <thread>
#include <atomic>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#endif

using WsServer = websocketpp::server<websocketpp::config::asio>;
using ConnHdl  = websocketpp::connection_hdl;

namespace maplecast_audio_ws
{

static WsServer _ws;
static std::thread _wsThread;
static std::set<ConnHdl, std::owner_less<ConnHdl>> _connections;
static std::mutex _connMutex;
static std::atomic<bool> _active{false};
static std::atomic<int>  _clientCount{0};

static void onOpen(ConnHdl hdl)
{
	{
		std::lock_guard<std::mutex> lock(_connMutex);
		_connections.insert(hdl);
	}
	_clientCount.fetch_add(1, std::memory_order_relaxed);
	printf("[maplecast-audio-ws] client connected (total=%d)\n",
		_clientCount.load(std::memory_order_relaxed));
}

static void onClose(ConnHdl hdl)
{
	{
		std::lock_guard<std::mutex> lock(_connMutex);
		_connections.erase(hdl);
	}
	const int n = _clientCount.fetch_sub(1, std::memory_order_relaxed) - 1;
	printf("[maplecast-audio-ws] client disconnected (total=%d)\n", n);
}

// Audio server accepts no messages from clients. Incoming frames are ignored.
static void onMessage(ConnHdl, WsServer::message_ptr) {}

bool init(int port)
{
	try {
		_ws.clear_access_channels(websocketpp::log::alevel::all);
		_ws.clear_error_channels(websocketpp::log::elevel::all);
		_ws.init_asio();
		_ws.set_reuse_addr(true);

		// Disable Nagle on every accepted socket — PCM chunks are small and
		// time-sensitive; we never want the kernel to hold one waiting for the
		// next one.
		_ws.set_socket_init_handler([](websocketpp::connection_hdl,
		                               websocketpp::lib::asio::ip::tcp::socket& s) {
			websocketpp::lib::asio::error_code ec;
			s.set_option(websocketpp::lib::asio::ip::tcp::no_delay(true), ec);
		});

		_ws.set_open_handler(&onOpen);
		_ws.set_close_handler(&onClose);
		_ws.set_message_handler(&onMessage);

		_ws.listen(port);
		_ws.start_accept();

		_active = true;
		_wsThread = std::thread([]() { _ws.run(); });

		printf("[maplecast-audio-ws] audio-only WebSocket server on ws://0.0.0.0:%d\n", port);
		return true;
	} catch (const std::exception& e) {
		printf("[maplecast-audio-ws] init failed: %s\n", e.what());
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
	return _active.load(std::memory_order_relaxed);
}

void broadcastBinary(const void* data, size_t size)
{
	if (!_active.load(std::memory_order_relaxed)) return;
	if (_clientCount.load(std::memory_order_relaxed) == 0) return;

	// Copy payload once into a std::string (websocketpp's buffer type),
	// then post the entire send operation onto our private io_service
	// thread. Caller returns immediately; all socket work happens on
	// _wsThread with zero contention with the TA mirror pipeline.
	std::string payload(reinterpret_cast<const char*>(data), size);
	_ws.get_io_service().post([payload = std::move(payload)]() mutable {
		std::vector<ConnHdl> targets;
		{
			std::lock_guard<std::mutex> lock(_connMutex);
			targets.reserve(_connections.size());
			for (auto& c : _connections) targets.push_back(c);
		}
		for (auto& conn : targets) {
			try {
				_ws.send(conn, payload.data(), payload.size(),
					websocketpp::frame::opcode::binary);
			} catch (...) {}
		}
	});
}

} // namespace maplecast_audio_ws
