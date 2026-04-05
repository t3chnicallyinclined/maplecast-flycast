/*
	MapleCast WebSocket Server — binary mirror broadcast + JSON lobby on port 7200.

	On new client connect: sends full VRAM + PVR regs as initial sync, plus lobby status.
	Binary frames: delta TA commands broadcast to all clients.
	Text frames: JSON lobby (join, status) + binary gamepad input forwarding to UDP 7100.
*/
#include "maplecast_ws_server.h"
#include "maplecast_input_server.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include "json/json.hpp"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <map>
#include <algorithm>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

using WsServer = websocketpp::server<websocketpp::config::asio>;
using ConnHdl = websocketpp::connection_hdl;
using json = nlohmann::json;

namespace maplecast_ws
{

static WsServer _ws;
static std::thread _wsThread;
static std::thread _statusThread;
static std::set<ConnHdl, std::owner_less<ConnHdl>> _connections;
static std::mutex _connMutex;
static std::atomic<int> _clientCount{0};
static bool _active = false;

// Lobby: connection → slot mapping, queue tracking
static std::map<void*, int> _connSlot;

struct QueueEntry {
	void* key;
	std::string name;
};
static std::vector<QueueEntry> _queue;

// Telemetry from mirror publish
static Telemetry _telemetry{};
static std::mutex _telemetryMutex;

// UDP socket for forwarding browser input to input server
static int _udpSock = -1;
static struct sockaddr_in _udpDest;

static int getSlotForConn(ConnHdl hdl)
{
	try {
		void* key = (void*)_ws.get_con_from_hdl(hdl).get();
		auto it = _connSlot.find(key);
		if (it != _connSlot.end()) return it->second;
	} catch (...) {}
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
	int players = (maplecast_input::getPlayer(0).connected ? 1 : 0)
	            + (maplecast_input::getPlayer(1).connected ? 1 : 0);
	// Each browser tab opens 2 WebSocket connections (parent page + iframe mirror)
	int viewers = (_clientCount.load() - players) / 2;
	if (viewers < 0) viewers = 0;
	json queueList = json::array();
	for (const auto& q : _queue)
		queueList.push_back(q.name);
	Telemetry t;
	{
		std::lock_guard<std::mutex> lock(_telemetryMutex);
		t = _telemetry;
	}
	json status;
	status["type"] = "status";
	status["p1"] = slotInfo(0);
	status["p2"] = slotInfo(1);
	status["spectators"] = viewers;
	status["queue"] = queueList;
	status["frame"] = t.frameNum;
	status["stream_kbps"] = (int64_t)(t.deltaSize * t.fps * 8 / 1024);
	status["publish_us"] = (int64_t)t.publishUs;
	status["fps"] = (int64_t)t.fps;
	status["dirty"] = t.dirtyPages;
	status["registering"] = maplecast_input::isRegistering();
	status["sticks"] = maplecast_input::registeredStickCount();
	return status;
}

static void broadcastStatus()
{
	std::string status = getStatus().dump();
	std::lock_guard<std::mutex> lock(_connMutex);
	for (auto& conn : _connections)
	{
		try { _ws.send(conn, status, websocketpp::frame::opcode::text); }
		catch (...) {}
	}
}

static void onOpen(ConnHdl hdl)
{
	{
		std::lock_guard<std::mutex> lock(_connMutex);
		_connections.insert(hdl);
		_clientCount++;
	}
	printf("[maplecast-ws] client connected (%d total)\n", _clientCount.load());

	// Send initial VRAM sync
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

	// Send lobby status
	try {
		_ws.send(hdl, getStatus().dump(), websocketpp::frame::opcode::text);
	} catch (...) {}
}

static void onClose(ConnHdl hdl)
{
	// Clean up slot assignment
	int slot = getSlotForConn(hdl);
	if (slot >= 0) {
		maplecast_input::disconnectPlayer(slot);
	}

	{
		std::lock_guard<std::mutex> lock(_connMutex);
		_connections.erase(hdl);
		try {
			void* key = (void*)_ws.get_con_from_hdl(hdl).get();
			_connSlot.erase(key);
			_queue.erase(std::remove_if(_queue.begin(), _queue.end(),
				[key](const QueueEntry& e) { return e.key == key; }), _queue.end());
		} catch (...) {}
		_clientCount--;
	}
	printf("[maplecast-ws] client disconnected (%d total)\n", _clientCount.load());

	// Notify remaining clients of updated status
	broadcastStatus();
}

static void onMessage(ConnHdl hdl, WsServer::message_ptr msg)
{
	if (msg->get_opcode() == websocketpp::frame::opcode::binary)
	{
		// Binary = gamepad input (4 bytes: LT, RT, btnHi, btnLo)
		const auto& data = msg->get_payload();
		if (data.size() == 4)
		{
			// Init UDP socket if needed
			if (_udpSock < 0)
			{
				_udpSock = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
				memset(&_udpDest, 0, sizeof(_udpDest));
				_udpDest.sin_family = AF_INET;
				_udpDest.sin_port = htons(7100);
				_udpDest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			}

			int slot = getSlotForConn(hdl);
			if (slot >= 0 && slot <= 1)
			{
				// Tagged 5-byte packet: slot + W3 data
				char tagged[5];
				tagged[0] = (char)slot;
				memcpy(tagged + 1, data.c_str(), 4);
				sendto(_udpSock, tagged, 5, 0, (struct sockaddr*)&_udpDest, sizeof(_udpDest));
			}
			else
			{
				// Unassigned player — send raw 4-byte
				sendto(_udpSock, data.c_str(), 4, 0, (struct sockaddr*)&_udpDest, sizeof(_udpDest));
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

				int slot = maplecast_input::registerPlayer(
					playerId.c_str(), name.c_str(), device.c_str(),
					maplecast_input::InputType::BrowserWS);

				// Register connection → slot mapping
				if (slot >= 0) {
					try {
						void* key = (void*)_ws.get_con_from_hdl(hdl).get();
						_connSlot[key] = slot;
					} catch (...) {}
				}

				json resp = {{"type", "assigned"}, {"slot", slot}, {"id", playerId.substr(0,8)}, {"name", name}};
				_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);

				// Broadcast updated status to all
				broadcastStatus();
			}
			else if (ctrl["type"] == "leave")
			{
				int slot = getSlotForConn(hdl);
				if (slot >= 0) {
					maplecast_input::disconnectPlayer(slot);
					try {
						void* key = (void*)_ws.get_con_from_hdl(hdl).get();
						_connSlot.erase(key);
					} catch (...) {}
				}

				json resp = {{"type", "assigned"}, {"slot", -1}};
				_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);

				broadcastStatus();
			}
			else if (ctrl["type"] == "queue_join")
			{
				std::string name = ctrl.value("name", "Anon");
				try {
					void* key = (void*)_ws.get_con_from_hdl(hdl).get();
					// Don't double-add
					bool found = false;
					for (const auto& q : _queue) { if (q.key == key) { found = true; break; } }
					if (!found)
						_queue.push_back({key, name});
				} catch (...) {}
				broadcastStatus();
			}
			else if (ctrl["type"] == "register_stick")
			{
				std::string browserId = ctrl.value("id", "");
				if (!browserId.empty()) {
					maplecast_input::startStickRegistration(browserId.c_str());
					json resp;
					resp["type"] = "register_started";
					resp["msg"] = "Tap any button 5 times, pause, then 5 times again";
					_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
				}
			}
			else if (ctrl["type"] == "unregister_stick")
			{
				std::string browserId = ctrl.value("id", "");
				if (!browserId.empty()) {
					maplecast_input::unregisterStick(browserId.c_str());
					json resp;
					resp["type"] = "stick_unregistered";
					_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
					broadcastStatus();
				}
			}
			else if (ctrl["type"] == "cancel_register")
			{
				maplecast_input::cancelStickRegistration();
			}
			else if (ctrl["type"] == "ping")
			{
				// Echo back with server timestamp for RTT measurement
				json resp;
				resp["type"] = "pong";
				resp["t"] = ctrl.value("t", 0.0);
				_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
			}
			else if (ctrl["type"] == "queue_leave")
			{
				try {
					void* key = (void*)_ws.get_con_from_hdl(hdl).get();
					_queue.erase(std::remove_if(_queue.begin(), _queue.end(),
						[key](const QueueEntry& e) { return e.key == key; }), _queue.end());
				} catch (...) {}
				broadcastStatus();
			}
		} catch (...) {}
	}
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

		// Periodic status broadcast (every 1 second)
		_active = true;
		_statusThread = std::thread([]() {
			while (_active) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				if (_active && _clientCount.load() > 0) {
					broadcastStatus();
				}
			}
		});

		printf("[maplecast-ws] WebSocket server on ws://0.0.0.0:%d (mirror + lobby)\n", port);
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
	if (_statusThread.joinable()) _statusThread.join();
	if (_udpSock >= 0) {
#ifdef _WIN32
		closesocket(_udpSock);
#else
		close(_udpSock);
#endif
		_udpSock = -1;
	}
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

void updateTelemetry(const Telemetry& t)
{
	std::lock_guard<std::mutex> lock(_telemetryMutex);
	_telemetry = t;
}

}  // namespace maplecast_ws
