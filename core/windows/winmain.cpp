/*
	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS 1
#endif
#include "build.h"
#ifdef TARGET_UWP
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Globalization.DateTimeFormatting.h>
#include <winrt/Windows.Storage.h>
#include <io.h>
#include <fcntl.h>
#include <nowide/config.hpp>
#include <nowide/convert.hpp>
#include "cfg/option.h"
#include "ui/gui.h"
#endif
#include "oslib/oslib.h"
#include "stdclass.h"
#include "cfg/cfg.h"
#include "log/LogManager.h"
#include "sdl/sdl.h"
#include "emulator.h"
#include "ui/mainui.h"
#include "oslib/directory.h"
#include "oslib/i18n.h"
#include "dynlink.h"
#include "rawinput_gamepad.h"
#ifdef USE_BREAKPAD
#include "breakpad/client/windows/handler/exception_handler.h"
#include "version.h"
#endif
#include "profiler/fc_profiler.h"

#include <nowide/args.hpp>
#include <nowide/stackstring.hpp>
#include <exception>

#include <windows.h>
#include <windowsx.h>

static void setupPath()
{
#ifndef TARGET_UWP
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
#else
	using namespace Windows::Storage;
	StorageFolder^ localFolder = Windows::Storage::ApplicationData::Current->LocalFolder;
	nowide::stackstring path;
	path.convert(localFolder->Path->Data());
	std::string homePath(path.get());
	homePath += '\\';
	set_user_config_dir(homePath);
	homePath += "data\\";
	set_user_data_dir(homePath);
	flycast::mkdir(homePath.c_str(), 0755);
	SetEnvironmentVariable(L"HOMEPATH", localFolder->Path->Data());
	SetEnvironmentVariable(L"HOMEDRIVE", nullptr);
#endif
}

static void reserveBottomMemory()
{
#if defined(_WIN64) && defined(_DEBUG)
    static bool s_initialized = false;
    if ( s_initialized )
        return;
    s_initialized = true;

    // Start by reserving large blocks of address space, and then
    // gradually reduce the size in order to capture all of the
    // fragments. Technically we should continue down to 64 KB but
    // stopping at 1 MB is sufficient to keep most allocators out.

    const size_t LOW_MEM_LINE = 0x100000000LL;
    size_t totalReservation = 0;
    size_t numVAllocs = 0;
    size_t numHeapAllocs = 0;
    for (size_t size = 256_MB; size >= 1_MB; size /= 2)
    {
        for (;;)
        {
            void* p = VirtualAlloc(0, size, MEM_RESERVE, PAGE_NOACCESS);
            if (!p)
                break;

            if ((size_t)p >= LOW_MEM_LINE)
            {
                // We don't need this memory, so release it completely.
                VirtualFree(p, 0, MEM_RELEASE);
                break;
            }

            totalReservation += size;
            ++numVAllocs;
        }
    }

    // Now repeat the same process but making heap allocations, to use up
    // the already reserved heap blocks that are below the 4 GB line.
    HANDLE heap = GetProcessHeap();
    for (size_t blockSize = 64_KB; blockSize >= 16; blockSize /= 2)
    {
        for (;;)
        {
            void* p = HeapAlloc(heap, 0, blockSize);
            if (!p)
                break;

            if ((size_t)p >= LOW_MEM_LINE)
            {
                // We don't need this memory, so release it completely.
                HeapFree(heap, 0, p);
                break;
            }

            totalReservation += blockSize;
            ++numHeapAllocs;
        }
    }

    // Perversely enough the CRT doesn't use the process heap. Suck up
    // the memory the CRT heap has already reserved.
    for (size_t blockSize = 64_KB; blockSize >= 16; blockSize /= 2)
    {
        for (;;)
        {
            void* p = malloc(blockSize);
            if (!p)
                break;

            if ((size_t)p >= LOW_MEM_LINE)
            {
                // We don't need this memory, so release it completely.
                free(p);
                break;
            }

            totalReservation += blockSize;
            ++numHeapAllocs;
        }
    }

    // Print diagnostics showing how many allocations we had to make in
    // order to reserve all of low memory, typically less than 200.
    char buffer[1000];
    snprintf(buffer, sizeof(buffer),
             "Reserved %1.3f MB (%d vallocs,"
             "%d heap allocs) of low-memory.\n",
             totalReservation / (1024 * 1024.0),
             (int)numVAllocs, (int)numHeapAllocs);
    OutputDebugStringA(buffer);
#endif
}
static void findKeyboardLayout()
{
#ifndef TARGET_UWP
	HKL keyboardLayout = GetKeyboardLayout(0);
	WORD lcid = HIWORD(keyboardLayout);
	switch (PRIMARYLANGID(lcid)) {
	case 0x09:	// English
		if (lcid == 0x0809)
			settings.input.keyboardLangId = KeyboardLayout::UK;
		else
			settings.input.keyboardLangId = KeyboardLayout::US;
		break;
	case 0x11:
		settings.input.keyboardLangId = KeyboardLayout::JP;
		break;
	case 0x07:
		settings.input.keyboardLangId = KeyboardLayout::GE;
		break;
	case 0x0c:
		settings.input.keyboardLangId = KeyboardLayout::FR;
		break;
	case 0x10:
		settings.input.keyboardLangId = KeyboardLayout::IT;
		break;
	case 0x0A:
		settings.input.keyboardLangId = KeyboardLayout::SP;
		break;
	default:
		break;
	}
#endif
}

#if defined(USE_BREAKPAD)
static bool dumpCallback(const wchar_t* dump_path,
		const wchar_t* minidump_id,
		void* context,
		EXCEPTION_POINTERS* exinfo,
		MDRawAssertionInfo* assertion,
		bool succeeded)
{
	if (succeeded)
	{
		wchar_t s[MAX_PATH + 32];
		_snwprintf(s, std::size(s), L"Minidump saved to '%s\\%s.dmp'", dump_path, minidump_id);
		::OutputDebugStringW(s);

		nowide::stackstring path;
		if (path.convert(dump_path))
		{
			std::string directory = path.get();
			if (path.convert(minidump_id))
			{
				std::string fullPath = directory + '\\' + std::string(path.get()) + ".dmp";
				registerCrash(directory.c_str(), fullPath.c_str());
			}
		}
	}
	return succeeded;
}
#endif

#ifdef TARGET_UWP
namespace nowide {

FILE *fopen(char const *file_name, char const *mode)
{
	wstackstring wname;
	if (!wname.convert(file_name))
	{
		errno = EINVAL;
		return nullptr;
	}
	DWORD dwDesiredAccess;
	DWORD dwCreationDisposition;
	int openFlags = 0;
	if (strchr(mode, '+') != nullptr)
		dwDesiredAccess = GENERIC_READ | GENERIC_WRITE;
	else if (strchr(mode, 'r') != nullptr)
	{
		openFlags |= _O_RDONLY;
		dwDesiredAccess = GENERIC_READ;
	}
	else
		dwDesiredAccess = GENERIC_WRITE;
	if (strchr(mode, 'w') != nullptr)
		dwCreationDisposition = CREATE_ALWAYS;
	else if (strchr(mode, 'a') != nullptr)
	{
		dwCreationDisposition = OPEN_ALWAYS;
		openFlags |= _O_APPEND;
	}
	else
		dwCreationDisposition = OPEN_EXISTING;
	if (strchr(mode, 'b') == nullptr)
		openFlags |= _O_TEXT;

	HANDLE fileh = CreateFile2FromAppW(wname.get(), dwDesiredAccess, FILE_SHARE_READ, dwCreationDisposition, nullptr);
	if (fileh == INVALID_HANDLE_VALUE)
		return nullptr;

	int fd = _open_osfhandle((intptr_t)fileh, openFlags);
	if (fd == -1)
	{
		WARN_LOG(COMMON, "_open_osfhandle failed");
		CloseHandle(fileh);
		return nullptr;
	}

	return _fdopen(fd, mode);
}

int remove(char const *name)
{
    wstackstring wname;
    if(!wname.convert(name)) {
        errno = EINVAL;
        return -1;
    }
    return _wremove(wname.get());
}

}
#endif

int main(int argc, char* argv[])
{
	nowide::args _(argc, argv);

	// MapleCast: attach console so printf output is visible.
	// Always attach for the wizard mode — it's a console-only experience.
	if (std::getenv("MAPLECAST") || std::getenv("MAPLECAST_BUTTON_WIZARD"))
	{
		AllocConsole();
		freopen("CONOUT$", "w", stdout);
		freopen("CONOUT$", "w", stderr);
		printf("[maplecast] console attached\n");
		fflush(stdout);
	}

	// MapleCast button wizard — runs early, before flycast initializes.
	// The user runs once with MAPLECAST_BUTTON_WIZARD=1 to set up their
	// XInput button mapping. The wizard saves to xinput-map.cfg and
	// exits cleanly; subsequent launches load the file automatically.
	if (std::getenv("MAPLECAST_BUTTON_WIZARD")) {
		maplecast_rawinput::runWizardStandalone();
		printf("[wizard] Done. Relaunch without MAPLECAST_BUTTON_WIZARD to play.\n");
		printf("Press Enter to close this window...\n");
		fflush(stdout);
		(void)getchar();
		return 0;
	}

#ifdef USE_BREAKPAD
	wchar_t tempDir[MAX_PATH + 1];
	GetTempPathW(MAX_PATH + 1, tempDir);

	static google_breakpad::CustomInfoEntry custom_entries[] = {
			google_breakpad::CustomInfoEntry(L"prod", L"Flycast"),
			google_breakpad::CustomInfoEntry(L"ver", L"" GIT_VERSION),
	};
	google_breakpad::CustomClientInfo custom_info = { custom_entries, std::size(custom_entries) };

	google_breakpad::ExceptionHandler handler(tempDir,
		nullptr,
		dumpCallback,
		nullptr,
		google_breakpad::ExceptionHandler::HANDLER_ALL,
		MiniDumpNormal,
		INVALID_HANDLE_VALUE,
		&custom_info);
	// crash on die() and failing verify()
	handler.set_handle_debug_exceptions(true);
#endif

#if defined(_WIN32) && defined(LOG_TO_PTY)
	setbuf(stderr, NULL);
#endif
	LogManager::Init();
	i18n::init();

	reserveBottomMemory();
	setupPath();
	findKeyboardLayout();

	if (flycast_init(argc, argv) != 0)
		die("Flycast initialization failed");

	// MapleCast: bump entire process to HIGH_PRIORITY_CLASS so input/render
	// threads win scheduling vs background OS work (search indexer, telemetry,
	// AV scans, etc.). REALTIME_PRIORITY_CLASS is intentionally NOT used —
	// a runaway spin would lock the machine. HIGH is safe and effective.
	// Skipped on builds that don't run a foreground UI (none today, but the
	// guard is there).
	if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
		printf("[maplecast] SetPriorityClass(HIGH) failed: %lu\n", GetLastError());
	else
		printf("[maplecast] process priority class: HIGH\n");

	// MAPLECAST_TOURNAMENT_MODE — opt-in latency tightening for competitive play.
	// Process-level only; per-thread MMCSS / RawInput / DXGI Independent Flip are
	// separate larger pieces tracked in docs/OPTIMIZATION-PLAN.md #5.2 and #5.3.
	if (std::getenv("MAPLECAST_TOURNAMENT_MODE")) {
		// Lock working-set bounds so the OS doesn't trim pages out from under
		// us during a frame. ~1-2 page-fault stalls per minute otherwise; each
		// is 100µs-1ms of jitter that shows up as dropped frames in the HUD.
		// Bounds chosen for the ~14 MB binary + ~50 MB savestate + ~60 MB GL
		// state + headroom. Process actually uses ~100 MB peak.
		SIZE_T minWS = 192 * 1024 * 1024;
		SIZE_T maxWS = 384 * 1024 * 1024;
		if (SetProcessWorkingSetSize(GetCurrentProcess(), minWS, maxWS))
			printf("[tournament-mode] working set locked %zu-%zu MB (no page-fault stalls)\n",
			       minWS >> 20, maxWS >> 20);
		else
			printf("[tournament-mode] SetProcessWorkingSetSize failed: %lu (need SeIncreaseWorkingSetPrivilege)\n",
			       GetLastError());

		// Disable priority boost — the kernel's auto-boost on I/O can cause
		// short bursts where threads run at unexpectedly high priority,
		// followed by drops. Predictable scheduling > opportunistic boosts.
		SetProcessPriorityBoost(GetCurrentProcess(), TRUE);  // TRUE = DISABLE boost

		printf("[tournament-mode] enabled — recommend also: powercfg Ultimate Performance, NVIDIA Ultra Low Latency, Defender exclusion for flycast.exe\n");
	}

	// MapleCast mirror client auto-load — mirrors core/linux-dist/main.cpp:269-296.
	// Two paths trigger romless mirror mode:
	//   1. --server <host>[:<port>]  command-line flag
	//   2. MAPLECAST_MIRROR_CLIENT=1 env var (with no positional ROM arg)
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
			std::string addr = argv[i + 1];
			std::string host = addr;
			std::string port = "7200";
			auto colon = addr.rfind(':');
			if (colon != std::string::npos) {
				host = addr.substr(0, colon);
				port = addr.substr(colon + 1);
			}
			_putenv_s("MAPLECAST_MIRROR_CLIENT", "1");
			_putenv_s("MAPLECAST_SERVER_HOST", host.c_str());
			_putenv_s("MAPLECAST_SERVER_PORT", port.c_str());
			printf("[MIRROR] --server %s:%s\n", host.c_str(), port.c_str());
			fflush(stdout);
			emu.loadGame(nullptr);
			break;
		}
	}
	if (nowide::getenv("MAPLECAST_MIRROR_CLIENT") && argc < 2) {
		printf("[MIRROR] Auto-loading without ROM\n");
		fflush(stdout);
		emu.loadGame(nullptr);
	}

#ifdef USE_BREAKPAD
	nowide::stackstring nws;
	static std::string tempDir8;
	if (nws.convert(tempDir))
		tempDir8 = nws.get();
	auto async = std::async(std::launch::async, uploadCrashes, tempDir8);
#endif

#ifdef TARGET_UWP
	if (config::ContentPath.get().empty())
		config::ContentPath.get().push_back(get_writable_config_path(""));
#endif
	os_InstallFaultHandler();

	try {
		mainui_loop();
	} catch (const std::exception& e) {
		ERROR_LOG(BOOT, "mainui_loop error: %s", e.what());
#ifndef TARGET_UWP
		MessageBox(NULL, i18n::T("Flycast Error"), e.what(), MB_ICONSTOP | MB_OK);
#endif
	} catch (...) {
		ERROR_LOG(BOOT, "mainui_loop unknown exception");
	}

	flycast_term();
	os_UninstallFaultHandler();

	return 0;
}

[[noreturn]] void os_DebugBreak()
{
	__debugbreak();
	std::abort();
}

void os_DoEvents()
{
	FC_PROFILE_SCOPE;

#ifndef TARGET_UWP
	MSG msg;
	while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
	{
		// If the message is WM_QUIT, exit the while loop
		if (msg.message == WM_QUIT)
		{
			dc_exit();
		}

		// Translate the message and dispatch it to WindowProc()
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
#endif
}

void os_RunInstance(int argc, const char *argv[])
{
	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(NULL, exePath, std::size(exePath));

	std::wstring cmdLine = L'"' + std::wstring(exePath) + L'"';
	for (int i = 0; i < argc; i++)
	{
		nowide::wstackstring wname;
		if (!wname.convert(argv[i])) {
			WARN_LOG(BOOT, "Invalid argument: %s", argv[i]);
			continue;
		}
		cmdLine += L" \"";
		for (wchar_t *p = wname.get(); *p != L'\0'; p++)
		{
			cmdLine += *p;
			if (*p == L'"')
				// escape double quote
				cmdLine += L'"';
		}
		cmdLine += L'"';
	}

	STARTUPINFOW startupInfo{};
	startupInfo.cb = sizeof(startupInfo);

	PROCESS_INFORMATION processInfo{};
	BOOL rc = CreateProcessW(exePath, (wchar_t *)cmdLine.c_str(), nullptr, nullptr, true, 0, nullptr, nullptr, &startupInfo, &processInfo);
	if (rc)
	{
		CloseHandle(processInfo.hProcess);
		CloseHandle(processInfo.hThread);
	}
	else
	{
		WARN_LOG(BOOT, "Cannot launch Flycast instance: error %d", GetLastError());
	}
}

static WinLibLoader kernelBaseLib("KernelBase.dll");

void os_SetThreadName(const char *name)
{
	nowide::wstackstring wname;
	if (wname.convert(name))
	{
		static HRESULT (WINAPI *SetThreadDescription)(HANDLE, PCWSTR) = kernelBaseLib.getFunc("SetThreadDescription", SetThreadDescription);
		if (SetThreadDescription != nullptr)
			SetThreadDescription(GetCurrentThread(), wname.get());
	}
}

const char *getThreadName()
{
	static HRESULT (WINAPI *GetThreadDescription)(HANDLE, PWSTR *) = kernelBaseLib.getFunc("GetThreadDescription", GetThreadDescription);
	if (GetThreadDescription == nullptr)
		return "?";
	PWSTR wname = nullptr;
	if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &wname)))
	{
		nowide::stackstring stname;
		thread_local std::string name;
		if (stname.convert(wname))
			name = stname.get();
		else
			name = "?";
		LocalFree(wname);
		return name.c_str();
	}
	else {
		return "?";
	}
}

#ifdef VIDEO_ROUTING
#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "SpoutSender.h"
#include "SpoutDX.h"
#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif

static SpoutSender* spoutSender;
static spoutDX* spoutDXSender;

void os_VideoRoutingPublishFrameTexture(GLuint texID, GLuint texTarget, float w, float h)
{
	if (spoutSender == nullptr)
	{
		spoutSender = new SpoutSender();
		int boardID = config::loadInt("naomi", "BoardId");
		char buf[32] = { 0 };
		vsnprintf(buf, sizeof(buf), (boardID == 0 ? "Flycast - Video Content" : "Flycast - Video Content - %d"), std::va_list(&boardID));
		spoutSender->SetSenderName(buf);
	}	
	spoutSender->SendTexture(texID, texTarget, w, h, true);
}

void os_VideoRoutingTermGL()
{
	if (spoutSender) 
	{
		spoutSender->ReleaseSender();
		spoutSender = nullptr;
	}
}

void os_VideoRoutingPublishFrameTexture(ID3D11Texture2D* pTexture)
{
	if (spoutDXSender == nullptr)
	{
		spoutDXSender = new spoutDX();
		ID3D11Resource* resource = nullptr;
		HRESULT hr = pTexture->QueryInterface(__uuidof(ID3D11Resource), reinterpret_cast<void**>(&resource));
		if (SUCCEEDED(hr))
		{
			ID3D11Device* pDevice = nullptr;
			resource->GetDevice(&pDevice);
			resource->Release();
			spoutDXSender->OpenDirectX11(pDevice);
			pDevice->Release();
			int boardID = config::loadInt("naomi", "BoardId");
			char buf[32] = { 0 };
			vsnprintf(buf, sizeof(buf), (boardID == 0 ? "Flycast - Video Content" : "Flycast - Video Content - %d"), std::va_list(&boardID));
			spoutDXSender->SetSenderName(buf);
		}
		else
		{
			return;
		}
	}
	spoutDXSender->SendTexture(pTexture);
}

void os_VideoRoutingTermDX()
{
	if (spoutDXSender)
	{
		spoutDXSender->ReleaseSender();
		spoutDXSender = nullptr;
	}
}
#endif
