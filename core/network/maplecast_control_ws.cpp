/*
	MapleCast Control WebSocket — see maplecast_control_ws.h.

	Loopback-bound JSON command channel for /overlord admin operations.
	Commands enqueue into a thread-safe queue; the render thread drains
	once per frame and executes them serially.

	Wire protocol: text frames, one JSON object per frame.

	Client → server commands:
	  {"cmd":"ping","reply_id":"X"}
	    → {"ok":true,"cmd":"ping","reply_id":"X","data":{"pong":true}}

	  {"cmd":"savestate_save","slot":N,"reply_id":"X"}
	    → {"ok":true,"cmd":"savestate_save","reply_id":"X","data":{"slot":N,"path":"...","size":N}}
	    Errors: invalid slot (out of [0,99]), dc_savestate failed

	  {"cmd":"savestate_load","slot":N,"reply_id":"X"}
	    → {"ok":true,"cmd":"savestate_load","reply_id":"X","data":{"slot":N}}
	    After successful load, automatically calls
	    maplecast_mirror::requestSyncBroadcast() so the next frame ships
	    a full SYNC and mirror clients realign their VRAM/PVR shadows.
	    Errors: invalid slot, dc_loadstate threw

	  {"cmd":"reset","reply_id":"X"}
	    → {"ok":true,"cmd":"reset","reply_id":"X","data":{}}
	    Soft-reset the emulator (dc_reset(true)). Triggers a fresh SYNC.

	  {"cmd":"status","reply_id":"X"}
	    → {"ok":true,"cmd":"status","reply_id":"X","data":{"frame":N,"slot":N,...}}
	    Doesn't actually need to bounce to the render thread — the
	    queryable state (frame count, slot from settings) is read by
	    the WS handler thread directly. Returned synchronously.

	Errors (any cmd):
	  {"ok":false,"cmd":"...","reply_id":"X","error":"human message"}

	Threading model:
	  - Listener + accept on _wsThread (single-threaded asio)
	  - Message handler runs on _wsThread, parses JSON, enqueues Command
	  - Render thread calls drainCommandQueue() once per frame, executes
	    each command, sends reply back via stored ConnHdl
	  - Queue is bounded (16 entries). Overflow → reject with error.
*/
#include "maplecast_control_ws.h"
#include "maplecast_mirror.h"
#include "emulator.h"        // dc_savestate, dc_loadstate
#include "cfg/option.h"      // config::SavestateSlot

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include "json/json.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

using ControlWsServer = websocketpp::server<websocketpp::config::asio>;
using ControlConnHdl = websocketpp::connection_hdl;
using json = nlohmann::json;

namespace maplecast_control_ws
{

// ========================= Server state =========================

static ControlWsServer _ws;
static std::thread _wsThread;
static std::atomic<bool> _active{false};
static std::set<ControlConnHdl, std::owner_less<ControlConnHdl>> _connections;
static std::mutex _connMutex;

// ========================= Command queue =========================

enum class CmdType {
	Ping,
	SavestateSave,
	SavestateLoad,
	Reset,
};

struct Command {
	CmdType type;
	int slot = 0;                // savestate cmds
	std::string reply_id;        // client correlation
	ControlConnHdl conn;         // who to reply to
};

static constexpr size_t MAX_QUEUE = 16;
static std::deque<Command> _queue;
static std::mutex _queueMutex;

// Push a command onto the queue. Returns false if the queue is full.
// Called from the WS handler thread.
static bool enqueueCommand(Command cmd)
{
	std::lock_guard<std::mutex> lock(_queueMutex);
	if (_queue.size() >= MAX_QUEUE) return false;
	_queue.push_back(std::move(cmd));
	return true;
}

// ========================= Reply helpers =========================

// Send a JSON text frame to a single connection. Swallows exceptions
// because the connection might be gone by the time the render thread
// gets around to replying. Logs but doesn't propagate.
static void sendJson(ControlConnHdl conn, const json& payload)
{
	if (!_active.load(std::memory_order_relaxed)) return;
	try {
		_ws.send(conn, payload.dump(), websocketpp::frame::opcode::text);
	} catch (const std::exception& e) {
		printf("[control-ws] send failed: %s\n", e.what());
	} catch (...) {
		printf("[control-ws] send failed: unknown\n");
	}
}

static json okReply(const Command& cmd, const char* cmdName, json data = json::object())
{
	return {
		{"ok", true},
		{"cmd", cmdName},
		{"reply_id", cmd.reply_id},
		{"data", data},
	};
}

static json errReply(const Command& cmd, const char* cmdName, const std::string& error)
{
	return {
		{"ok", false},
		{"cmd", cmdName},
		{"reply_id", cmd.reply_id},
		{"error", error},
	};
}

static json errReplyImmediate(const std::string& reply_id, const char* cmdName, const std::string& error)
{
	return {
		{"ok", false},
		{"cmd", cmdName},
		{"reply_id", reply_id},
		{"error", error},
	};
}

// ========================= Command execution (render thread) =========================

static void executePing(const Command& cmd)
{
	json data = {{"pong", true}};
	sendJson(cmd.conn, okReply(cmd, "ping", data));
}

static void executeSavestateSave(const Command& cmd)
{
	if (cmd.slot < 0 || cmd.slot > 99) {
		sendJson(cmd.conn, errReply(cmd, "savestate_save", "slot out of range [0,99]"));
		return;
	}
	try {
		dc_savestate(cmd.slot, nullptr, 0);
		printf("[control-ws] dc_savestate(%d) OK\n", cmd.slot);
		json data = {
			{"slot", cmd.slot},
		};
		sendJson(cmd.conn, okReply(cmd, "savestate_save", data));
	} catch (const std::exception& e) {
		printf("[control-ws] dc_savestate(%d) threw: %s\n", cmd.slot, e.what());
		sendJson(cmd.conn, errReply(cmd, "savestate_save", e.what()));
	} catch (...) {
		printf("[control-ws] dc_savestate(%d) threw unknown\n", cmd.slot);
		sendJson(cmd.conn, errReply(cmd, "savestate_save", "unknown exception"));
	}
}

static void executeSavestateLoad(const Command& cmd)
{
	if (cmd.slot < 0 || cmd.slot > 99) {
		sendJson(cmd.conn, errReply(cmd, "savestate_load", "slot out of range [0,99]"));
		return;
	}
	try {
		dc_loadstate(cmd.slot);
		printf("[control-ws] dc_loadstate(%d) OK\n", cmd.slot);

		// CRITICAL: trigger a fresh SYNC so the mirror's per-region
		// shadows realign. Without this, the next per-frame VRAM diff
		// is computed against the pre-load shadow base and ships
		// wrong-base deltas grafted onto stale state. Wasm clients
		// see corrupt VRAM for seconds. See ARCHITECTURE.md bug #8
		// and the existing client_request_sync handler in
		// maplecast_mirror.cpp.
		maplecast_mirror::requestSyncBroadcast();

		json data = {{"slot", cmd.slot}};
		sendJson(cmd.conn, okReply(cmd, "savestate_load", data));
	} catch (const std::exception& e) {
		printf("[control-ws] dc_loadstate(%d) threw: %s\n", cmd.slot, e.what());
		sendJson(cmd.conn, errReply(cmd, "savestate_load", e.what()));
	} catch (...) {
		printf("[control-ws] dc_loadstate(%d) threw unknown\n", cmd.slot);
		sendJson(cmd.conn, errReply(cmd, "savestate_load", "unknown exception"));
	}
}

static void executeReset(const Command& cmd)
{
	try {
		emu.requestReset();
		// Same shadow-realignment concern as load — the reset path
		// invalidates VRAM and re-runs reios. Force a fresh SYNC.
		maplecast_mirror::requestSyncBroadcast();
		printf("[control-ws] reset OK\n");
		sendJson(cmd.conn, okReply(cmd, "reset"));
	} catch (const std::exception& e) {
		printf("[control-ws] reset threw: %s\n", e.what());
		sendJson(cmd.conn, errReply(cmd, "reset", e.what()));
	} catch (...) {
		sendJson(cmd.conn, errReply(cmd, "reset", "unknown exception"));
	}
}

void drainCommandQueue()
{
	if (!_active.load(std::memory_order_relaxed)) return;

	// Move queue contents into a local buffer under the lock, then
	// execute outside the lock so the WS handler can keep enqueueing
	// while we run.
	std::deque<Command> local;
	{
		std::lock_guard<std::mutex> lock(_queueMutex);
		if (_queue.empty()) return;
		local.swap(_queue);
	}

	for (auto& cmd : local) {
		switch (cmd.type) {
			case CmdType::Ping:           executePing(cmd); break;
			case CmdType::SavestateSave:  executeSavestateSave(cmd); break;
			case CmdType::SavestateLoad:  executeSavestateLoad(cmd); break;
			case CmdType::Reset:          executeReset(cmd); break;
		}
	}
}

// ========================= WS handlers =========================

static void onOpen(ControlConnHdl hdl)
{
	std::lock_guard<std::mutex> lock(_connMutex);
	_connections.insert(hdl);
	printf("[control-ws] client connected (total: %zu)\n", _connections.size());
}

static void onClose(ControlConnHdl hdl)
{
	std::lock_guard<std::mutex> lock(_connMutex);
	_connections.erase(hdl);
	printf("[control-ws] client disconnected (total: %zu)\n", _connections.size());
}

static void onMessage(ControlConnHdl hdl, ControlWsServer::message_ptr msg)
{
	if (msg->get_opcode() != websocketpp::frame::opcode::text) {
		// Control WS is text-only. Drop binary frames silently.
		return;
	}

	const std::string& payload = msg->get_payload();
	std::string reply_id;
	std::string cmdName;

	json parsed;
	try {
		parsed = json::parse(payload);
	} catch (const std::exception& e) {
		// Best-effort error reply with no reply_id (we couldn't parse it)
		json err = errReplyImmediate("", "?", std::string("invalid JSON: ") + e.what());
		sendJson(hdl, err);
		return;
	}

	if (!parsed.is_object()) {
		sendJson(hdl, errReplyImmediate("", "?", "payload must be a JSON object"));
		return;
	}

	if (parsed.contains("reply_id") && parsed["reply_id"].is_string())
		reply_id = parsed["reply_id"].get<std::string>();

	if (!parsed.contains("cmd") || !parsed["cmd"].is_string()) {
		sendJson(hdl, errReplyImmediate(reply_id, "?", "missing or non-string 'cmd' field"));
		return;
	}
	cmdName = parsed["cmd"].get<std::string>();

	Command cmd;
	cmd.reply_id = reply_id;
	cmd.conn = hdl;

	if (cmdName == "ping") {
		cmd.type = CmdType::Ping;
	} else if (cmdName == "savestate_save") {
		cmd.type = CmdType::SavestateSave;
		if (!parsed.contains("slot") || !parsed["slot"].is_number_integer()) {
			sendJson(hdl, errReplyImmediate(reply_id, "savestate_save", "missing 'slot' integer"));
			return;
		}
		cmd.slot = parsed["slot"].get<int>();
	} else if (cmdName == "savestate_load") {
		cmd.type = CmdType::SavestateLoad;
		if (!parsed.contains("slot") || !parsed["slot"].is_number_integer()) {
			sendJson(hdl, errReplyImmediate(reply_id, "savestate_load", "missing 'slot' integer"));
			return;
		}
		cmd.slot = parsed["slot"].get<int>();
	} else if (cmdName == "reset") {
		cmd.type = CmdType::Reset;
	} else if (cmdName == "status") {
		// Synchronous query — answered by the WS handler thread, no
		// queue bounce needed. Reads cheap atomic-ish state.
		json data = {
			{"slot", config::SavestateSlot.get()},
		};
		sendJson(hdl, json{
			{"ok", true},
			{"cmd", "status"},
			{"reply_id", reply_id},
			{"data", data},
		});
		return;
	} else {
		sendJson(hdl, errReplyImmediate(reply_id, cmdName.c_str(),
			std::string("unknown command: ") + cmdName));
		return;
	}

	if (!enqueueCommand(std::move(cmd))) {
		sendJson(hdl, errReplyImmediate(reply_id, cmdName.c_str(),
			"command queue full (max 16 pending)"));
	}
}

// ========================= Lifecycle =========================

bool init(int port)
{
	if (_active.load()) {
		printf("[control-ws] already initialized\n");
		return true;
	}

	try {
		_ws.clear_access_channels(websocketpp::log::alevel::all);
		_ws.clear_error_channels(websocketpp::log::elevel::all);
		_ws.init_asio();
		_ws.set_reuse_addr(true);

		_ws.set_open_handler(&onOpen);
		_ws.set_close_handler(&onClose);
		_ws.set_message_handler(&onMessage);

		// CRITICAL: bind to LOOPBACK ONLY. Control commands include
		// destructive operations (savestate save/load, reset). Public
		// exposure would be an instant P0 vulnerability. The /overlord
		// admin panel reaches us through the same-VPS relay, which
		// runs on the same box and can connect over loopback.
		websocketpp::lib::asio::ip::tcp::endpoint loopback(
			websocketpp::lib::asio::ip::address_v4::loopback(),
			static_cast<uint16_t>(port));
		_ws.listen(loopback);
		_ws.start_accept();

		_active.store(true);
		_wsThread = std::thread([&]() {
			try {
				_ws.run();
			} catch (const std::exception& e) {
				printf("[control-ws] run() threw: %s\n", e.what());
			} catch (...) {
				printf("[control-ws] run() threw unknown\n");
			}
		});

		printf("[control-ws] listening on ws://127.0.0.1:%d (loopback only)\n", port);
		return true;
	} catch (const std::exception& e) {
		printf("[control-ws] init failed: %s\n", e.what());
		_active.store(false);
		return false;
	}
}

void shutdown()
{
	if (!_active.exchange(false)) return;
	try { _ws.stop(); } catch (...) {}
	if (_wsThread.joinable()) _wsThread.join();
	{
		std::lock_guard<std::mutex> lock(_connMutex);
		_connections.clear();
	}
	{
		std::lock_guard<std::mutex> lock(_queueMutex);
		_queue.clear();
	}
}

bool active()
{
	return _active.load();
}

} // namespace maplecast_control_ws
