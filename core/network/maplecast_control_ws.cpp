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
#include "maplecast_audio_client.h"
#include "maplecast_input_server.h"
#include "emulator.h"
#include "cfg/option.h"
#include "ui/gui.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/mem/mem_watch.h"
#include "input/gamepad_device.h"
#include "input/gamepad.h"
#include "input/mapping.h"
#include "maplecast_input_sink.h"
#include "maplecast_gamestate.h"
#include "maplecast_palette.h"

extern u32 kcode[4];
extern u16 lt[4], rt[4];

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
	OpenControls,
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

// Find the gamepad assigned to a specific DC port (0=P1, 1=P2).
// GetGamepad(index) returns by registration order, not by port.
static std::shared_ptr<GamepadDevice> findGamepadOnPort(int port)
{
	for (int i = 0; i < GamepadDevice::GetGamepadCount(); i++) {
		auto gp = GamepadDevice::GetGamepad(i);
		if (gp && gp->maple_port() == port)
			return gp;
	}
	return nullptr;
}

// ========================= Persistent palette override =========================
// When set, re-applied every frame by applyPaletteOverrides() called from
// serverPublish. Survives game reloads because we write after the game does.
struct PaletteOverride {
	bool active = false;
	// PVR palette start index (0-1023) and ARGB4444 colors
	int startIndex = 0;
	std::vector<u16> colors;
};
static std::mutex _palOverrideMutex;
static std::vector<PaletteOverride> _palOverrides;

// Reserved for future dynamic palette targeting
} // close namespace temporarily for extern
extern uint64_t g_activePalBanks;
namespace maplecast_control_ws {

void applyPaletteOverrides()
{
	std::lock_guard<std::mutex> lock(_palOverrideMutex);
	if (_palOverrides.empty()) return;
	for (auto& ov : _palOverrides) {
		if (!ov.active) continue;
		for (int i = 0; i < (int)ov.colors.size(); i++) {
			u32 addr = PALETTE_RAM_START_addr + (ov.startIndex + i) * 4;
			pvr_WriteReg(addr, ov.colors[i]);
		}
	}
	// Force the PVR palette dirty page to ship every frame. The diff
	// scan compares PVR regs against a shadow copy; after the first
	// frame, shadow matches our override and the page stops shipping.
	// Toggle the last byte of the palette page (entry 1023, unused by
	// MVC2 which only uses banks 16-56) to ensure shadow != current.
	static u32 _tick = 0;
	pvr_WriteReg(PALETTE_RAM_START_addr + 1023 * 4, ++_tick & 0xF);
}

// ========================= Pending mapping detect state =========================
static ControlConnHdl _detectConn;
static std::string _detectReplyId;
static std::string _detectDcKey;
static std::atomic<bool> _detectPending{false};

static void onInputDetected(u32 code, bool analog, bool positive)
{
	printf("[control-ws] mapping_detected: code=%u analog=%d positive=%d key=%s\n",
	       code, analog, positive, _detectDcKey.c_str());
	if (!_detectPending.load()) return;
	_detectPending.store(false);
	json data = {{"code", (int)code}, {"analog", analog}, {"positive", positive}, {"dc_key", _detectDcKey}};
	sendJson(_detectConn, json{
		{"ok", true}, {"cmd", "mapping_detected"}, {"reply_id", _detectReplyId}, {"data", data},
	});
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
	// !!! KNOWN BROKEN — DO NOT TRUST THIS PATH IN PRODUCTION !!!
	//
	// dc_loadstate() goes through Emulator::loadstate() which calls
	// dc_deserialize, getSh4Executor()->ResetCache(), and triggers an
	// EventManager Event::LoadState. Empirically (verified on the
	// nobd.net VPS deploy 2026-04-08), this kills the Flycast-emu
	// thread without an exception log — the thread exits cleanly
	// because some downstream code transitions Emulator::state away
	// from Running. After that the SH4 dynarec is dead, no new TA
	// frames are produced, and (because the render thread is the one
	// that drains the control WS command queue) every subsequent
	// control WS command times out.
	//
	// ARCHITECTURE.md "Other hard-learned lessons" explicitly says:
	// "NEVER use emu.loadstate() for live resync. Corrupts scheduler/
	// DMA/interrupt state → SIGSEGV after ~1000 frames. Use direct
	// memcpy of RAM/VRAM/ARAM instead." Same root cause we're hitting.
	//
	// The proper fix is to parse the savestate file ourselves into a
	// Deserializer, walk it for the RAM/VRAM/ARAM/PVR sections, and
	// memcpy them into the live arrays without going through the full
	// Emulator::loadstate state-machine bounce. That's a Phase A.2
	// follow-up tracked in WORKSTREAM-OVERLORD §5 "Pitfalls".
	//
	// Until then we hard-fail this command rather than silently kill
	// the SH4 thread.
	{
		(void)cmd; // suppress unused-warning while we no-op
		sendJson(cmd.conn, errReply(cmd, "savestate_load",
			"savestate_load is temporarily disabled — see WORKSTREAM-OVERLORD Phase A.2"));
		return;
	}

	// Unreachable below — kept for the eventual safe-load implementation.
	#if 0
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
	#endif // disabled until safe-load is implemented
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

static void executeOpenControls(const Command& cmd)
{
	// Open the flycast native settings GUI directly (bypasses the
	// mirror client's HTML-page redirect in gui_open_settings).
	gui_setState(GuiState::Settings);
	sendJson(cmd.conn, okReply(cmd, "open_controls"));
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
			case CmdType::OpenControls:   executeOpenControls(cmd); break;
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
	} else if (cmdName == "open_controls") {
		cmd.type = CmdType::OpenControls;
	} else if (cmdName == "status") {
		json data = {
			{"slot", config::SavestateSlot.get()},
		};
		auto vs = maplecast_mirror::getClientStats();
		data["frame"] = vs.frameCount;
		data["wsConnected"] = vs.wsConnected;
		data["packetsReceived"] = vs.packetsReceived;
		data["bytesReceived"] = vs.bytesReceived;
		data["isClient"] = maplecast_mirror::isClient();
		sendJson(hdl, json{
			{"ok", true}, {"cmd", "status"}, {"reply_id", reply_id}, {"data", data},
		});
		return;

	} else if (cmdName == "config_get") {
		// Return all mirror-client-relevant config values
		json data = {
			{"RenderResolution", config::RenderResolution.get()},
			{"TextureFiltering", config::TextureFiltering.get()},
			{"AnisotropicFiltering", config::AnisotropicFiltering.get()},
			{"PerPixelLayers", config::PerPixelLayers.get()},
			{"Fog", (bool)config::Fog},
			{"ModifierVolumes", (bool)config::ModifierVolumes},
			{"UseMipmaps", (bool)config::UseMipmaps},
			{"Widescreen", (bool)config::Widescreen},
			{"LinearInterpolation", (bool)config::LinearInterpolation},
			{"ShowFPS", (bool)config::ShowFPS},
			{"AudioVolume", config::AudioVolume.get()},
		};
		sendJson(hdl, json{
			{"ok", true}, {"cmd", "config_get"}, {"reply_id", reply_id}, {"data", data},
		});
		return;

	} else if (cmdName == "config_set") {
		// Set a single config value: {"cmd":"config_set","key":"RenderResolution","value":1920}
		if (!parsed.contains("key") || !parsed["key"].is_string()) {
			sendJson(hdl, errReplyImmediate(reply_id, "config_set", "missing 'key'"));
			return;
		}
		std::string key = parsed["key"].get<std::string>();
		if (!parsed.contains("value")) {
			sendJson(hdl, errReplyImmediate(reply_id, "config_set", "missing 'value'"));
			return;
		}
		auto& val = parsed["value"];
		bool ok = true;

		if      (key == "RenderResolution" && val.is_number()) config::RenderResolution = val.get<int>();
		else if (key == "TextureFiltering" && val.is_number()) config::TextureFiltering.set(val.get<int>());
		else if (key == "AnisotropicFiltering" && val.is_number()) config::AnisotropicFiltering.set(val.get<int>());
		else if (key == "PerPixelLayers" && val.is_number()) config::PerPixelLayers.set(val.get<int>());
		else if (key == "Fog" && val.is_boolean()) config::Fog.set(val.get<bool>());
		else if (key == "ModifierVolumes" && val.is_boolean()) config::ModifierVolumes.set(val.get<bool>());
		else if (key == "UseMipmaps" && val.is_boolean()) config::UseMipmaps.set(val.get<bool>());
		else if (key == "Widescreen" && val.is_boolean()) config::Widescreen.set(val.get<bool>());
		else if (key == "LinearInterpolation" && val.is_boolean()) config::LinearInterpolation.set(val.get<bool>());
		else if (key == "ShowFPS" && val.is_boolean()) config::ShowFPS.set(val.get<bool>());
		else if (key == "AudioVolume" && val.is_number()) config::AudioVolume.set(val.get<int>());
		else if (key == "LatchPolicy" && val.is_string() && parsed.contains("slot")) {
			int slot = parsed["slot"].get<int>();
			if (slot >= 0 && slot <= 1) {
				auto p = val.get<std::string>() == "ConsistencyFirst"
					? maplecast_input::LatchPolicy::ConsistencyFirst
					: maplecast_input::LatchPolicy::LatencyFirst;
				maplecast_input::setLatchPolicy(slot, p);
			} else ok = false;
		}
		else if (key == "GuardUs" && val.is_number()) maplecast_input::setGuardUs(val.get<int64_t>());
		else { ok = false; }

		if (ok) {
			sendJson(hdl, json{
				{"ok", true}, {"cmd", "config_set"}, {"reply_id", reply_id},
				{"data", {{"key", key}}},
			});
		} else {
			sendJson(hdl, errReplyImmediate(reply_id, "config_set",
				"unknown key or type mismatch: " + key));
		}
		return;

	} else if (cmdName == "palette_write") {
		// Write palette data. Two modes:
		//   PVR mode (default): writes to PVR palette RAM (immediate, may flicker)
		//   RAM mode ("ram_offset" set): writes to DC RAM source data (no flicker)
		// "persist":true re-applies every frame via applyPaletteOverrides().
		bool persist = parsed.value("persist", false);

		// PVR palette write. Takes "index" (0-1023) and "colors" array
		// of ARGB4444 u16 values. If persist=true, re-applied every frame.
		int startIdx = parsed.value("index", 0);
		if (!parsed.contains("colors") || !parsed["colors"].is_array()) {
			sendJson(hdl, errReplyImmediate(reply_id, "palette_write", "need 'colors' array"));
			return;
		}
		auto& colors = parsed["colors"];
		int count = (int)colors.size();
		if (startIdx < 0 || startIdx + count > 1024) {
			sendJson(hdl, errReplyImmediate(reply_id, "palette_write", "index+count out of range"));
			return;
		}
		// Immediate write
		std::vector<u16> colorVec;
		for (int i = 0; i < count; i++) {
			u16 c16 = colors[i].get<int>() & 0xFFFF;
			pvr_WriteReg(PALETTE_RAM_START_addr + (startIdx + i) * 4, c16);
			colorVec.push_back(c16);
		}
		if (persist) {
			std::lock_guard<std::mutex> lock(_palOverrideMutex);
			// Upsert: replace existing override for the same startIndex
			bool replaced = false;
			for (auto& existing : _palOverrides) {
				if (existing.startIndex == startIdx) {
					existing.colors = std::move(colorVec);
					existing.active = true;
					replaced = true;
					break;
				}
			}
			if (!replaced) {
				PaletteOverride ov;
				ov.active = true;
				ov.startIndex = startIdx;
				ov.colors = std::move(colorVec);
				_palOverrides.push_back(std::move(ov));
			}
		}
		sendJson(hdl, json{
			{"ok", true}, {"cmd", "palette_write"}, {"reply_id", reply_id},
			{"data", {{"index", startIdx}, {"count", count}, {"persist", persist}}},
		});
		return;

	} else if (cmdName == "palette_clear") {
		{
			std::lock_guard<std::mutex> lock(_palOverrideMutex);
			_palOverrides.clear();
		}
		sendJson(hdl, json{
			{"ok", true}, {"cmd", "palette_clear"}, {"reply_id", reply_id}, {"data", json::object()},
		});
		return;

	} else if (cmdName == "client_palette_set") {
		// Client-side palette override — applied locally before each render.
		int startIdx = parsed.value("index", 0);
		if (!parsed.contains("colors") || !parsed["colors"].is_array()) {
			sendJson(hdl, errReplyImmediate(reply_id, "client_palette_set", "need 'colors'"));
			return;
		}
		auto& colors = parsed["colors"];
		std::vector<u16> colorVec;
		for (auto& c : colors)
			colorVec.push_back(c.get<int>() & 0xFFFF);
		maplecast_palette::setOverride(startIdx, colorVec.data(), (int)colorVec.size());
		sendJson(hdl, json{
			{"ok", true}, {"cmd", "client_palette_set"}, {"reply_id", reply_id},
			{"data", {{"index", startIdx}, {"count", colorVec.size()}}},
		});
		return;

	} else if (cmdName == "client_palette_clear") {
		maplecast_palette::clearOverrides();
		sendJson(hdl, json{
			{"ok", true}, {"cmd", "client_palette_clear"}, {"reply_id", reply_id},
			{"data", json::object()},
		});
		return;

	} else if (cmdName == "palette_probe") {
		// Auto-discover which PVR palette bank a character slot uses.
		// Binary search: write a marker to half the banks, wait a frame,
		// check which half the client reports as changed. 6 iterations
		// to narrow from 64 banks to 1 exact bank.
		//
		// Since the headless server PVR palette is empty, we can probe
		// by writing to candidate banks and seeing which one produces
		// visible change on the viewer. But we can't see from the server.
		//
		// Alternative approach: write a unique color to each of the 64
		// banks in ONE shot. The client/viewer sees which color each
		// character renders as. Return the mapping.
		//
		// For now, the command just writes unique-per-bank colors and
		// returns the mapping. The caller (HTML page) observes the result.
		//
		// Usage: call palette_probe, then visually identify which bank
		// each character uses from the displayed colors.

		// Write unique hue per bank
		{
			std::lock_guard<std::mutex> lock(_palOverrideMutex);
			_palOverrides.clear();
		}
		json bankColors = json::array();
		for (int bank = 0; bank < 64; bank++) {
			int hue = bank * 4;
			int r, g, b;
			if (hue < 64)       { r = 15; g = hue*15/64; b = 0; }
			else if (hue < 128) { r = 15-(hue-64)*15/64; g = 15; b = 0; }
			else if (hue < 192) { r = 0; g = 15; b = (hue-128)*15/64; }
			else                { r = (hue-192)*15/64; g = 0; b = 15; }

			std::vector<u16> colors;
			colors.push_back(0x0000);
			for (int i = 1; i < 16; i++) {
				int s = std::min(15, i);
				colors.push_back(0xF000 | ((std::min(15,r*s/15))<<8) |
					((std::min(15,g*s/15))<<4) | std::min(15,b*s/15));
			}

			// Write immediately
			for (int i = 0; i < 16; i++)
				pvr_WriteReg(PALETTE_RAM_START_addr + (bank*16+i)*4, colors[i]);

			// Also persist
			PaletteOverride ov;
			ov.active = true;
			ov.startIndex = bank * 16;
			ov.colors = std::move(colors);
			{
				std::lock_guard<std::mutex> lock(_palOverrideMutex);
				_palOverrides.push_back(std::move(ov));
			}

			bankColors.push_back({{"bank", bank}, {"r", r}, {"g", g}, {"b", b}});
		}

		sendJson(hdl, json{
			{"ok", true}, {"cmd", "palette_probe"}, {"reply_id", reply_id},
			{"data", {
				{"bankColors", bankColors},
				{"hint", "Each bank has a unique color. Identify which color each character shows to find their bank."},
			}},
		});
		return;

	} else if (cmdName == "ram_read") {
		int offset = parsed.value("offset", -1);
		int size = parsed.value("size", 0);
		if (offset < 0 || size <= 0 || offset + size > 16 * 1024 * 1024) {
			sendJson(hdl, errReplyImmediate(reply_id, "ram_read", "bad offset/size"));
			return;
		}
		// Return as hex string
		std::string hex;
		hex.reserve(size * 2);
		for (int i = 0; i < size; i++) {
			char buf[3];
			snprintf(buf, sizeof(buf), "%02x", (unsigned)::mem_b[offset + i]);
			hex += buf;
		}
		sendJson(hdl, json{
			{"ok", true}, {"cmd", "ram_read"}, {"reply_id", reply_id},
			{"data", {{"offset", offset}, {"size", size}, {"hex", hex}}},
		});
		return;

	} else if (cmdName == "match_info") {
		// Read all 6 character structs to get the current match state.
		// Character struct: +0x000=active, +0x001=character_id, +0x52D=palette
		struct CharInfo {
			int slot; u8 active; u8 charId; u8 palette;
			float posX, posY; u8 health;
		};
		static const u32 bases[] = {
			0x268340, 0x2688E4, 0x268E88,  // P1C1, P2C1, P1C2
			0x26942C, 0x2699D0, 0x269F74,  // P2C2, P1C3, P2C3
		};
		static const char* slotNames[] = {
			"P1C1", "P2C1", "P1C2", "P2C2", "P1C3", "P2C3"
		};
		// Character name table
		static const char* charNames[] = {
			"Ryu","Zangief","Guile","Morrigan","Anakaris","Strider",
			"Cyclops","Wolverine","Psylocke","Iceman","Rogue","Capt America",
			"Spider-Man","Hulk","Venom","Dr Doom","Tron Bonne","Jill",
			"Hayato","Ruby Heart","SonSon","Amingo","Marrow","Cable",
			"Abyss1","Abyss2","Abyss3","Chun-Li","Mega Man","Roll",
			"Akuma","BB Hood","Felicia","Charlie","Sakura","Dan",
			"Cammy","Dhalsim","M.Bison","Ken","Gambit","Juggernaut",
			"Storm","Sabretooth","Magneto","Shuma","War Machine",
			"Silver Samurai","Omega Red","Spiral","Colossus","Iron Man",
			"Sentinel","Blackheart","Thanos","Jin","Capt Commando",
			"Bone Wolverine","Servbot"
		};

		json chars = json::array();
		for (int i = 0; i < 6; i++) {
			u32 base = bases[i];
			u8 active = ::mem_b[base + 0x000];
			u8 charId = ::mem_b[base + 0x001];
			u8 palette = ::mem_b[base + 0x52D];
			u8 health = ::mem_b[base + 0x420];
			float posX, posY;
			memcpy(&posX, &::mem_b[base + 0x034], 4);
			memcpy(&posY, &::mem_b[base + 0x038], 4);

			const char* name = (charId < 59) ? charNames[charId] : "Unknown";
			chars.push_back({
				{"slot", slotNames[i]},
				{"active", active},
				{"charId", charId},
				{"charIdHex", json(std::string("0x") + std::string(1, "0123456789ABCDEF"[charId>>4]) + std::string(1, "0123456789ABCDEF"[charId&0xF]))},
				{"name", name},
				{"palette", palette},
				{"health", health},
				{"posX", posX},
				{"posY", posY},
			});
		}
		sendJson(hdl, json{
			{"ok", true}, {"cmd", "match_info"}, {"reply_id", reply_id},
			{"data", {{"characters", chars}}},
		});
		return;

	} else if (cmdName == "palette_find") {
		// Search DC RAM for a byte pattern (palette signature).
		// Returns the offset where the pattern was found.
		// Used to dynamically locate character palettes after load.
		if (!parsed.contains("hex") || !parsed["hex"].is_string()) {
			sendJson(hdl, errReplyImmediate(reply_id, "palette_find", "need 'hex' pattern"));
			return;
		}
		std::string hex = parsed["hex"].get<std::string>();
		std::vector<u8> pattern;
		for (size_t i = 0; i + 1 < hex.size(); i += 2)
			pattern.push_back((u8)std::strtol(hex.substr(i, 2).c_str(), nullptr, 16));
		if (pattern.empty()) {
			sendJson(hdl, errReplyImmediate(reply_id, "palette_find", "empty pattern"));
			return;
		}
		// Search 16MB DC RAM
		json matches = json::array();
		const u8* ram = &::mem_b[0];
		size_t ramSize = 16 * 1024 * 1024;
		for (size_t i = 0; i + pattern.size() <= ramSize; i++) {
			if (memcmp(ram + i, pattern.data(), pattern.size()) == 0) {
				matches.push_back((int)i);
				if (matches.size() >= 10) break;  // cap results
			}
		}
		sendJson(hdl, json{
			{"ok", true}, {"cmd", "palette_find"}, {"reply_id", reply_id},
			{"data", {{"pattern", hex}, {"matches", matches}}},
		});
		return;

	} else if (cmdName == "palette_read") {
		// Read current PVR palette RAM entries
		int startIdx = parsed.value("index", 0);
		int count = parsed.value("count", 16);
		if (startIdx < 0 || startIdx + count > 1024) {
			sendJson(hdl, errReplyImmediate(reply_id, "palette_read", "out of range"));
			return;
		}
		json colors = json::array();
		for (int i = 0; i < count; i++) {
			u32 addr = PALETTE_RAM_START_addr + (startIdx + i) * 4;
			u32 val = pvr_ReadReg(addr);
			colors.push_back(val & 0xFFFF);
		}
		sendJson(hdl, json{
			{"ok", true}, {"cmd", "palette_read"}, {"reply_id", reply_id},
			{"data", {{"index", startIdx}, {"count", count}, {"colors", colors}}},
		});
		return;

	} else if (cmdName == "ram_write") {
		// Write bytes to DC RAM at a specific offset.
		// Used for patching palette source data in main memory.
		int offset = parsed.value("offset", -1);
		if (offset < 0 || !parsed.contains("hex")) {
			sendJson(hdl, errReplyImmediate(reply_id, "ram_write", "need 'offset' and 'hex'"));
			return;
		}
		std::string hex = parsed["hex"].get<std::string>();
		std::vector<u8> bytes;
		for (size_t i = 0; i + 1 < hex.size(); i += 2) {
			bytes.push_back((u8)std::strtol(hex.substr(i, 2).c_str(), nullptr, 16));
		}
		if (offset + (int)bytes.size() > 16 * 1024 * 1024) {
			sendJson(hdl, errReplyImmediate(reply_id, "ram_write", "exceeds 16MB DC RAM"));
			return;
		}
		memcpy(&::mem_b[offset], bytes.data(), bytes.size());
		sendJson(hdl, json{
			{"ok", true}, {"cmd", "ram_write"}, {"reply_id", reply_id},
			{"data", {{"offset", offset}, {"size", bytes.size()}}},
		});
		return;

	} else if (cmdName == "mapping_get") {
		// Return current button mapping for the first gamepad
		auto gp = findGamepadOnPort(0);
		json data = json::object();
		if (gp && gp->get_input_mapping()) {
			auto mapper = gp->get_input_mapping();
			data["device"] = gp->name();
			data["api"] = gp->api_name();
			// Map of DC key name → SDL button code
			struct { const char* name; DreamcastKey key; } dcKeys[] = {
				{"A", DC_BTN_A}, {"B", DC_BTN_B}, {"C", DC_BTN_C},
				{"X", DC_BTN_X}, {"Y", DC_BTN_Y}, {"Z", DC_BTN_Z},
				{"D", DC_BTN_D}, {"Start", DC_BTN_START},
				{"DPad_Up", DC_DPAD_UP}, {"DPad_Down", DC_DPAD_DOWN},
				{"DPad_Left", DC_DPAD_LEFT}, {"DPad_Right", DC_DPAD_RIGHT},
			};
			json buttons = json::object();
			for (auto& dk : dcKeys) {
				u32 code = mapper->get_button_code(0, dk.key);
				buttons[dk.name] = (code != InputMapping::InputDef::INVALID_CODE) ? (int)code : -1;
			}
			data["buttons"] = buttons;
			// Axes
			json axes = json::object();
			auto [ltCode, ltInv] = mapper->get_axis_code(0, DC_AXIS_LT);
			auto [rtCode, rtInv] = mapper->get_axis_code(0, DC_AXIS_RT);
			axes["LT"] = (ltCode != InputMapping::InputDef::INVALID_CODE) ? (int)ltCode : -1;
			axes["RT"] = (rtCode != InputMapping::InputDef::INVALID_CODE) ? (int)rtCode : -1;
			data["axes"] = axes;
		} else {
			data["error"] = "no gamepad found";
		}
		sendJson(hdl, json{
			{"ok", true}, {"cmd", "mapping_get"}, {"reply_id", reply_id}, {"data", data},
		});
		return;

	} else if (cmdName == "mapping_detect") {
		int gpCount = GamepadDevice::GetGamepadCount();
		auto gp = findGamepadOnPort(0);
		if (!gp) {
			sendJson(hdl, errReplyImmediate(reply_id, "mapping_detect",
				"no gamepad (count=" + std::to_string(gpCount) + ")"));
			return;
		}
		_detectConn = hdl;
		_detectReplyId = reply_id;
		_detectDcKey = parsed.value("dc_key", "");
		_detectPending.store(true);
		gp->detectInput(false, onInputDetected);
		printf("[control-ws] mapping_detect: listening for %s on '%s' (port=%d, count=%d)\n",
		       _detectDcKey.c_str(), gp->name().c_str(), gp->maple_port(), gpCount);
		sendJson(hdl, json{
			{"ok", true}, {"cmd", "mapping_detect"}, {"reply_id", reply_id},
			{"data", {{"listening", true}, {"dc_key", _detectDcKey},
			          {"device", gp->name()}, {"port", gp->maple_port()}}},
		});
		return;

	} else if (cmdName == "mapping_set") {
		// Set a button mapping: {"cmd":"mapping_set","dc_key":"A","code":3}
		auto gp = findGamepadOnPort(0);
		if (!gp || !gp->get_input_mapping()) {
			sendJson(hdl, errReplyImmediate(reply_id, "mapping_set", "no gamepad"));
			return;
		}
		std::string dcKeyName = parsed.value("dc_key", "");
		int code = parsed.value("code", -1);
		if (dcKeyName.empty() || code < 0) {
			sendJson(hdl, errReplyImmediate(reply_id, "mapping_set", "need dc_key and code"));
			return;
		}
		// Resolve DC key name to enum
		DreamcastKey dk = EMU_BTN_NONE;
		struct { const char* n; DreamcastKey k; } map[] = {
			{"A", DC_BTN_A}, {"B", DC_BTN_B}, {"C", DC_BTN_C},
			{"X", DC_BTN_X}, {"Y", DC_BTN_Y}, {"Z", DC_BTN_Z},
			{"D", DC_BTN_D}, {"Start", DC_BTN_START},
			{"DPad_Up", DC_DPAD_UP}, {"DPad_Down", DC_DPAD_DOWN},
			{"DPad_Left", DC_DPAD_LEFT}, {"DPad_Right", DC_DPAD_RIGHT},
			{"LT", DC_AXIS_LT}, {"RT", DC_AXIS_RT},
		};
		for (auto& m : map) { if (dcKeyName == m.n) { dk = m.k; break; } }
		if (dk == EMU_BTN_NONE) {
			sendJson(hdl, errReplyImmediate(reply_id, "mapping_set", "unknown dc_key: " + dcKeyName));
			return;
		}
		auto mapper = gp->get_input_mapping();
		mapper->set_button(0, dk, InputMapping::ButtonCombo{
			InputMapping::InputSet{InputMapping::InputDef::from_button((u32)code)}, false
		});
		gp->save_mapping();
		sendJson(hdl, json{
			{"ok", true}, {"cmd", "mapping_set"}, {"reply_id", reply_id},
			{"data", {{"dc_key", dcKeyName}, {"code", code}}},
		});
		return;

	} else if (cmdName == "mapping_cancel") {
		auto gp = findGamepadOnPort(0);
		if (gp) gp->cancel_detect_input();
		sendJson(hdl, json{
			{"ok", true}, {"cmd", "mapping_cancel"}, {"reply_id", reply_id}, {"data", json::object()},
		});
		return;

	} else if (cmdName == "telemetry") {
		auto vs = maplecast_mirror::getClientStats();
		auto as = maplecast_audio_client::getStats();

		// Input sink stats (client-side)
		auto is = maplecast_input_sink::getStats();

		// Per-player info — on the client, only the local player is
		// meaningful. Server-side PlayerInfo is empty because
		// maplecast_input::init() wasn't called.
		json players = json::array();
		if (maplecast_mirror::isClient()) {
			// Client-side: show input sink stats as the local player
			auto gp = findGamepadOnPort(0);
			players.push_back({
				{"slot", 0},
				{"connected", maplecast_input_sink::active()},
				{"name", gp ? gp->name() : "unknown"},
				{"device", gp ? gp->api_name() : ""},
				{"pps", is.sendRateHz},
				{"buttonChanges", is.buttonChanges},
				{"triggerChanges", is.triggerChanges},
				{"packetsSent", is.packetsSent},
				{"latchPolicy", "N/A (client)"},
				{"guardHits", 0},
			});
		} else {
			// Server-side: use the real PlayerInfo
			for (int s = 0; s < 2; s++) {
				const auto& p = maplecast_input::getPlayer(s);
				players.push_back({
					{"slot", s},
					{"connected", p.connected},
					{"name", p.name},
					{"device", p.device},
					{"buttons", p.buttons},
					{"lt", p.lt}, {"rt", p.rt},
					{"pps", p.packetsPerSec},
					{"cps", p.changesPerSec},
					{"avgE2eMs", p.avgE2eUs / 1000.0},
					{"avgJitterMs", p.avgJitterUs / 1000.0},
					{"rttMs", p.rttMs},
					{"latchPolicy", maplecast_input::getLatchPolicy(s) ==
						maplecast_input::LatchPolicy::ConsistencyFirst
						? "ConsistencyFirst" : "LatencyFirst"},
					{"guardHits", p.guardHits},
				});
			}
		}

		json data = {
			// Video
			{"frame", vs.frameCount},
			{"wsConnected", vs.wsConnected},
			{"videoPackets", vs.packetsReceived},
			{"videoBytes", vs.bytesReceived},
			{"arrivalEmaMs", vs.arrivalEmaUs / 1000.0},
			{"arrivalMaxMs", vs.arrivalMaxUs / 1000.0},
			{"decodeLastMs", vs.lastDecodeUs / 1000.0},
			{"decodeEmaMs", vs.decodeEmaUs / 1000.0},
			{"lastTaSize", vs.lastTaSize},
			{"lastDirtyPages", vs.lastDirtyPages},
			{"lastVramDirty", vs.lastVramDirty},
			// Audio
			{"audioConnected", as.connected},
			{"audioPackets", as.packetsReceived},
			{"audioDropped", as.packetsDropped},
			{"audioBytes", as.bytesReceived},
			{"audioPushFails", as.pushFailures},
			{"audioArrivalEmaMs", as.arrivalIntervalEmaUs / 1000.0},
			{"audioArrivalMaxMs", as.arrivalIntervalMaxUs / 1000.0},
			{"audioSeq", as.lastSeq},
			// Input
			{"guardUs", maplecast_input::getGuardUs()},
			{"players", players},
			// Input sink (client-side)
			{"inputSinkActive", maplecast_input_sink::active()},
			{"inputPacketsSent", is.packetsSent},
			{"inputSendRateHz", is.sendRateHz},
			{"inputButtonChanges", is.buttonChanges},
			{"inputTriggerChanges", is.triggerChanges},
			// E2E latency (button press → visual change on screen)
			{"e2eLastMs", is.e2eLastMs},
			{"e2eEmaMs", is.e2eEmaMs},
			{"e2eMinMs", is.e2eMinMs},
			{"e2eMaxMs", is.e2eMaxMs},
			{"e2eProbes", is.e2eProbes},
			// Active palette banks (from TA probe in serverPublish)
			{"activePalBanks", (uint64_t)::g_activePalBanks},
			{"activePalBankCount", __builtin_popcountll(::g_activePalBanks)},
			{"serverFrame", maplecast_mirror::currentFrame()},
			// Raw input state for button tester
			{"kcode0", (unsigned)(kcode[0] & 0xFFFF)},
			{"kcode1", (unsigned)(kcode[1] & 0xFFFF)},
			{"lt0", (unsigned)(lt[0] >> 8)},
			{"rt0", (unsigned)(rt[0] >> 8)},
			{"lt1", (unsigned)(lt[1] >> 8)},
			{"rt1", (unsigned)(rt[1] >> 8)},
		};

		// Append game state if available (client receives GSTA from server)
		maplecast_gamestate::GameState gs;
		if (maplecast_mirror::getClientGameState(gs)) {
			json game;
			game["in_match"] = (bool)gs.in_match;
			game["timer"] = gs.game_timer;
			game["stage"] = gs.stage_id;
			game["frame"] = gs.frame_counter;
			game["p1_combo"] = gs.p1_combo;
			game["p2_combo"] = gs.p2_combo;
			game["p1_meter"] = gs.p1_meter_level;
			game["p2_meter"] = gs.p2_meter_level;
			json chars = json::array();
			for (int i = 0; i < 6; i++) {
				chars.push_back({
					{"active", (bool)gs.chars[i].active},
					{"id", gs.chars[i].character_id},
					{"health", gs.chars[i].health},
					{"red_health", gs.chars[i].red_health},
					{"pos_x", gs.chars[i].pos_x},
					{"pos_y", gs.chars[i].pos_y},
					{"anim_state", gs.chars[i].animation_state},
				});
			}
			game["chars"] = chars;
			data["game"] = game;
		}
		sendJson(hdl, json{
			{"ok", true}, {"cmd", "telemetry"}, {"reply_id", reply_id}, {"data", data},
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

// Global palette functions callable from maplecast_ws_server.cpp
// (king.html skin picker sends commands through the relay WS, which
// arrives at the mirror WS handler, which calls these)
void maplecast_palette_write(int startIdx, const std::vector<u16>& colors, bool persist)
{
	for (int i = 0; i < (int)colors.size(); i++) {
		pvr_WriteReg(PALETTE_RAM_START_addr + (startIdx + i) * 4, colors[i]);
	}
	if (persist) {
		using namespace maplecast_control_ws;
		std::lock_guard<std::mutex> lock(_palOverrideMutex);
		bool replaced = false;
		for (auto& existing : _palOverrides) {
			if (existing.startIndex == startIdx) {
				existing.colors = colors;
				existing.active = true;
				replaced = true;
				break;
			}
		}
		if (!replaced) {
			PaletteOverride ov;
			ov.active = true;
			ov.startIndex = startIdx;
			ov.colors = colors;
			_palOverrides.push_back(std::move(ov));
		}
	}
}

void maplecast_palette_clear()
{
	using namespace maplecast_control_ws;
	std::lock_guard<std::mutex> lock(_palOverrideMutex);
	_palOverrides.clear();
}
