/*
	MapleCast Raw Input / XInput direct gamepad bypass — Windows.

	Equivalent of core/network/maplecast_evdev_input.cpp on Linux. Bypasses
	SDL's gamepad event queue (which adds ~1-3ms via its internal poll
	thread + event pump) by talking to the gamepad APIs directly:

	  • XInput fast-path  — Xbox-compatible controllers (>= 90% of pads).
	    Polled at 1 kHz on a THREAD_PRIORITY_TIME_CRITICAL thread.
	    XInputGetState() returns gamepad state in a single syscall, no
	    queue, no event pump.
	  • Raw Input fallback — for non-XInput sticks (older fightsticks,
	    DInput-only devices). Hidden message-only window with
	    RIDEV_INPUTSINK; HID reports parsed inline via HidP_*.

	Translates physical button/axis state -> DreamcastKey using flycast's
	own InputMapping (so the user's Settings -> Controls bindings still
	apply), then writes directly into maplecast_input_sink::_buttons +
	calls sendState(). Same downstream path as SDL — input sink can't
	tell which path produced the events.

	Enable: MAPLECAST_RAWINPUT=1 (auto-disabled on non-Windows)
	Or:     MAPLECAST_RAWINPUT=xinput  (XInput only — skip Raw Input)
	Or:     MAPLECAST_RAWINPUT=raw     (Raw Input only — skip XInput)

	The bypass calls into the same ButtonListener system (GamepadDevice::
	listenButtonsGlobal) that SDL fires, so input_sink::onButton receives
	events identically. SDL is left running for non-gamepad input (window,
	keyboard) — we just race it to the punch on gamepad events.
*/

#ifdef _WIN32

#include "rawinput_gamepad.h"
#include "../network/maplecast_input_sink.h"
#include "../input/gamepad.h"
#include "../input/gamepad_device.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#pragma comment(lib, "Xinput.lib")

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <direct.h>

// Globals owned by gamepad_device.cpp — analog trigger state per maple
// port. We write to these directly when the user maps A1/A2 to an axis
// DreamcastKey, because input_sink::onButton early-returns on
// non-bitmap keys (anything > DC_BTN_BITMAPPED_LAST = 0x20000), so
// fireButtonGlobal alone won't reach the wire for triggers.
extern u16 lt[4], rt[4];

namespace maplecast_rawinput {

static std::thread        _xinputThread;
static std::atomic<bool>  _run{false};
static std::atomic<bool>  _active{false};

// ── User-defined XInput → DreamcastKey mapping ───────────────────────
//
// The wizard (MAPLECAST_BUTTON_WIZARD=1) prompts the user to press each
// MVC2 button and records the XInput input that fired. The resulting
// mapping is saved to %USERPROFILE%\.maplecast\xinput-map.cfg and loaded
// at startup. When loaded, the XInput thread fires DreamcastKey directly
// via fireButtonGlobal — bypassing flycast's InputMapping entirely (which
// avoids issues with default-mapping bugs and lets the user pick exactly
// which physical button does what).
//
// If no mapping file exists, the XInput thread falls back to the
// "feed SDL joystick button index into gamepad_btn_input" path below
// (which uses flycast's InputMapping if SDL has detected a gamepad).
//
// File format (INI-ish):
//     [xinput-map]
//     LP=button:0x4000              # XINPUT_GAMEPAD_X
//     HP=trigger:left               # analog left trigger above threshold
//     DPAD_UP=button:0x0001
//     ...
struct XInputCapture {
	enum Type { NONE, BUTTON_BIT, TRIGGER_LT, TRIGGER_RT };
	Type type = NONE;
	WORD bit = 0;
	bool serialize(char* buf, size_t bufsz) const {
		switch (type) {
		case BUTTON_BIT:  return snprintf(buf, bufsz, "button:0x%04x", (unsigned)bit) > 0;
		case TRIGGER_LT:  return snprintf(buf, bufsz, "trigger:left")  > 0;
		case TRIGGER_RT:  return snprintf(buf, bufsz, "trigger:right") > 0;
		default:          return false;
		}
	}
	static XInputCapture parse(const std::string& s) {
		XInputCapture c;
		if (s.rfind("button:", 0) == 0) {
			c.type = BUTTON_BIT;
			c.bit = (WORD)std::strtoul(s.c_str() + 7, nullptr, 0);
		} else if (s == "trigger:left")  c.type = TRIGGER_LT;
		else if (s == "trigger:right")   c.type = TRIGGER_RT;
		return c;
	}
	bool operator==(const XInputCapture& o) const {
		if (type != o.type) return false;
		if (type == BUTTON_BIT) return bit == o.bit;
		return true;  // TRIGGER_LT/RT have no extra data
	}
	const char* humanName() const {
		switch (type) {
		case BUTTON_BIT: {
			static thread_local char buf[32];
			snprintf(buf, sizeof(buf), "button bit 0x%04x", (unsigned)bit);
			return buf;
		}
		case TRIGGER_LT: return "Left Trigger";
		case TRIGGER_RT: return "Right Trigger";
		default:         return "?";
		}
	}
};

// MVC2 button list — ordered for the wizard's user-prompt sequence.
// MVC2 uses 4 attacks (LP, HP, LK, HK) + 2 assists (A1, A2) — NOT the
// Street Fighter 6-button LP/MP/HP/LK/MK/HK layout. The DC mapping
// follows the standard arcade MVC2 → Dreamcast convention:
//   X = LP, Y = HP, LT = A1, A = LK, B = HK, RT = A2
struct Mvc2Slot {
	const char* name;        // short label used in mapping file
	const char* prompt;      // user-visible description
	DreamcastKey dcKey;      // what to emit on the wire
};
static const Mvc2Slot kSlots[] = {
	{ "LP",        "LIGHT PUNCH  (LP)",      DC_BTN_X },
	// MVC2 HP = DC_BTN_Y per the canonical mapping in
	// maplecast_evdev_input.cpp:128. Source: docs/WEBGPU-RENDERER.md.
	{ "HP",        "HEAVY PUNCH  (HP)",      DC_BTN_Y },
	{ "A1",        "ASSIST 1     (A1)",      DC_AXIS_LT },
	{ "LK",        "LIGHT KICK   (LK)",      DC_BTN_A },
	{ "HK",        "HEAVY KICK   (HK)",      DC_BTN_B },
	{ "A2",        "ASSIST 2     (A2)",      DC_AXIS_RT },
	{ "START",     "START button",           DC_BTN_START },
	{ "DPAD_UP",   "D-Pad UP",               DC_DPAD_UP },
	{ "DPAD_DOWN", "D-Pad DOWN",             DC_DPAD_DOWN },
	{ "DPAD_LEFT", "D-Pad LEFT",             DC_DPAD_LEFT },
	{ "DPAD_RIGHT","D-Pad RIGHT",            DC_DPAD_RIGHT },
};
static constexpr int kNumSlots = sizeof(kSlots) / sizeof(kSlots[0]);

// Per-slot user mapping. Indexed by kSlots position. Loaded from file or
// populated by the wizard.
static XInputCapture _userMap[kNumSlots];
static bool          _userMapLoaded = false;

// Default fallback table — used only when no user mapping file exists.
// The XInput thread falls back to the SDL InputMapping path via this.
struct XInputBindingFallback {
	WORD xinputBit;
	int  sdlJoyButton;
};
static const XInputBindingFallback kFallbackBindings[] = {
	{ XINPUT_GAMEPAD_A,              0 },
	{ XINPUT_GAMEPAD_B,              1 },
	{ XINPUT_GAMEPAD_X,              2 },
	{ XINPUT_GAMEPAD_Y,              3 },
	{ XINPUT_GAMEPAD_LEFT_SHOULDER,  4 },
	{ XINPUT_GAMEPAD_RIGHT_SHOULDER, 5 },
	{ XINPUT_GAMEPAD_BACK,           6 },
	{ XINPUT_GAMEPAD_START,          7 },
	{ XINPUT_GAMEPAD_LEFT_THUMB,     8 },
	{ XINPUT_GAMEPAD_RIGHT_THUMB,    9 },
	{ XINPUT_GAMEPAD_DPAD_UP,        0x10000 + 0 },
	{ XINPUT_GAMEPAD_DPAD_DOWN,      0x10000 + 1 },
	{ XINPUT_GAMEPAD_DPAD_LEFT,      0x10000 + 2 },
	{ XINPUT_GAMEPAD_DPAD_RIGHT,     0x10000 + 3 },
};

// Mapping file path: %USERPROFILE%\.maplecast\xinput-map.cfg
static std::string mappingFilePath()
{
	const char* home = std::getenv("USERPROFILE");
	if (!home || !*home) home = std::getenv("APPDATA");
	if (!home || !*home) return std::string();
	std::string dir = std::string(home) + "\\.maplecast";
	_mkdir(dir.c_str());   // ignore EEXIST
	return dir + "\\xinput-map.cfg";
}

static bool loadMapping()
{
	std::string path = mappingFilePath();
	if (path.empty()) return false;
	std::ifstream f(path);
	if (!f.is_open()) return false;

	std::map<std::string, std::string> kv;
	std::string line;
	while (std::getline(f, line)) {
		// Strip whitespace + comments
		auto hash = line.find('#');
		if (hash != std::string::npos) line.erase(hash);
		auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		std::string k = line.substr(0, eq);
		std::string v = line.substr(eq + 1);
		auto trim = [](std::string& s) {
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
			size_t i = 0;
			while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
			s.erase(0, i);
		};
		trim(k); trim(v);
		if (!k.empty() && !v.empty()) kv[k] = v;
	}

	int captured = 0;
	for (int i = 0; i < kNumSlots; i++) {
		auto it = kv.find(kSlots[i].name);
		if (it != kv.end()) {
			_userMap[i] = XInputCapture::parse(it->second);
			if (_userMap[i].type != XInputCapture::NONE) captured++;
		}
	}
	if (captured > 0) {
		_userMapLoaded = true;
		printf("[rawinput] Loaded %d/%d button mappings from %s\n",
			captured, kNumSlots, path.c_str());
	}
	return _userMapLoaded;
}

static bool saveMapping()
{
	std::string path = mappingFilePath();
	if (path.empty()) return false;
	std::ofstream f(path, std::ios::trunc);
	if (!f.is_open()) {
		printf("[rawinput] saveMapping: cannot open %s for write\n", path.c_str());
		return false;
	}
	f << "# MapleCast XInput button mapping for MVC2.\n";
	f << "# Generated by the wizard (MAPLECAST_BUTTON_WIZARD=1).\n";
	f << "# Format: SLOT=button:0xBIT  or  SLOT=trigger:{left,right}\n";
	f << "[xinput-map]\n";
	for (int i = 0; i < kNumSlots; i++) {
		char buf[64] = {0};
		if (_userMap[i].serialize(buf, sizeof(buf)))
			f << kSlots[i].name << "=" << buf << "\n";
	}
	printf("[rawinput] Saved button mapping to %s\n", path.c_str());
	return true;
}

// ── Wizard mode — interactive button mapping setup ────────────────────
//
// Triggered by MAPLECAST_BUTTON_WIZARD=1. Runs synchronously at startup,
// blocking until the user has mapped every MVC2 button. After saving,
// the program exits; the user relaunches without the env var to play.
static bool waitForRelease(DWORD slot)
{
	// Wait until controller state has no buttons pressed and triggers idle.
	for (int tries = 0; tries < 500; tries++) {  // up to 5 seconds
		XINPUT_STATE s{};
		if (XInputGetState(slot, &s) != ERROR_SUCCESS) return false;
		if (s.Gamepad.wButtons == 0
		 && s.Gamepad.bLeftTrigger < 30
		 && s.Gamepad.bRightTrigger < 30)
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

static bool captureOne(DWORD slot, XInputCapture& out)
{
	// Snapshot baseline state.
	XINPUT_STATE base{};
	if (XInputGetState(slot, &base) != ERROR_SUCCESS) return false;
	const WORD baseButtons = base.Gamepad.wButtons;

	// Poll until something deviates from baseline.
	while (true) {
		XINPUT_STATE s{};
		if (XInputGetState(slot, &s) != ERROR_SUCCESS) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		// New button bit?
		WORD pressed = s.Gamepad.wButtons & ~baseButtons;
		if (pressed) {
			// Pick the lowest set bit.
			WORD bit = pressed & (~pressed + 1);
			out.type = XInputCapture::BUTTON_BIT;
			out.bit = bit;
			return true;
		}
		// Trigger over threshold?
		if (s.Gamepad.bLeftTrigger >= 100) {
			out.type = XInputCapture::TRIGGER_LT;
			return true;
		}
		if (s.Gamepad.bRightTrigger >= 100) {
			out.type = XInputCapture::TRIGGER_RT;
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

static const char* xinputBitName(WORD bit)
{
	switch (bit) {
	case XINPUT_GAMEPAD_DPAD_UP:        return "D-Pad Up";
	case XINPUT_GAMEPAD_DPAD_DOWN:      return "D-Pad Down";
	case XINPUT_GAMEPAD_DPAD_LEFT:      return "D-Pad Left";
	case XINPUT_GAMEPAD_DPAD_RIGHT:     return "D-Pad Right";
	case XINPUT_GAMEPAD_START:          return "Start";
	case XINPUT_GAMEPAD_BACK:           return "Back";
	case XINPUT_GAMEPAD_LEFT_THUMB:     return "Left Thumb";
	case XINPUT_GAMEPAD_RIGHT_THUMB:    return "Right Thumb";
	case XINPUT_GAMEPAD_LEFT_SHOULDER:  return "Left Bumper";
	case XINPUT_GAMEPAD_RIGHT_SHOULDER: return "Right Bumper";
	case XINPUT_GAMEPAD_A:              return "A";
	case XINPUT_GAMEPAD_B:              return "B";
	case XINPUT_GAMEPAD_X:              return "X";
	case XINPUT_GAMEPAD_Y:              return "Y";
	default:                            return "Unknown";
	}
}

static void runWizard()
{
	printf("\n");
	printf("==========================================================\n");
	printf("    MapleCast Button Mapping Wizard\n");
	printf("==========================================================\n");
	printf("Press each button when prompted. Wait between presses for\n");
	printf("the wizard to detect release before moving to the next.\n");
	printf("\n");
	printf("If you make a mistake, close the program (Ctrl+C in this\n");
	printf("console window) and re-run. Mappings save only at the end.\n");
	printf("\n");

	// Find which XInput slot has a connected controller.
	DWORD slot = 0xFFFFFFFF;
	for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
		XINPUT_STATE s{};
		if (XInputGetState(i, &s) == ERROR_SUCCESS) { slot = i; break; }
	}
	if (slot == 0xFFFFFFFF) {
		printf("[wizard] No XInput controller detected. Plug one in and re-run.\n");
		fflush(stdout);
		return;
	}
	printf("[wizard] Controller detected on slot %lu. Starting in 2s...\n", slot);
	fflush(stdout);
	std::this_thread::sleep_for(std::chrono::seconds(2));

	for (int i = 0; i < kNumSlots; i++) {
		// Wait for previous press to release.
		waitForRelease(slot);

		// Inner retry loop — re-prompt if the captured input is already
		// bound to an earlier slot. Without this, two slots can collide
		// on the same physical button and one of them is silently a
		// duplicate (e.g. press X for LP, press X for HP -> only LP works
		// because both write bit 0x400).
		while (true) {
			printf(">> Press %s ... ", kSlots[i].prompt);
			fflush(stdout);

			XInputCapture cap;
			if (!captureOne(slot, cap)) {
				printf("FAILED — controller disconnected?\n");
				return;
			}

			// Duplicate check against earlier slots.
			int dupIdx = -1;
			for (int j = 0; j < i; j++) {
				if (_userMap[j].type != XInputCapture::NONE && _userMap[j] == cap) {
					dupIdx = j;
					break;
				}
			}
			if (dupIdx >= 0) {
				const char* desc = "?";
				switch (cap.type) {
				case XInputCapture::BUTTON_BIT:  desc = xinputBitName(cap.bit); break;
				case XInputCapture::TRIGGER_LT:  desc = "Left Trigger";          break;
				case XInputCapture::TRIGGER_RT:  desc = "Right Trigger";         break;
				default: break;
				}
				printf("ALREADY USED for %s (%s) — release and try a DIFFERENT button.\n",
					kSlots[dupIdx].name, desc);
				fflush(stdout);
				waitForRelease(slot);
				continue;
			}

			const char* desc = "?";
			switch (cap.type) {
			case XInputCapture::BUTTON_BIT:  desc = xinputBitName(cap.bit); break;
			case XInputCapture::TRIGGER_LT:  desc = "Left Trigger";          break;
			case XInputCapture::TRIGGER_RT:  desc = "Right Trigger";         break;
			default: break;
			}
			printf("captured: %s\n", desc);
			fflush(stdout);
			_userMap[i] = cap;
			break;
		}
	}

	printf("\n");
	printf("Wizard complete. Saving mapping...\n");
	if (saveMapping()) {
		printf("\n");
		printf("Done! Restart flycast normally (without MAPLECAST_BUTTON_WIZARD)\n");
		printf("and your mapping will be loaded automatically.\n");
		printf("\n");
	}
	fflush(stdout);
}

// XInput state we'd emit a "press" or "release" event for — change-detected
// against the previous poll so we don't spam onButton 1000x/sec when nothing
// changed.
struct LastState {
	bool  connected;
	WORD  buttons;
	BYTE  leftTrigger;
	BYTE  rightTrigger;
	SHORT thumbLX;
	SHORT thumbLY;
};
static LastState _last[XUSER_MAX_COUNT];

// Trigger threshold: anything above this counts as "pressed" for digital
// trigger emulation; analog trigger value is also forwarded via input_sink's
// sendTrigger() path so the server sees full 0-255 resolution.
static constexpr BYTE kTriggerThreshold = 30;
// Stick deadzone for d-pad emulation when only the analog stick is moving.
static constexpr SHORT kStickDeadzone = 16384;  // 50% of full range

// Forward an XInput state change through flycast's regular mapping pipeline:
//   gamepad_btn_input(joybutton, pressed)
//     -> InputMapping::get_button_id() applies the user's saved mapping
//     -> handleButtonInput() updates kcode[]/lt[]/rt[] AND fires
//        globalButtonListener (which input_sink listens on)
//
// The dedup in maplecast_input_sink::onButton drops the SDL fire of the
// same press, so the user sees: XInput races to onButton first (winning
// the latency), gets the user's mapping applied, sends one UDP packet.
// SDL fires later, sees no button-bitmap change, drops silently.
static std::shared_ptr<GamepadDevice> findSdlGamepad()
{
	// Find an SDL gamepad device — that's the one whose mapping the user
	// edited via F10 -> Settings -> Controls. Positive filter on api_name
	// "SDL" so we never grab a keyboard or mouse pseudo-device by accident.
	// For multi-pad scenarios we'd need to disambiguate; mirror clients are
	// single-player by design so taking the first match is correct here.
	for (int i = 0; i < GamepadDevice::GetGamepadCount(); i++) {
		auto dev = GamepadDevice::GetGamepad(i);
		if (dev && dev->api_name() == "SDL")
			return dev;
	}
	return nullptr;
}

static void emitButton(int joyButton, bool pressed)
{
	auto dev = findSdlGamepad();
	if (dev)
		dev->gamepad_btn_input((u32)joyButton, pressed);
	// No SDL gamepad found yet (init race) — drop silently. Next press
	// after SDL gamepad init completes will go through normally.
}

static void emitAxis(int joyAxis, int value)
{
	auto dev = findSdlGamepad();
	if (dev)
		dev->gamepad_axis_input((u32)joyAxis, value);
}

static void xinputPollLoop()
{
	// Defer priority bump until we've slept past the dynamic-load window.
	std::this_thread::sleep_for(std::chrono::milliseconds(500));

	memset(_last, 0, sizeof(_last));

	if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
		printf("[rawinput] xinput thread: SetThreadPriority(TIME_CRITICAL) failed (%lu)\n", GetLastError());
	else
		printf("[rawinput] xinput thread -> THREAD_PRIORITY_TIME_CRITICAL\n");
	fflush(stdout);

	using clock = std::chrono::steady_clock;
	auto nextTick = clock::now();
	uint64_t iterCount = 0;

	while (_run.load(std::memory_order_relaxed))
	{
		try {
		// Poll all 4 XInput slots. In the mirror-client case only slot 0
		// matters but checking all 4 is cheap — XInputGetState on a
		// disconnected slot returns ERROR_DEVICE_NOT_CONNECTED in <1us.
		for (DWORD slot = 0; slot < XUSER_MAX_COUNT; slot++)
		{
			XINPUT_STATE state{};
			DWORD rc = XInputGetState(slot, &state);
			LastState& prev = _last[slot];

			if (rc != ERROR_SUCCESS) {
				if (prev.connected) {
					// Just disconnected — release any held buttons.
					if (_userMapLoaded) {
						for (int i = 0; i < kNumSlots; i++) {
							const auto& cap = _userMap[i];
							if (cap.type == XInputCapture::BUTTON_BIT && (prev.buttons & cap.bit))
								GamepadDevice::fireButtonGlobal((int)slot, kSlots[i].dcKey, false);
						}
					} else {
						for (const auto& b : kFallbackBindings)
							if (prev.buttons & b.xinputBit)
								emitButton(b.sdlJoyButton, false);
					}
					memset(&prev, 0, sizeof(prev));
				}
				continue;
			}

			const XINPUT_GAMEPAD& gp = state.Gamepad;

			// Helper: emit a DreamcastKey for the wire. Bitmap-class keys
			// (A/B/X/Y/dpad/start) go through fireButtonGlobal which the
			// input_sink picks up via its onButton listener. Axis-class
			// keys (DC_AXIS_LT/RT) bypass that listener path because
			// input_sink::onButton early-returns on key > 0x20000, so we
			// also have to write the lt[]/rt[] globals directly. The
			// input_sink trigger poll thread reads those and sends them
			// in the W3 wire packet's analog trigger bytes.
			auto emitDcKey = [&](DreamcastKey key, bool pressed) {
				const int port = (int)slot;
				if (key == DC_AXIS_LT && port >= 0 && port < 4) {
					lt[port] = pressed ? 0xffff : 0;
					return;
				}
				if (key == DC_AXIS_RT && port >= 0 && port < 4) {
					rt[port] = pressed ? 0xffff : 0;
					return;
				}
				GamepadDevice::fireButtonGlobal(port, key, pressed);
			};

			// Digital buttons — diff against previous poll.
			WORD changed = gp.wButtons ^ prev.buttons;
			if (_userMapLoaded) {
				// User-mapping fast path: fire DreamcastKey directly per the
				// wizard's saved bindings, bypassing flycast's InputMapping.
				if (changed) {
					for (int i = 0; i < kNumSlots; i++) {
						const auto& cap = _userMap[i];
						if (cap.type == XInputCapture::BUTTON_BIT && (changed & cap.bit)) {
							bool pressed = (gp.wButtons & cap.bit) != 0;
							emitDcKey(kSlots[i].dcKey, pressed);
						}
					}
				}
				// Trigger crossings — fire when crossing the threshold.
				bool prevLtPressed = prev.leftTrigger >= 100;
				bool curLtPressed  = gp.bLeftTrigger  >= 100;
				if (prevLtPressed != curLtPressed) {
					for (int i = 0; i < kNumSlots; i++) {
						if (_userMap[i].type == XInputCapture::TRIGGER_LT)
							emitDcKey(kSlots[i].dcKey, curLtPressed);
					}
				}
				bool prevRtPressed = prev.rightTrigger >= 100;
				bool curRtPressed  = gp.bRightTrigger  >= 100;
				if (prevRtPressed != curRtPressed) {
					for (int i = 0; i < kNumSlots; i++) {
						if (_userMap[i].type == XInputCapture::TRIGGER_RT)
							emitDcKey(kSlots[i].dcKey, curRtPressed);
					}
				}
			} else {
				// Fallback path: route through flycast's SDL InputMapping.
				if (changed) {
					for (const auto& b : kFallbackBindings) {
						if (changed & b.xinputBit) {
							bool pressed = (gp.wButtons & b.xinputBit) != 0;
							emitButton(b.sdlJoyButton, pressed);
						}
					}
				}
				if (gp.bLeftTrigger != prev.leftTrigger) {
					int v = (int)gp.bLeftTrigger * 32767 / 255;
					emitAxis(4, v);
				}
				if (gp.bRightTrigger != prev.rightTrigger) {
					int v = (int)gp.bRightTrigger * 32767 / 255;
					emitAxis(5, v);
				}
				if (gp.sThumbLX != prev.thumbLX) emitAxis(0, gp.sThumbLX);
				if (gp.sThumbLY != prev.thumbLY) emitAxis(1, -gp.sThumbLY);
			}

			prev.connected = true;
			prev.buttons     = gp.wButtons;
			prev.leftTrigger = gp.bLeftTrigger;
			prev.rightTrigger = gp.bRightTrigger;
			prev.thumbLX     = gp.sThumbLX;
			prev.thumbLY     = gp.sThumbLY;
		}

		// 1 kHz poll cadence — tighter than SDL's default. Use a steady
		// next-tick so we don't drift if a poll takes longer than expected.
		nextTick += std::chrono::milliseconds(1);
		std::this_thread::sleep_until(nextTick);

		// First-iteration diagnostic — confirm we made it through the
		// initial poll without crashing.
		if (iterCount++ == 0) {
			printf("[rawinput] first poll iteration completed cleanly\n");
			fflush(stdout);
		}
		} catch (const std::exception& e) {
			printf("[rawinput] xinput thread caught std::exception: %s\n", e.what());
			fflush(stdout);
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		} catch (...) {
			printf("[rawinput] xinput thread caught unknown exception\n");
			fflush(stdout);
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	printf("[rawinput] xinput thread exiting\n");
}

bool init()
{
	// Idempotent — emulator.cpp calls this from multiple init paths
	// (early-init + mirror-client init). Re-entry on an already-spawned
	// thread would cause `_xinputThread = std::thread(...)` to invoke
	// std::terminate() per C++17 [thread.thread.assign] (re-assigning a
	// joinable thread is UB and the runtime aborts). That manifests as
	// STATUS_STACK_BUFFER_OVERRUN (0xc0000409) in ucrtbase, killing the
	// process at a seemingly random later point. Don't be that guy.
	if (_active.load(std::memory_order_acquire)) {
		printf("[rawinput] init() already active — skipping re-entry\n");
		return true;
	}

	// (The wizard now runs from winmain.cpp early-startup, BEFORE flycast
	// is initialized — that way the user doesn't need MIRROR_CLIENT or
	// any other mode-flag set to remap. See runWizardStandalone() below.)

	const char* env = std::getenv("MAPLECAST_RAWINPUT");
	if (!env || !*env) return false;

	std::string mode = env;
	bool wantXInput = (mode == "1" || mode == "xinput" || mode == "all");
	bool wantRaw    = (mode == "1" || mode == "raw"    || mode == "all");

	// Try to load a previously-saved user mapping. If found, the XInput
	// poll loop will fire DreamcastKey directly (bypassing flycast's
	// InputMapping). If not, fall back to the canonical Xbox layout via
	// SDL's gamepad mapping system.
	loadMapping();

	if (wantXInput) {
		_run.store(true, std::memory_order_relaxed);
		_xinputThread = std::thread(xinputPollLoop);
		_xinputThread.detach();   // detach so destruction can't terminate()
		_active.store(true, std::memory_order_release);
		printf("[rawinput] XInput direct polling enabled (1 kHz, TIME_CRITICAL)\n");
		if (_userMapLoaded)
			printf("[rawinput] Using user mapping from %s\n", mappingFilePath().c_str());
		else
			printf("[rawinput] No user mapping found — using SDL fallback. Run with MAPLECAST_BUTTON_WIZARD=1 to create one.\n");
	}

	// Raw Input fallback for non-XInput devices is left unimplemented for
	// the first cut. XInput covers Xbox-compat (90%+ of gamepads); generic
	// HID via Raw Input is a future addition for niche fightsticks. Falls
	// through to SDL's existing path for those devices in the meantime.
	if (wantRaw && !wantXInput) {
		printf("[rawinput] Raw Input mode requested but not yet implemented — using SDL fallback\n");
	}

	return _active.load(std::memory_order_relaxed);
}

void shutdown()
{
	if (!_active.load(std::memory_order_relaxed)) return;
	_run.store(false, std::memory_order_relaxed);
	if (_xinputThread.joinable()) _xinputThread.join();
	_active.store(false, std::memory_order_relaxed);
	printf("[rawinput] shutdown complete\n");
}

bool active() {
	return _active.load(std::memory_order_relaxed);
}

bool userMappingActive() {
	return _userMapLoaded;
}

bool runWizardStandalone()
{
	// Console may not be attached yet — caller in winmain.cpp handles
	// AllocConsole when MAPLECAST=1 is set. We just print and capture.
	runWizard();
	return _userMap[0].type != XInputCapture::NONE;  // crude success check
}

} // namespace maplecast_rawinput

#endif // _WIN32
