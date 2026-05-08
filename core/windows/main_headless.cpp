// Windows headless entry point — used only when MAPLECAST_HEADLESS=ON.
//
// Minimal console-mode main(). No SDL, no DirectSound, no rawinput, no
// imguiDriver. Same shape as core/linux-dist/main.cpp's headless block:
// flycast_init -> loadGame -> Running -> mainui_loop. The headless-aware
// gates inside flycast already handle "no GUI" correctly; we just need a
// non-WinMain entry that doesn't drag in the desktop dependencies.

#include "types.h"
#include "log/LogManager.h"
#include "emulator.h"
#include "ui/mainui.h"
#include "ui/gui.h"
#include "stdclass.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>

#include <windows.h>

void os_DoEvents() {}

void os_RunInstance(int argc, const char* argv[])
{
	// Multiboard (Naomi multi-cabinet) launches a child flycast on Linux via
	// fork+exec. Headless Windows doesn't support multiboard for V1 — log and
	// no-op so callers don't crash.
	(void)argc; (void)argv;
	WARN_LOG(BOOT, "[HEADLESS-WIN] os_RunInstance called but multiboard not supported on Windows");
}

[[noreturn]] void os_DebugBreak()
{
	__debugbreak();
	std::abort();
}

// Thread-name utilities — used by stdclass.cpp / fault_handler.cpp / etc.
// Normally provided by core/windows/winmain.cpp; we replicate the minimal
// implementation here so the headless build links cleanly.
static thread_local std::string _threadName;

void os_SetThreadName(const char* name)
{
	if (name == nullptr) return;
	_threadName = name;
	// SetThreadDescription is a Win10 1607+ API for thread names visible
	// in debuggers and ETW traces. Resolve dynamically to remain compatible
	// with older Windows builds.
	using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
	static SetThreadDescriptionFn fn = []() -> SetThreadDescriptionFn {
		HMODULE k = GetModuleHandleA("kernel32.dll");
		return k ? (SetThreadDescriptionFn)GetProcAddress(k, "SetThreadDescription") : nullptr;
	}();
	if (fn) {
		std::wstring wname(name, name + strlen(name));
		fn(GetCurrentThread(), wname.c_str());
	}
}

const char* getThreadName()
{
	return _threadName.empty() ? "?" : _threadName.c_str();
}

// rawinput stubs — full implementations live in core/windows/rawinput.cpp,
// which is excluded from headless builds because it transitively depends
// on the SDL window handle. Headless predictor doesn't need raw HID input
// (gamepad arrives via UDP from the network sink).
namespace rawinput
{
	void init() {}
	void term() {}
}

// maplecast_rawinput stub — XInput direct-poll bypass for the GUI client.
// Not relevant to the headless predictor; gamepad routing is the input
// server's responsibility.
namespace maplecast_rawinput
{
	bool init() { return false; }
}

int main(int argc, char* argv[])
{
	LogManager::Init();

	if (flycast_init(argc, argv))
		die("Flycast initialization failed\n");

	// MAPLECAST_HEADLESS_BUILD compile flag is always set in this binary.
#ifdef MAPLECAST_HEADLESS_BUILD
	const bool _headless_mode = true;
#else
	const bool _headless_mode = (std::getenv("MAPLECAST_HEADLESS") != nullptr);
#endif

	if (_headless_mode) {
		if (!settings.content.path.empty()) {
			printf("[HEADLESS-WIN] Loading game: %s\n", settings.content.path.c_str());
			fflush(stdout);
			try {
				emu.loadGame(settings.content.path.c_str());
				emu.start();
			} catch (const FlycastException& e) {
				ERROR_LOG(BOOT, "[HEADLESS-WIN] loadGame failed: %s", e.what());
				return 1;
			}
		} else {
			ERROR_LOG(BOOT, "[HEADLESS-WIN] no ROM path — pass a rom as the first argument");
			return 1;
		}
		gui_setState(GuiState::Closed);
		printf("[HEADLESS-WIN] Game loaded, GUI closed, entering main loop\n");
		fflush(stdout);
	}

	try {
		mainui_loop();
	} catch (const std::exception& e) {
		ERROR_LOG(BOOT, "mainui_loop error: %s", e.what());
	} catch (...) {
		ERROR_LOG(BOOT, "mainui_loop unknown exception");
	}

	flycast_term();

	// WS threads detached for performance can outlive main(); force-exit
	// to avoid hanging the binary on shutdown. Same pattern as Linux.
	_exit(0);
}
