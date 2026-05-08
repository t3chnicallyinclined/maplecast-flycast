// Windows headless entry point — used only when MAPLECAST_HEADLESS=ON.
//
// Mirrors core/windows/winmain.cpp's main() init sequence step-for-step,
// dropping only the GUI-specific pieces (breakpad, button-wizard, SDL,
// keyboard layout, mirror-client autoload, MessageBox error popup). The
// load-bearing pieces — setupPath, i18n::init, os_InstallFaultHandler,
// SetPriorityClass — are kept identical so the SH4 dynarec sees the same
// init state it does in the GUI client and on the Linux headless server.
//
// Without os_InstallFaultHandler the SH4 dynarec's first guest-memory
// access raises STATUS_ACCESS_VIOLATION and Windows kills the process
// silently — exactly the symptom we hit before this file mirrored the
// fault-handler call.

#include "types.h"
#include "log/LogManager.h"
#include "emulator.h"
#include "ui/mainui.h"
#include "ui/gui.h"
#include "stdclass.h"
#include "cfg/cfg.h"
#include "oslib/oslib.h"
#include "oslib/i18n.h"
#include "oslib/directory.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>

#include <windows.h>
#include "nowide/args.hpp"
#include "nowide/stackstring.hpp"
#include "nowide/cstdlib.hpp"

void os_DoEvents() {}

void os_RunInstance(int argc, const char* argv[])
{
	(void)argc; (void)argv;
	WARN_LOG(BOOT, "[HEADLESS-WIN] os_RunInstance called but multiboard not supported on Windows");
}

[[noreturn]] void os_DebugBreak()
{
	__debugbreak();
	std::abort();
}

// Thread-name utilities — identical to the GUI build's winmain.cpp impl.
static thread_local std::string _threadName;

void os_SetThreadName(const char* name)
{
	if (name == nullptr) return;
	_threadName = name;
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

// rawinput stubs — full impls are in core/windows/rawinput.cpp, excluded
// from headless builds because they require getNativeHwnd().
namespace rawinput
{
	void init() {}
	void term() {}
}

// maplecast_rawinput stub — XInput direct-poll bypass for the GUI client.
namespace maplecast_rawinput
{
	bool init() { return false; }
}

// setupPath — copied from winmain.cpp:58. Sets user_config_dir and
// user_data_dir relative to the binary's location. Without this,
// flycast can't find/write its config files (cfg/option lookups
// silently fall through to defaults, which can leave critical state
// fields like AutoLoadState unset and break headless boot).
static void setupPath()
{
	wchar_t fname[512];
	GetModuleFileNameW(0, fname, std::size(fname));

	std::string fn;
	nowide::stackstring path;
	if (!path.convert(fname))
		fn = ".\\";
	else
		fn = path.get();
	size_t pos = get_last_slash_pos(fn);
	if (pos != std::string::npos)
		fn = fn.substr(0, pos) + "\\";
	else
		fn = ".\\";
	set_user_config_dir(fn);
	add_system_data_dir(fn);

	std::string data_path = fn + "data\\";
	set_user_data_dir(data_path);
	flycast::mkdir(data_path.c_str(), 0755);
}

int main(int argc, char* argv[])
{
	nowide::args _(argc, argv);

	// Console is already attached because /SUBSYSTEM:CONSOLE was set in
	// CMakeLists for headless. No AllocConsole needed.

	LogManager::Init();
	i18n::init();

	setupPath();

	if (flycast_init(argc, argv) != 0)
		die("Flycast initialization failed");

	// Boost process priority — same call as the GUI client.
	if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
		printf("[maplecast-headless] SetPriorityClass(HIGH) failed: %lu\n", GetLastError());
	else
		printf("[maplecast-headless] process priority class: HIGH\n");
	fflush(stdout);

	// CRITICAL: install the fault handler. The SH4 dynarec relies on
	// SEH-style fault handling for guest-memory accesses; without this,
	// the first SH4 access violation kills the process silently.
	os_InstallFaultHandler();

	// Headless mode: load the ROM synchronously, transition to Running,
	// drop the GUI state to Closed. Same shape as core/linux-dist/main.cpp's
	// headless block.
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
	os_UninstallFaultHandler();

	return 0;
}
