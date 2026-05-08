# MapleCast — Native Windows Headless Build

Build a native `flycast.exe` on Windows that runs the **same** headless server build that powers the production VPS — full SH4 + AICA emulation, mirror server (TA stream over WebSocket), input server (UDP) — but **no GUI, no renderer, no SDL**. Same architecture, same protocols, same wire format. Only the binary's deployment context changes.

This is the foundation of [Phase 1 rollback prediction](ROLLBACK-PREDICTION.md): a competitive player runs the headless predictor on their own machine while the existing [Windows mirror client](WINDOWS-CLIENT-BUILD.md) (renderer-only) connects to `127.0.0.1:7200`. The user gets sub-millisecond input latency because the SH4 is computing locally; the mirror client renders whatever the local SH4 produces.

> **Why a separate build mode from `MAPLECAST_CLIENT_ONLY`?** The mirror-client build is renderer-only; it explicitly **stops the SH4** to avoid running an emulator alongside the wire-stream decoder. The headless build does the opposite — runs the full SH4, no renderer. Two opposite shapes, two opposite build flags. Use both side-by-side for the local-rollback topology.

---

## What you get

A ~9 MB `flycast.exe` that:

1. **Loads MVC2** from a local `.gdi` (the headless predictor needs the ROM locally — unlike mirror clients which never see the ROM).
2. **Runs full SH4 emulation** including AICA audio, JVS / maple, dynarec, fault-handled vmem.
3. **Hosts the mirror server** on `0.0.0.0:7200` (TA + memory-diff WS broadcast), `0.0.0.0:7203` (audio WS), `0.0.0.0:7102` (state-sync TCP).
4. **Hosts the input server** on `0.0.0.0:7100` (UDP, gamepad input from the local mirror client), `0.0.0.0:7101` (UDP tape, frame-stamped input log for replicas).
5. **Logs to console** (binary is `/SUBSYSTEM:CONSOLE`, not WinMain — visible logs without `AllocConsole`).

Identical wire format as the Linux server. A mirror client connecting to this binary cannot tell whether it's hitting prod or localhost.

---

## Prerequisites

Same as the [mirror-client build](WINDOWS-CLIENT-BUILD.md):

| What | Version | Why |
|---|---|---|
| Visual Studio 2022 Build Tools | 14.44+ (MSVC v143) | Compiler, linker, Windows SDK |
| CMake | 3.27+ | Build configure |
| Ninja | 1.10+ | Build driver |
| Git | any | Clone + submodules |
| vcpkg | any | Provides libcurl, libzip, libzstd |

You do **not** need:
- ❌ SDL2 (excluded — no window)
- ❌ DirectX (excluded — no renderer)
- ❌ NVENC / WebRTC / OpenSSL (excluded — same as mirror-client build)

---

## Build

From the project root in PowerShell:

```powershell
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$vcpkg  = "C:\Users\trist\vcpkg"   # adjust to your install

& cmd /c "`"$vcvars`" >nul && cmake -S . -B build-headless-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DMAPLECAST_HEADLESS=ON -DCMAKE_TOOLCHAIN_FILE=$vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows"

& cmd /c "`"$vcvars`" >nul && cmake --build build-headless-win --target flycast -j 4"
```

Output: `build-headless-win\flycast.exe`.

---

## Run

```powershell
$env:MAPLECAST=1
$env:MAPLECAST_MIRROR_SERVER=1
$env:MAPLECAST_HEADLESS=1
$env:MAPLECAST_PORT=7100
$env:MAPLECAST_SERVER_PORT=7200
$env:MAPLECAST_HEADLESS_AUTOLOAD=1   # optional — autoload a savestate at boot

.\build-headless-win\flycast.exe "<path-to-mvc2.gdi>"
```

Same env vars as the Linux systemd unit (`/etc/systemd/system/maplecast-headless.service`). A fresh launch should print:

```
[maplecast-headless] process priority class: HIGH
[input-server] === READY === port 7100
[MIRROR] Windows: SHM backed by private heap (no cross-process share)
[maplecast-ws] WebSocket server on ws://0.0.0.0:7200 (mirror + lobby)
[maplecast-audio-ws] audio-only WebSocket server on ws://0.0.0.0:7203
[state-sync] === SERVER READY === port 7102
[MIRROR] === SERVER MODE === streaming TA + memory diffs
[MIRROR] Server frame 600 | TA=... | dirty pages | zstd ...
```

Verify ports with `netstat -ano | findstr "7100 7200 7102"`.

---

## Connect a mirror client to localhost

In a second PowerShell:

```powershell
$env:MAPLECAST_MIRROR_CLIENT=1
$env:MAPLECAST_SERVER_HOST="127.0.0.1"
$env:MAPLECAST_SERVER_PORT="7200"

.\build\flycast.exe
```

Same launch as for connecting to a remote server, just localhost. The mirror client doesn't know or care that the server is on the same machine.

---

## Savestates

The headless build's working directory is `build-headless-win\`. Savestate path is `build-headless-win\data\<rom-basename>.state`. To use an existing savestate (e.g., a copy from prod with all characters unlocked):

```powershell
# rom-basename matches the ROM filename without extension
# e.g., "Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!]" → state file at
#       build-headless-win\data\Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].state

scp <user>@<server>:/opt/maplecast/.local/share/flycast/mvc2.state `
    "build-headless-win\data\<rom-basename>.state"
```

Set `MAPLECAST_HEADLESS_AUTOLOAD=1` to force `dc_loadstate` at boot regardless of GUI config.

---

## Implementation notes

### `core/windows/main_headless.cpp` — the entry point

Console-mode `int main()` that mirrors `core/linux-dist/main.cpp`'s headless block step-for-step:

1. `LogManager::Init()`
2. `i18n::init()`
3. `setupPath()` — sets `user_config_dir` and `user_data_dir` relative to the binary (matching `winmain.cpp`'s logic so flycast finds its files identically)
4. `flycast_init(argc, argv)` — same as on Linux
5. `SetPriorityClass(HIGH_PRIORITY_CLASS)` — same as `winmain.cpp`
6. **`os_InstallFaultHandler()`** — critical. Without this, the SH4 dynarec's first vmem-faulting guest-memory access raises `STATUS_ACCESS_VIOLATION` and Windows kills the process silently. Same call site as `winmain.cpp:462`.
7. `emu.loadGame(rom_path)` + `emu.start()` + `gui_setState(Closed)` — identical to the Linux headless block in `linux-dist/main.cpp`
8. `mainui_loop(true)` — drives the render-thread loop (which is a no-op-renderer no-op since `imguiDriver == nullptr` in headless, but must be called to drive `emu.render()` and the mirror server publish loop)
9. `os_UninstallFaultHandler()` + `flycast_term()`

The file also stubs `os_SetThreadName` / `getThreadName` / `rawinput::init/term` / `maplecast_rawinput::init` because their full implementations live in `winmain.cpp` / `rawinput.cpp` — both excluded from this build.

### `openShm` Windows fallback

Linux uses `/dev/shm` so a separate relay process can read the ring buffer without TCP. Windows has no relay process and no `/dev/shm`, but the rest of the mirror server (`serverPublish` ring writes, WS broadcast) still expects `_shmPtr` to be a valid buffer. The fallback allocates a private `malloc(SHM_SIZE)` heap buffer — same in-process layout, no cross-process sharing. **Caller logic is identical on both platforms**; the divergence is contained to the four lines inside `openShm`'s `#ifdef _WIN32` block.

### CMakeLists.txt gates

When `MAPLECAST_HEADLESS=ON` on Windows:
- `winmain.cpp` is replaced with `main_headless.cpp`
- `rawinput.cpp` and `rawinput_gamepad.cpp` are excluded (they need `getNativeHwnd()` from the GUI layer)
- `audiobackend_directsound.cpp` is excluded (also needs `getNativeHwnd()`)
- `LINK_FLAGS = "/SUBSYSTEM:CONSOLE /ENTRY:mainCRTStartup"` so the binary uses `main()` not `WinMain`

All other source files are shared with the GUI client and the Linux headless build.

---

## Differences from the Linux headless build

The two builds use **identical** caller logic; only platform-specific implementations diverge:

| Concern | Linux | Windows |
|---|---|---|
| Entry point | `core/linux-dist/main.cpp` (`int main()`) | `core/windows/main_headless.cpp` (`int main()`) |
| Path discovery | `find_user_config_dir()` (XDG paths, env) | `setupPath()` (paths relative to binary, matches `winmain.cpp`) |
| Fault handler installation site | `common_linux_setup()` (POSIX `sigaction`) | `os_InstallFaultHandler()` direct (Win32 SEH) |
| SHM backend | `mmap(/dev/shm)` for cross-process relay | `malloc(SHM_SIZE)` private heap |
| Thread priority | systemd `Nice=-15` | `SetPriorityClass(HIGH_PRIORITY_CLASS)` |
| Audio backend | null (no real audio output by design) | null (same — explicit override in headless) |

Same protocols, same env vars, same default ports, same wire format.
