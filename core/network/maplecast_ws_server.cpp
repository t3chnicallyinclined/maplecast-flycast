/*
	MapleCast WebSocket Server — binary mirror broadcast + JSON lobby on port 7200.

	On new client connect: sends full VRAM + PVR regs as initial sync, plus lobby status.
	Binary frames: delta TA commands broadcast to all clients.
	Text frames: JSON lobby (join, status) + binary gamepad input forwarding to UDP 7100.
*/
#include "maplecast_ws_server.h"
#include "maplecast_compress.h"
#include "maplecast_input_server.h"
#include "maplecast_gamestate.h"
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
	ConnHdl conn;
};

// ==================== P2P Spectator Relay Tree ====================
// Server feeds binary TA frames to 2-3 "seed" spectators.
// Seeds relay to children via WebRTC DataChannels (browser-side JS).
// Server manages topology, signals parent/child assignments via JSON.

static const int MAX_SEEDS = 3;
static const int MAX_CHILDREN = 3;
static int _nextPeerId = 1;

struct RelayNode {
	std::string peerId;
	ConnHdl conn;
	void* connKey = nullptr;
	bool isPlayer = false;      // players get direct stream, never relay
	bool isSeed = false;        // seeds receive binary from server
	bool canRelay = true;       // client reported relay capability
	std::string parentId;       // empty for seeds/players
	std::vector<std::string> children;
	int64_t connectedAt = 0;
};

static std::map<std::string, RelayNode> _relayTree;
static std::vector<std::string> _seedPeers;
static std::map<void*, std::string> _connToPeerId;  // connKey → peerId
static std::vector<QueueEntry> _queue;

// Loss detection state
static bool _matchActive = false;
static bool _matchEndHandled = false;
static int64_t _matchEndTime = 0; // when match ended (for delay before kick)

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
	status["stream_kbps"] = (int64_t)(t.compressedSize * t.fps * 8 / 1024);
	status["raw_kbps"] = (int64_t)(t.deltaSize * t.fps * 8 / 1024);
	status["publish_us"] = (int64_t)t.publishUs;
	status["compress_us"] = (int64_t)t.compressUs;
	status["compression_ratio"] = t.compressedSize > 0 ? (double)t.deltaSize / t.compressedSize : 1.0;
	status["fps"] = (int64_t)t.fps;
	status["dirty"] = t.dirtyPages;
	status["registering"] = maplecast_input::isRegistering();
	status["web_registering"] = maplecast_input::isWebRegistering();
	status["web_registering_user"] = maplecast_input::isWebRegistering() ?
		maplecast_input::webRegisteringUsername() : "";
	status["sticks"] = maplecast_input::registeredStickCount();
	status["relay_seeds"] = (int)_seedPeers.size();
	status["relay_nodes"] = (int)_relayTree.size();
	status["input_buffer_ms"] = maplecast_input::getBufferMs();
	status["buffer_pending"] = maplecast_input::isBufferPending();

	// Game state for leaderboard/stats
	maplecast_gamestate::GameState gs;
	maplecast_gamestate::readGameState(gs);
	if (gs.in_match) {
		json game;
		game["in_match"] = true;
		game["timer"] = gs.game_timer;
		game["stage"] = gs.stage_id;
		game["p1_combo"] = gs.p1_combo;
		game["p2_combo"] = gs.p2_combo;
		game["p1_meter"] = gs.p1_meter_level;
		game["p2_meter"] = gs.p2_meter_level;
		// Character health: 3 per player
		json p1hp = json::array();
		json p2hp = json::array();
		json p1chars = json::array();
		json p2chars = json::array();
		for (int i = 0; i < 3; i++) {
			p1hp.push_back(gs.chars[i*2].health);
			p2hp.push_back(gs.chars[i*2+1].health);
			p1chars.push_back(gs.chars[i*2].character_id);
			p2chars.push_back(gs.chars[i*2+1].character_id);
		}
		game["p1_hp"] = p1hp;
		game["p2_hp"] = p2hp;
		game["p1_chars"] = p1chars;
		game["p2_chars"] = p2chars;
		status["game"] = game;
	}
	return status;
}

static void checkMatchEnd()
{
	maplecast_gamestate::GameState gs;
	maplecast_gamestate::readGameState(gs);

	if (gs.in_match) {
		_matchActive = true;
		_matchEndHandled = false;

		// Check if all 3 chars on one side are dead
		bool p1dead = (gs.chars[0].health == 0 && gs.chars[2].health == 0 && gs.chars[4].health == 0);
		bool p2dead = (gs.chars[1].health == 0 && gs.chars[3].health == 0 && gs.chars[5].health == 0);

		if ((p1dead || p2dead) && !_matchEndHandled) {
			_matchEndHandled = true;
			_matchEndTime = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::high_resolution_clock::now().time_since_epoch()).count();

			int loserSlot = p1dead ? 0 : 1;
			int winnerSlot = p1dead ? 1 : 0;
			const auto& loser = maplecast_input::getPlayer(loserSlot);
			const auto& winner = maplecast_input::getPlayer(winnerSlot);
			printf("[maplecast-ws] MATCH END: P%d (%s) wins! P%d (%s) eliminated.\n",
				winnerSlot+1, winner.name, loserSlot+1, loser.name);

			// Notify all clients
			json endMsg;
			endMsg["type"] = "match_end";
			endMsg["winner"] = winnerSlot;
			endMsg["winner_name"] = std::string(winner.name);
			endMsg["loser"] = loserSlot;
			endMsg["loser_name"] = std::string(loser.name);
			std::string endStr = endMsg.dump();
			{
				std::lock_guard<std::mutex> lock(_connMutex);
				for (auto& conn : _connections)
					try { _ws.send(conn, endStr, websocketpp::frame::opcode::text); } catch (...) {}
			}
		}
	}
	else if (_matchActive && _matchEndHandled)
	{
		// Match ended and game returned to non-match state (character select, etc.)
		// Wait 3 seconds after match end, then kick loser and promote next in queue
		int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::high_resolution_clock::now().time_since_epoch()).count();

		if (now - _matchEndTime > 3000) {
			_matchActive = false;

			// Find loser's slot (the one who had all chars dead)
			// Re-read to confirm
			maplecast_gamestate::GameState gs2;
			maplecast_gamestate::readGameState(gs2);
			// The loser was already determined — disconnect them
			// Both players might have been disconnected by now, check
			for (int slot = 0; slot < 2; slot++) {
				const auto& p = maplecast_input::getPlayer(slot);
				if (!p.connected) continue;

				// If queue is empty, don't kick anyone (winner stays on)
				if (_queue.empty()) break;
			}

			// Simpler: notify the first person in queue it's their turn
			if (!_queue.empty()) {
				auto next = _queue.front();
				json yourTurn;
				yourTurn["type"] = "your_turn";
				yourTurn["msg"] = "It's your turn! Press Start to play!";
				try {
					_ws.send(next.conn, yourTurn.dump(), websocketpp::frame::opcode::text);
				} catch (...) {}
				printf("[maplecast-ws] Notified %s: it's your turn!\n", next.name.c_str());
			}
		}
	}
}

// ==================== Relay Topology Helpers ====================

static std::string generatePeerId()
{
	return "p" + std::to_string(_nextPeerId++);
}

static void sendJson(ConnHdl hdl, const json& msg)
{
	try { _ws.send(hdl, msg.dump(), websocketpp::frame::opcode::text); }
	catch (...) {}
}

// Find the shallowest relay node with available child slots (BFS)
static std::string findBestParent()
{
	// BFS from seeds outward
	std::vector<std::string> queue;
	for (auto& sid : _seedPeers) queue.push_back(sid);

	size_t idx = 0;
	while (idx < queue.size()) {
		const std::string& id = queue[idx++];
		auto it = _relayTree.find(id);
		if (it == _relayTree.end()) continue;
		if ((int)it->second.children.size() < MAX_CHILDREN && it->second.canRelay)
			return id;
		for (auto& child : it->second.children)
			queue.push_back(child);
	}
	return "";  // tree is full
}

static void makeSeed(const std::string& peerId)
{
	auto it = _relayTree.find(peerId);
	if (it == _relayTree.end()) return;
	it->second.isSeed = true;
	it->second.parentId.clear();
	_seedPeers.push_back(peerId);

	// Send SYNC to new seed
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
	try { _ws.send(it->second.conn, syncBuf.data(), syncSize, websocketpp::frame::opcode::binary); }
	catch (...) {}

	sendJson(it->second.conn, {{"type", "relay_role"}, {"role", "seed"}, {"peerId", peerId}});
	printf("[relay] %s promoted to SEED (%d seeds)\n", peerId.c_str(), (int)_seedPeers.size());
}

static void assignChild(const std::string& childId, const std::string& parentId)
{
	auto childIt = _relayTree.find(childId);
	auto parentIt = _relayTree.find(parentId);
	if (childIt == _relayTree.end() || parentIt == _relayTree.end()) return;

	childIt->second.parentId = parentId;
	parentIt->second.children.push_back(childId);

	std::string role = childIt->second.canRelay ? "relay" : "leaf";
	sendJson(childIt->second.conn, {{"type", "relay_role"}, {"role", role}, {"peerId", childId}});
	sendJson(childIt->second.conn, {{"type", "relay_assign_parent"}, {"parentId", parentId}});
	sendJson(parentIt->second.conn, {{"type", "relay_assign_child"}, {"childId", childId}});

	printf("[relay] %s assigned to parent %s (role=%s)\n", childId.c_str(), parentId.c_str(), role.c_str());
}

static void removeFromTree(const std::string& peerId)
{
	auto it = _relayTree.find(peerId);
	if (it == _relayTree.end()) return;

	// Remove from parent's children list
	if (!it->second.parentId.empty()) {
		auto parentIt = _relayTree.find(it->second.parentId);
		if (parentIt != _relayTree.end()) {
			auto& pc = parentIt->second.children;
			pc.erase(std::remove(pc.begin(), pc.end(), peerId), pc.end());
			sendJson(parentIt->second.conn, {{"type", "relay_remove_child"}, {"childId", peerId}});
		}
	}

	// Remove from seed list
	if (it->second.isSeed)
		_seedPeers.erase(std::remove(_seedPeers.begin(), _seedPeers.end(), peerId), _seedPeers.end());

	// Orphan children — reassign them
	std::vector<std::string> orphans = it->second.children;
	_relayTree.erase(it);

	for (auto& orphanId : orphans) {
		auto orphanIt = _relayTree.find(orphanId);
		if (orphanIt == _relayTree.end()) continue;
		orphanIt->second.parentId.clear();
		sendJson(orphanIt->second.conn, {{"type", "relay_orphaned"}});

		// Try to find a new parent
		if ((int)_seedPeers.size() < MAX_SEEDS && orphanIt->second.canRelay) {
			makeSeed(orphanId);
		} else {
			std::string newParent = findBestParent();
			if (!newParent.empty())
				assignChild(orphanId, newParent);
			else
				makeSeed(orphanId);  // no room, make it a seed
		}
	}

	printf("[relay] %s removed from tree, %d orphans reassigned\n", peerId.c_str(), (int)orphans.size());
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

	// Always send SYNC on connect — backward compat for clients without relay.js
	// (test-renderer.html, standalone clients, etc.)
	// The bandwidth savings come from delta frames (broadcastBinary), not SYNC.
	// Non-seed relay clients will also get SYNC from their parent via WebRTC,
	// but getting it from the server first means they can start rendering immediately.
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
		MirrorCompressor syncComp;
		syncComp.init(syncSize + 128);
		size_t compSyncSize = 0;
		uint64_t compUs = 0;
		const uint8_t* compSync = syncComp.compress(syncBuf.data(), (uint32_t)syncSize, compSyncSize, compUs, 3);
		_ws.send(hdl, compSync, compSyncSize, websocketpp::frame::opcode::binary);
		syncComp.destroy();
		printf("[maplecast-ws] sent compressed sync: %.1f MB -> %.1f MB (%.1fx) in %lums\n",
			syncSize / (1024.0 * 1024.0), compSyncSize / (1024.0 * 1024.0),
			(double)syncSize / compSyncSize, compUs / 1000);
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
		// Reset buffer when a player leaves
		maplecast_input::setBufferMs(0);
	}

	void* key = nullptr;
	{
		std::lock_guard<std::mutex> lock(_connMutex);
		_connections.erase(hdl);
		try {
			key = (void*)_ws.get_con_from_hdl(hdl).get();
			_connSlot.erase(key);
			_queue.erase(std::remove_if(_queue.begin(), _queue.end(),
				[key](const QueueEntry& e) { return e.key == key; }), _queue.end());
		} catch (...) {}
		_clientCount--;
	}

	// Remove from relay tree (handles orphan reassignment)
	if (key) {
		auto peerIt = _connToPeerId.find(key);
		if (peerIt != _connToPeerId.end()) {
			removeFromTree(peerIt->second);
			_connToPeerId.erase(peerIt);
		}
	}

	printf("[maplecast-ws] client disconnected (%d total, %d seeds, %d relay nodes)\n",
		_clientCount.load(), (int)_seedPeers.size(), (int)_relayTree.size());

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

						// Mark as player in relay tree (gets direct stream, never relays)
						auto peerIt = _connToPeerId.find(key);
						if (peerIt != _connToPeerId.end()) {
							auto nodeIt = _relayTree.find(peerIt->second);
							if (nodeIt != _relayTree.end())
								nodeIt->second.isPlayer = true;
						}
					} catch (...) {}
				}

				json resp = {{"type", "assigned"}, {"slot", slot}, {"id", playerId.substr(0,8)}, {"name", name}};
				_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);

				// Patch in-game player name
				if (slot >= 0)
					maplecast_gamestate::setPlayerName(slot, name.c_str());

				// Auto-propose buffer when both players are connected
				if (maplecast_input::connectedCount() == 2 && !maplecast_input::isBufferPending()) {
					int rec = maplecast_input::getRecommendedBufferMs();
					if (rec > 0) {
						maplecast_input::proposeBuffer(rec);
						json prop = {{"type", "buffer_propose"}, {"ms", rec},
							{"p1_type", maplecast_input::getPlayer(0).type == maplecast_input::InputType::NobdUDP ? "hardware" : "browser"},
							{"p2_type", maplecast_input::getPlayer(1).type == maplecast_input::InputType::NobdUDP ? "hardware" : "browser"}};
						std::string propStr = prop.dump();
						std::lock_guard<std::mutex> lock2(_connMutex);
						for (auto& conn : _connections)
							try { _ws.send(conn, propStr, websocketpp::frame::opcode::text); } catch (...) {}
					}
				}

				// Broadcast updated status to all
				broadcastStatus();
			}
			else if (ctrl["type"] == "leave")
			{
				int slot = getSlotForConn(hdl);
				if (slot >= 0) {
					maplecast_input::disconnectPlayer(slot);
					maplecast_gamestate::restorePlayerNames();
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
						_queue.push_back({key, name, hdl});
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
			else if (ctrl["type"] == "register_stick_web")
			{
				// Web-based registration: username + "press any button"
				std::string username = ctrl.value("username", "");
				if (!maplecast_input::isValidUsername(username.c_str())) {
					json resp = {{"type", "register_error"}, {"msg", "Invalid username. 4-12 chars, letters/numbers/underscore only."}};
					_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
				} else {
					// Check if username is already registered
					auto info = maplecast_input::getStickInfo(username.c_str());
					if (info.registered) {
						json resp = {{"type", "register_error"}, {"msg", "Username already registered. Unregister first."}};
						_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
					} else {
						maplecast_input::startWebRegistration(username.c_str());
						json resp = {{"type", "register_waiting"}, {"username", username}, {"msg", "Press any button on your stick..."}};
						_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
					}
				}
			}
			else if (ctrl["type"] == "cancel_register_web")
			{
				maplecast_input::cancelWebRegistration();
				json resp = {{"type", "register_cancelled"}};
				_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
			}
			else if (ctrl["type"] == "check_stick")
			{
				// Check if a username has a registered stick and if it's online
				std::string username = ctrl.value("username", "");
				auto info = maplecast_input::getStickInfo(username.c_str());
				json resp = {
					{"type", "stick_status"},
					{"username", username},
					{"registered", info.registered},
					{"online", info.online}
				};
				_ws.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
			}
			else if (ctrl["type"] == "patch_name")
			{
				int slot = ctrl.value("slot", 0);
				std::string name = ctrl.value("name", "");
				printf("[maplecast-ws] PATCH_NAME received: slot=%d name='%s'\n", slot, name.c_str());
				if (!name.empty()) {
					maplecast_gamestate::setPlayerName(slot, name.c_str());
					// Verify the write took effect
					uint8_t check = addrspace::read8(0x8CBBC31E);
					printf("[maplecast-ws] Verify: 0x8CBBC31E = 0x%02X ('%c')\n", check, check >= 32 ? check : '.');
				}
			}
			// ==================== P2P Relay Messages ====================
			else if (ctrl["type"] == "relay_ready")
			{
				bool canRelay = ctrl.value("canRelay", true);
				try {
					void* key = (void*)_ws.get_con_from_hdl(hdl).get();
					std::string peerId = generatePeerId();

					RelayNode node;
					node.peerId = peerId;
					node.conn = hdl;
					node.connKey = key;
					node.canRelay = canRelay;
					node.connectedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::high_resolution_clock::now().time_since_epoch()).count();

					_relayTree[peerId] = node;
					_connToPeerId[key] = peerId;

					// Assign role: seed if we need more, otherwise find a parent
					if ((int)_seedPeers.size() < MAX_SEEDS && canRelay) {
						makeSeed(peerId);
					} else {
						std::string parentId = findBestParent();
						if (!parentId.empty()) {
							assignChild(peerId, parentId);
						} else {
							// No room in tree — make another seed
							makeSeed(peerId);
						}
					}
				} catch (...) {}
			}
			else if (ctrl["type"] == "relay_signal")
			{
				// Forward WebRTC signaling between peers
				std::string toPeerId = ctrl.value("toPeerId", "");
				auto toIt = _relayTree.find(toPeerId);
				if (toIt != _relayTree.end()) {
					// Find sender's peerId
					std::string fromPeerId;
					try {
						void* key = (void*)_ws.get_con_from_hdl(hdl).get();
						auto fromIt = _connToPeerId.find(key);
						if (fromIt != _connToPeerId.end()) fromPeerId = fromIt->second;
					} catch (...) {}

					if (!fromPeerId.empty()) {
						json fwd;
						fwd["type"] = "relay_signal";
						fwd["fromPeerId"] = fromPeerId;
						fwd["signal"] = ctrl["signal"];
						sendJson(toIt->second.conn, fwd);
					}
				}
			}
			else if (ctrl["type"] == "relay_parent_lost")
			{
				// Child reports parent DataChannel died — reassign
				try {
					void* key = (void*)_ws.get_con_from_hdl(hdl).get();
					auto peerIt = _connToPeerId.find(key);
					if (peerIt != _connToPeerId.end()) {
						std::string peerId = peerIt->second;
						auto nodeIt = _relayTree.find(peerId);
						if (nodeIt != _relayTree.end()) {
							// Clear parent
							nodeIt->second.parentId.clear();
							// Find new parent
							if ((int)_seedPeers.size() < MAX_SEEDS && nodeIt->second.canRelay) {
								makeSeed(peerId);
							} else {
								std::string newParent = findBestParent();
								if (!newParent.empty())
									assignChild(peerId, newParent);
								else
									makeSeed(peerId);
							}
						}
					}
				} catch (...) {}
			}
			else if (ctrl["type"] == "relay_stats")
			{
				// Health report from relay node — log for now
			}
			// ==================== Input Buffer Negotiation ====================
			else if (ctrl["type"] == "buffer_propose")
			{
				// Server or admin triggers negotiation
				int ms = ctrl.value("ms", -1);
				if (ms < 0) ms = maplecast_input::getRecommendedBufferMs();
				maplecast_input::proposeBuffer(ms);

				// Notify both players
				json prop = {{"type", "buffer_propose"},
					{"ms", ms},
					{"p1_type", maplecast_input::getPlayer(0).connected ?
						(maplecast_input::getPlayer(0).type == maplecast_input::InputType::NobdUDP ? "hardware" : "browser") : "none"},
					{"p2_type", maplecast_input::getPlayer(1).connected ?
						(maplecast_input::getPlayer(1).type == maplecast_input::InputType::NobdUDP ? "hardware" : "browser") : "none"}};
				std::string propStr = prop.dump();
				std::lock_guard<std::mutex> lock(_connMutex);
				for (auto& conn : _connections)
					try { _ws.send(conn, propStr, websocketpp::frame::opcode::text); } catch (...) {}
			}
			else if (ctrl["type"] == "buffer_accept")
			{
				int slot = getSlotForConn(hdl);
				if (slot >= 0) {
					bool both = maplecast_input::acceptBuffer(slot);
					if (both) {
						// Notify all clients that buffer is active
						json msg = {{"type", "buffer_active"}, {"ms", maplecast_input::getBufferMs()}};
						std::string s = msg.dump();
						std::lock_guard<std::mutex> lock(_connMutex);
						for (auto& conn : _connections)
							try { _ws.send(conn, s, websocketpp::frame::opcode::text); } catch (...) {}
					}
				}
			}
			else if (ctrl["type"] == "buffer_reject")
			{
				int slot = getSlotForConn(hdl);
				if (slot >= 0) {
					maplecast_input::rejectBuffer(slot);
					json msg = {{"type", "buffer_rejected"}, {"slot", slot}};
					std::string s = msg.dump();
					std::lock_guard<std::mutex> lock(_connMutex);
					for (auto& conn : _connections)
						try { _ws.send(conn, s, websocketpp::frame::opcode::text); } catch (...) {}
				}
			}
			else if (ctrl["type"] == "buffer_set")
			{
				// Direct set (admin/debug)
				int ms = ctrl.value("ms", 0);
				maplecast_input::setBufferMs(ms);
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

		// CRITICAL: disable Nagle's algorithm on every accepted socket.
		// Without this, TCP buffers small writes (status JSON, ping echoes,
		// 4-byte input forwards) for up to 40ms hoping to coalesce them with
		// the next write. With it, every send hits the wire immediately.
		// The relay→home and browser→relay paths benefit by 0-40ms p99.
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

		// TODO: multi-threaded io_context. Several global structures
		// (_queue, _relayTree, _seedPeers) are read without _connMutex in
		// status broadcast and relay-tree helpers. Migrating to multi-threaded
		// asio requires auditing every access path first. For now, single
		// thread + the relay echoing pings locally keeps the hot path clean.
		_wsThread = std::thread([&]() { _ws.run(); });

		// Periodic status broadcast (every 1 second)
		_active = true;
		_statusThread = std::thread([]() {
			while (_active) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				if (_active && _clientCount.load() > 0) {
					checkMatchEnd();
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

	// Build set of connections that should NOT receive binary
	// (non-seed relay nodes that get frames via WebRTC instead)
	std::set<void*> relaySkip;
	for (auto& [key, peerId] : _connToPeerId) {
		auto it = _relayTree.find(peerId);
		if (it != _relayTree.end() && !it->second.isSeed && !it->second.isPlayer)
			relaySkip.insert(key);
	}

	for (auto& conn : _connections) {
		try {
			void* key = (void*)_ws.get_con_from_hdl(conn).get();
			if (relaySkip.count(key)) continue;  // gets frames via WebRTC relay
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
