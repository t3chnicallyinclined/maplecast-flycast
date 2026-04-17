#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS 1
#endif
#include "types.h"

#if defined(__unix__) || defined(__HAIKU__)
#include "log/LogManager.h"
#include "emulator.h"
#include "ui/mainui.h"
#include "ui/gui.h"
#include "oslib/directory.h"
#include "oslib/oslib.h"
#include "oslib/i18n.h"
#include "stdclass.h"

#include <csignal>
#include <string>
#include <unistd.h>
#include <vector>
#include <exception>

#if defined(SUPPORT_X11)
	#include "x11.h"
#endif

#if defined(USE_SDL)
	#include "sdl/sdl.h"
#endif

#ifdef USE_BREAKPAD
#include "breakpad/client/linux/handler/exception_handler.h"
#endif

void os_DoEvents()
{
	#if defined(SUPPORT_X11)
		input_x11_handle();
		event_x11_handle();
	#endif
}

void common_linux_setup();

// Find the user config directory.
// $HOME/.config/flycast on linux
static std::string find_user_config_dir()
{
	std::string xdg_home;
	if (nowide::getenv("XDG_CONFIG_HOME") != nullptr)
		// If XDG_CONFIG_HOME is set explicitly, we'll use that instead of $HOME/.config
		xdg_home = (std::string)nowide::getenv("XDG_CONFIG_HOME");
	else if (nowide::getenv("HOME") != nullptr)
		/* If $XDG_CONFIG_HOME is not set, we're supposed to use "$HOME/.config" instead.
		 * Consult the XDG Base Directory Specification for details:
		 *   http://standards.freedesktop.org/basedir-spec/basedir-spec-latest.html#variables
		 */
		xdg_home = (std::string)nowide::getenv("HOME") + "/.config";

	if (!xdg_home.empty())
	{
		std::string fullpath = xdg_home + "/flycast/";
		struct stat info;
		if (flycast::stat(fullpath.c_str(), &info) != 0 || (info.st_mode & S_IFDIR) == 0)
			// Create .config/flycast
			flycast::mkdir(fullpath.c_str(), 0755);

		return fullpath;
	}
	// Unable to detect config dir, use the current folder
	return ".";
}

// Find the user data directory.
// $HOME/.local/share/flycast on linux
static std::string find_user_data_dir()
{
	std::string xdg_home;
	if (nowide::getenv("XDG_DATA_HOME") != nullptr)
		// If XDG_DATA_HOME is set explicitly, we'll use that instead of $HOME/.local/share
		xdg_home = (std::string)nowide::getenv("XDG_DATA_HOME");
	else if (nowide::getenv("HOME") != nullptr)
		/* If $XDG_DATA_HOME is not set, we're supposed to use "$HOME/.local/share" instead.
		 * Consult the XDG Base Directory Specification for details:
		 *   http://standards.freedesktop.org/basedir-spec/basedir-spec-latest.html#variables
		 */
		xdg_home = (std::string)nowide::getenv("HOME") + "/.local/share";

	if (!xdg_home.empty())
	{
		std::string fullpath = xdg_home + "/flycast/";
		struct stat info;
		if (flycast::stat(fullpath.c_str(), &info) != 0 || (info.st_mode & S_IFDIR) == 0)
			// Create .local/share/flycast
			flycast::mkdir(fullpath.c_str(), 0755);

		return fullpath;
	}
	// Unable to detect data dir, use the current folder
	return ".";
}

static void addDirectoriesFromPath(std::vector<std::string>& dirs, const std::string& path, const std::string& suffix)
{
	std::string::size_type pos = 0;
	std::string::size_type n = path.find(':', pos);
	while (n != std::string::npos)
	{
		if (n != pos)
			dirs.push_back(path.substr(pos, n - pos) + suffix);
		pos = n + 1;
		n = path.find(':', pos);
	}
	// Separator not found
	if (pos < path.length())
		dirs.push_back(path.substr(pos) + suffix);
}

// Find a file in the user and system config directories.
// The following folders are checked in this order:
// $HOME/.config/flycast
// if XDG_CONFIG_DIRS is defined:
//   <$XDG_CONFIG_DIRS>/flycast
// else
//   /etc/flycast/
//   /etc/xdg/flycast/
// .
static std::vector<std::string> find_system_config_dirs()
{
	std::vector<std::string> dirs;

	std::string xdg_home;
	if (nowide::getenv("XDG_CONFIG_HOME") != nullptr)
		// If XDG_CONFIG_HOME is set explicitly, we'll use that instead of $HOME/.config
		xdg_home = (std::string)nowide::getenv("XDG_CONFIG_HOME");
	else if (nowide::getenv("HOME") != nullptr)
		xdg_home = (std::string)nowide::getenv("HOME") + "/.config";
	if (!xdg_home.empty())
		// XDG config locations
		dirs.push_back(xdg_home + "/flycast/");

	if (nowide::getenv("XDG_CONFIG_DIRS") != nullptr)
	{
		std::string path = (std::string)nowide::getenv("XDG_CONFIG_DIRS");
		addDirectoriesFromPath(dirs, path, "/flycast/");
	}
	else
	{
#ifdef FLYCAST_SYSCONFDIR
		const std::string config_dir(FLYCAST_SYSCONFDIR);
		dirs.push_back(config_dir);
#endif
		dirs.push_back("/etc/flycast/"); // This isn't part of the XDG spec, but much more common than /etc/xdg/
		dirs.push_back("/etc/xdg/flycast/");
	}
	dirs.push_back("./");

	return dirs;
}

// Find a file in the user data directories.
// The following folders are checked in this order:
// $HOME/.local/share/flycast
// if XDG_DATA_DIRS is defined:
//   <$XDG_DATA_DIRS>/flycast
// else
//   /usr/local/share/flycast
//   /usr/share/flycast
// <$FLYCAST_BIOS_PATH>
// ./
// ./data
static std::vector<std::string> find_system_data_dirs()
{
	std::vector<std::string> dirs;

	std::string xdg_home;
	if (nowide::getenv("XDG_DATA_HOME") != nullptr)
		// If XDG_DATA_HOME is set explicitly, we'll use that instead of $HOME/.local/share
		xdg_home = (std::string)nowide::getenv("XDG_DATA_HOME");
	else if (nowide::getenv("HOME") != nullptr)
		xdg_home = (std::string)nowide::getenv("HOME") + "/.local/share";
	if (!xdg_home.empty())
		// XDG data locations
		dirs.push_back(xdg_home + "/flycast/");

	if (nowide::getenv("XDG_DATA_DIRS") != nullptr)
	{
		std::string path = (std::string)nowide::getenv("XDG_DATA_DIRS");
		addDirectoriesFromPath(dirs, path, "/flycast/");
	}
	else
	{
#ifdef FLYCAST_DATADIR
		const std::string data_dir(FLYCAST_DATADIR);
		dirs.push_back(data_dir);
#endif
		dirs.push_back("/usr/local/share/flycast/");
		dirs.push_back("/usr/share/flycast/");
	}
	if (nowide::getenv("FLYCAST_BIOS_PATH") != nullptr)
	{
		std::string path = (std::string)nowide::getenv("FLYCAST_BIOS_PATH");
		addDirectoriesFromPath(dirs, path, "/");
	}
	dirs.push_back("./");
	dirs.push_back("data/");

	return dirs;
}

static const char *selfPath;

void os_RunInstance(int argc, const char *argv[])
{
	if (fork() == 0)
	{
		std::vector<char *> localArgs;
		localArgs.push_back((char *)selfPath);
		for (int i = 0; i < argc; i++)
			localArgs.push_back((char *)argv[i]);
		localArgs.push_back(nullptr);
		execv(selfPath, &localArgs[0]);
	}
}

#if defined(USE_BREAKPAD)
static bool dumpCallback(const google_breakpad::MinidumpDescriptor& descriptor, void* context, bool succeeded)
{
	if (succeeded)
		registerCrash(descriptor.directory().c_str(), descriptor.path());

	return succeeded;
}
#endif

int main(int argc, char* argv[])
{
	selfPath = argv[0];
#if defined(USE_BREAKPAD)
	google_breakpad::MinidumpDescriptor descriptor("/tmp");
	google_breakpad::ExceptionHandler eh(descriptor, nullptr, dumpCallback, nullptr, true, -1);
#endif

	LogManager::Init();
	i18n::init();

	// Set directories
	set_user_config_dir(find_user_config_dir());
	set_user_data_dir(find_user_data_dir());
	for (const auto& dir : find_system_config_dirs())
		add_system_config_dir(dir);
	for (const auto& dir : find_system_data_dirs())
		add_system_data_dir(dir);
	INFO_LOG(BOOT, "Config dir is: %s", get_writable_config_path("").c_str());
	INFO_LOG(BOOT, "Data dir is:   %s", get_writable_data_path("").c_str());

#if defined(USE_SDL)
	// init video now: on rpi3 it installs a sigsegv handler(?)
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
	{
		die("SDL: Initialization failed!");
	}
#endif

	common_linux_setup();

	if (flycast_init(argc, argv))
		die("Flycast initialization failed\n");

	// Mirror client: --server <host>[:<port>] launches romless mirror mode
	// Also supports env vars MAPLECAST_MIRROR_CLIENT + MAPLECAST_SERVER_HOST
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
			// Parse host:port
			std::string addr = argv[i + 1];
			std::string host = addr;
			std::string port = "7200";
			auto colon = addr.rfind(':');
			if (colon != std::string::npos) {
				host = addr.substr(0, colon);
				port = addr.substr(colon + 1);
			}
			setenv("MAPLECAST_MIRROR_CLIENT", "1", 1);
			setenv("MAPLECAST_SERVER_HOST", host.c_str(), 1);
			setenv("MAPLECAST_SERVER_PORT", port.c_str(), 1);
			printf("[MIRROR] --server %s:%s\n", host.c_str(), port.c_str());
			fflush(stdout);
			emu.loadGame(nullptr);
			break;
		}
	}
	// Env var fallback
	if (nowide::getenv("MAPLECAST_MIRROR_CLIENT") && argc < 2) {
		printf("[MIRROR] Auto-loading without ROM\n");
		fflush(stdout);
		emu.loadGame(nullptr);
	}

	// MapleCast headless: no GUI, no GameLoader. Load the ROM synchronously
	// on the main thread (same as what BackgroundGameLoader::load would do
	// asynchronously), then drop the GUI state to Closed so mainui_loop
	// falls through to the emu.render() branch without ever calling
	// gui_display_ui() (which dereferences imguiDriver).
	//
	// Triggers on either the compile-time MAPLECAST_HEADLESS_BUILD define
	// (Phase 3 compile-out binary — always headless) or the runtime
	// MAPLECAST_HEADLESS=1 env var (Phase 1 runtime gate on GPU builds).
#ifdef MAPLECAST_HEADLESS_BUILD
	const bool _headless_mode = true;
#else
	const bool _headless_mode = (std::getenv("MAPLECAST_HEADLESS") != nullptr);
#endif
	if (_headless_mode)
	{
		if (!settings.content.path.empty())
		{
			printf("[HEADLESS] Loading game: %s\n", settings.content.path.c_str());
			fflush(stdout);
			try {
				emu.loadGame(settings.content.path.c_str());
				// loadGame leaves us in state=Loaded. Transition to Running so
				// emu.render() actually runs the SH4 executor. Emulator::start()
				// is also where maplecast_mirror::initServer() runs.
				emu.start();
			} catch (const FlycastException& e) {
				ERROR_LOG(BOOT, "[HEADLESS] loadGame failed: %s", e.what());
				return 1;
			}
		}
		else
		{
			ERROR_LOG(BOOT, "[HEADLESS] no ROM path — pass a rom as an argument");
			return 1;
		}
		gui_setState(GuiState::Closed);
		printf("[HEADLESS] Game loaded, GUI closed, entering main loop\n");
		fflush(stdout);
	}

#if defined(USE_BREAKPAD)
	auto async = std::async(std::launch::async, uploadCrashes, "/tmp");
#endif

	try {
		mainui_loop();
	} catch (const std::exception& e) {
		ERROR_LOG(BOOT, "mainui_loop error: %s", e.what());
	} catch (...) {
		ERROR_LOG(BOOT, "mainui_loop unknown exception");
	}

	flycast_term();
	os_UninstallFaultHandler();

	// Mirror client detaches WS threads that can outlive main().
	// Force-exit to avoid hanging processes that steal server slots.
	_exit(0);
}

[[noreturn]] void os_DebugBreak()
{
	raise(SIGTRAP);
	std::abort();
}

#endif // __unix__
