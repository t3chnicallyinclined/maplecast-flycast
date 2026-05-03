# MapleCast — Native Windows Mirror Client Build

Build a native `flycast.exe` on Windows that runs in mirror-client mode and connects to a remote MapleCast server (e.g. `nobd.net`). The client receives the byte-perfect TA mirror stream over WebSocket, decompresses with zstd, and renders MVC2 pixel-perfect via WebGL/OpenGL — same flow as a Linux mirror client. No NVENC, no WebRTC, no OpenSSL, no DirectX 9 SDK.

This is the build mode added by `MAPLECAST_CLIENT_ONLY=ON`. It excludes the server-side streaming paths (NVENC H.264 encoder, libdatachannel WebRTC, DX9 backend) so the binary builds cleanly on a stock Windows dev environment with vcpkg-managed libcurl as the only external dep.

---

## What you get

A ~14 MB `flycast.exe` that, when launched with the right env vars:

1. Skips ROM/BIOS loading (you don't need MVC2 on disk locally).
2. Queries the hub at `https://nobd.net/hub/api/input-servers/nearby` via libcurl HTTPS.
3. UDP-probes each candidate input server, picks the lowest-RTT one.
4. Opens a WebSocket to the chosen server's flycast WS port (7200, direct — bypasses the spectator relay for lower latency, per the [competitive-client](COMPETITIVE-CLIENT.md) design).
5. Receives the cached SYNC frame (~8 MB VRAM + PVR, zstd-compressed to ~0.5 MB).
6. Streams delta TA frames at 60fps, renders pixel-perfect locally.
7. Sends gamepad input over UDP:7100 to the same input server (with redundant T+1ms sends).
8. Plays audio via a separate WebSocket on port 7203.

Verified end-to-end against `nobd.net` with 5.4 ms RTT and 16× SYNC compression.

---

## Prerequisites

| What | Version | Why |
|---|---|---|
| Visual Studio 2022 Build Tools (or full VS 2022) | 14.44+ (MSVC v143) | Compiler, linker, Windows SDK |
| Windows SDK | 10.0.22000+ | Headers, ws2_32.lib |
| CMake | 3.27+ | Build configure |
| Ninja | 1.10+ | Build driver |
| Git | any modern | Clone + submodules |
| vcpkg | any | Provides libcurl |

You do **not** need:
- ❌ NVIDIA CUDA Toolkit
- ❌ NVIDIA Video Codec SDK / nvEncodeAPI.h
- ❌ OpenSSL (libdatachannel is excluded)
- ❌ June 2010 DirectX SDK (`d3dx9shader.h`)
- ❌ A local MVC2 ROM (the server has the ROM)

---

## One-time setup

### 1. Install build tools

If you don't already have MSVC + CMake + Ninja:

```powershell
# CMake from https://cmake.org/download/ or:
winget install Kitware.CMake
# Ninja:
winget install Ninja-build.Ninja
# MSVC Build Tools (community standalone):
# https://visualstudio.microsoft.com/downloads/ → "Tools for Visual Studio" → Build Tools for VS 2022
```

Confirm:
```powershell
cmake --version       # >= 3.27
ninja --version       # >= 1.10
# vcvars64.bat lives at:
# C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
```

### 2. Bootstrap vcpkg

vcpkg is the package manager that provides libcurl. Install it in your home directory (no admin needed):

```powershell
cd C:\Users\$env:USERNAME
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat -disableMetrics
```

You should now have `C:\Users\<you>\vcpkg\vcpkg.exe`.

### 3. Install libcurl via vcpkg

```powershell
C:\Users\$env:USERNAME\vcpkg\vcpkg.exe install curl:x64-windows
```

Takes ~20 seconds (vcpkg pulls a binary cache). Verify the install:

```powershell
Test-Path "C:\Users\$env:USERNAME\vcpkg\installed\x64-windows\include\curl\curl.h"      # True
Test-Path "C:\Users\$env:USERNAME\vcpkg\installed\x64-windows\lib\libcurl.lib"          # True
Test-Path "C:\Users\$env:USERNAME\vcpkg\installed\x64-windows\bin\libcurl.dll"          # True
```

### 4. Clone maplecast-flycast

```powershell
cd C:\Users\$env:USERNAME\projects   # or wherever
git clone https://github.com/t3chnicallyinclined/maplecast-flycast.git
cd maplecast-flycast
git submodule update --init --recursive
```

`mvc2-randomizer-temp` will warn and skip — that's fine; it's an upstream gitlink without a `.gitmodules` entry, doesn't block the build.

---

## Build

From the project root in **a regular PowerShell** (not a developer prompt — vcvars is invoked inline):

```powershell
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$vcpkg  = "C:\Users\$env:USERNAME\vcpkg"

# Configure (one-time, or after CMakeLists.txt changes)
& cmd /c "`"$vcvars`" >nul && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMAPLECAST_CLIENT_ONLY=ON -DCMAKE_TOOLCHAIN_FILE=$vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows"

# Build
& cmd /c "`"$vcvars`" >nul && cmake --build build --config Release"
```

Configure takes ~70 seconds (lots of submodule subprojects). Build takes 3-10 min depending on cores; Ninja parallelizes automatically.

A successful configure prints:

```
-- MapleCast: CLIENT-ONLY build — no NVENC, no WebRTC, no libdatachannel/OpenSSL/DX9 deps
-- Found OpenSSL: ...               # (false alarm — OpenSSL detected on system but unused)
-- Found CURL: ...
CMake Warning at CMakeLists.txt:1764 (message):
  MapleCast: libxdp/libbpf not found — XDP input disabled, using fallback
CMake Warning at CMakeLists.txt:1783 (message):
  MapleCast: libdatachannel not found — WebRTC disabled, WebSocket-only
-- MapleCast: TA streaming enabled — visual cache recording active
-- Configuring done
```

A successful build ends with `flycast.exe` at `build/flycast.exe`.

### Stage runtime DLLs

vcpkg's `libcurl.dll` needs to be next to the binary at runtime. CMake doesn't copy it automatically:

```powershell
Copy-Item "$vcpkg\installed\x64-windows\bin\libcurl.dll" "build\" -Force
# Optional dependency DLLs from vcpkg:
Copy-Item "$vcpkg\installed\x64-windows\bin\zlib1.dll" "build\" -Force -ErrorAction SilentlyContinue
```

---

## Run

```powershell
$env:MAPLECAST=1                                    # attach console for printf output
$env:MAPLECAST_MIRROR_CLIENT=1                      # boot in mirror-client mode (no ROM)
$env:MAPLECAST_HUB_URL="https://nobd.net/hub/api"   # let hub discovery pick the server
& "C:\Users\$env:USERNAME\projects\maplecast-flycast\build\flycast.exe"
```

A console window pops alongside the Flycast window with all the diagnostic output.

### Alternative launch modes

**Direct connection (skip hub discovery):**
```powershell
$env:MAPLECAST_MIRROR_CLIENT=1
$env:MAPLECAST_SERVER_HOST="nobd.net"
$env:MAPLECAST_SERVER_PORT="7200"   # direct flycast WS — players
& "build\flycast.exe"
```

**Spectator (use the relay instead of direct):**
```powershell
$env:MAPLECAST_MIRROR_CLIENT=1
$env:MAPLECAST_HUB_URL="https://nobd.net/hub/api"
$env:MAPLECAST_SPECTATE=1   # routes via relay, doesn't claim a player slot
& "build\flycast.exe"
```

**Log to file instead of console window:**
```powershell
$env:MAPLECAST_MIRROR_CLIENT=1
$env:MAPLECAST_HUB_URL="https://nobd.net/hub/api"
& "build\flycast.exe" *> mirror.log
```

---

## What you should see in the console

The successful-launch fingerprint:

```
[maplecast] console attached
[MIRROR] Auto-loading without ROM
[MIRROR] No ROM — initializing renderer only
[input-server] === READY === port 7100
[MIRROR] Hub discovery enabled — querying https://nobd.net/hub/api
[hub-discovery] Discovered N input server(s) from https://nobd.net/hub/api
[hub-discovery] probe nobd-main: X.Xms avg (4 samples)
[MIRROR] Hub picked input server 'nobd-main' at nobd.net:7201 (X.Xms RTT)
[MIRROR] === CLIENT MODE (WebSocket) === ws://nobd.net:7200/
[MIRROR-WS] Connecting to nobd.net:7200...
[audio-client] thread started, target ws://nobd.net:7203/
[control-ws] listening on ws://127.0.0.1:7211 (loopback only)
[input-sink] ready → <ip>:7100 slot 0 (19-byte seq+ts + redundant send)
[MIRROR] === CLIENT MODE === CPU stopped, renderer-only, 16 texture threads
[MIRROR-WS] TCP connected (NODELAY)
[MIRROR-WS] WebSocket handshake OK — waiting for initial sync
[audio-client] connected, streaming PCM (pre-buffering 6 packets)
[MIRROR-WS] Initial sync received: 8.0 MB (0.5 MB compressed) — VRAM + PVR loaded
[audio-client] pre-buffer filled (6 packets, ~69 ms), playback active
[MIRROR-WS] First frame decoded
```

If you see `[MIRROR-WS] Initial sync received` and `First frame decoded`, you're connected and rendering.

The hub returns the relay port (7201); the native client subtracts 1 to connect to flycast's direct WS port (7200). This is intentional — the relay is for spectator fan-out, players skip it for lower latency. See [maplecast_mirror.cpp](../core/network/maplecast_mirror.cpp) `initClient()` for the logic.

---

## In-game keys

| Key | Toggles |
|---|---|
| **`** (backtick) | MapleCast settings panel — button mapping, audio, network options |
| **F1** | Network HUD — server name, RTT, loss, jitter, network grade |
| **F2** | Latency HUD — input poll → wire → frame → pixel breakdown |
| **F3** | Input HUD — send rate, redundant sends, failover count, primary/backup status |
| **F4** | Combo trainer note highway |
| **F6** | In-game input overlay (button display) |
| **F12** | Toggle ALL HUD elements |

`Tab` is intentionally **disabled** in mirror-client mode (it conflicts with the HP fightstick button). Use the gear icon or backtick instead.

The diagnostic HUD reads from `maplecast_input_sink::getStats()` — if F1/F3 show real RTT and send rates, gamepad input is going through the MapleCast input pipeline (not the legacy SDL→ggpo path).

---

## Ports the client uses

All outbound. No inbound listeners except `127.0.0.1:7211` (loopback, for the local overlay panel).

| Direction | Protocol | Target | Purpose |
|---|---|---|---|
| Outbound | HTTPS | `nobd.net:443` | Hub discovery (libcurl) |
| Outbound | WS | `nobd.net:7200` | TA stream (mirror frames) |
| Outbound | WS | `nobd.net:7203` | Audio PCM stream |
| Outbound | UDP | `nobd.net:7100` | Gamepad input (redundant T+0/T+1ms) |
| Inbound | TCP | `127.0.0.1:7211` | Local overlay control WS (loopback only) |

No firewall configuration needed beyond standard outbound permission.

---

## Troubleshooting

**Console window doesn't appear:**
You forgot `$env:MAPLECAST=1`. Without that env var, `winmain.cpp` skips `AllocConsole()` and stdout goes nowhere.

**Black window, no game:**
Check the console for `[MIRROR-WS]` errors. Common causes:
- Hub URL unreachable: check internet, check the URL exactly matches what nobd.net exposes
- WebSocket handshake fails: server is down, or your network blocks port 7200
- No SYNC frame within ~10s: server is alive but not sending — restart your client

**`libcurl.dll was not found`:**
You forgot to copy `libcurl.dll` from vcpkg's `bin/` to `build/`. See "Stage runtime DLLs" above.

**Configure fails with `Could NOT find CURL`:**
The `-DCMAKE_TOOLCHAIN_FILE=...vcpkg.cmake` flag wasn't picked up. Re-run configure with the explicit path. If you still see the error, run `vcpkg list` to confirm `curl:x64-windows` is installed.

**Build fails with `Cannot open include file: 'd3dx9shader.h'`:**
You configured without `-DMAPLECAST_CLIENT_ONLY=ON`. The default build pulls in DX9 which requires the deprecated June 2010 DirectX SDK. Reconfigure with the flag.

**Build fails with `Cannot open input file 'dl.lib'`:**
Same root cause — `MAPLECAST_CLIENT_ONLY` not set. The default build tries to link the CUDA block which adds Linux-only `dl`.

**Console shows `[hub-discovery] HTTP GET ... failed: Could not resolve host`:**
DNS issue, or HTTPS being blocked. Test with `curl https://nobd.net/hub/api/input-servers/nearby` from the same shell.

**Game window opens but freezes after first frame:**
Audio backend may have stalled — try `$env:MAPLECAST_NO_AUDIO=1` (if implemented) or kill the audio thread by checking `[audio-client]` errors in the console.

---

## Known limitations

- **No Raw Input bypass on Windows yet** — gamepad goes through SDL, which adds ~1-3ms vs the Linux evdev direct path. SDL is fine for desktop play; Raw Input would be a future improvement.
- **No SCHED_FIFO equivalent** — input/audio threads run at default Windows priority. The Linux client gets RT-FIFO scheduling for the SH4 thread (irrelevant on the client) and the input sink. On Windows we don't currently use `SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL)` — could be added.
- **No XDP zero-copy input** — Linux-only feature, irrelevant for clients (the client *sends* gamepad UDP, server *receives* with XDP).
- **No mmap'd `/dev/shm` same-box optimization** — Linux-only, irrelevant for remote clients connecting to nobd.net.
- **Comment encoding noise** — During the port, some Unicode characters in C++ comments (box-drawing `├─`, em-dashes, `µ` in latency comments) got re-encoded by PowerShell's UTF-8 save into multi-byte sequences. Code is correct, comments look mangled. Cosmetic; can be cleaned up in a future pass.

---

## How this relates to the other build modes

| Build flag | Binary | Use case |
|---|---|---|
| (default — no flag) | `flycast.exe` / `flycast` | Everything-binary: emulator + server + client. Linux dev workstations, physical cab. |
| `-DMAPLECAST_HEADLESS=ON` | `flycast` (no GUI) | VPS production server. ~26 MB stripped. No GPU/SDL/X11/audio. Pillars 1-3 (emulator + input server + stream server). |
| `-DMAPLECAST_CLIENT_ONLY=ON` | `flycast.exe` (Windows) | Native Windows mirror-client desktop. Renderer + mirror networking only. No NVENC, no WebRTC, no DX9. |

The three modes are mutually exclusive at build time — they produce different binaries with different deps. Source tree is shared.

---

## Related docs

- [ARCHITECTURE.md](ARCHITECTURE.md) — five-pillar mental model, wire protocol, byte-perfect determinism guarantee
- [COMPETITIVE-CLIENT.md](COMPETITIVE-CLIENT.md) — design of the native mirror-client experience (multi-server failover, replay recording, HUD)
- [DEPLOYMENT.md](DEPLOYMENT.md) — VPS server deployment + the variant table
- [INPUT-LATCH.md](INPUT-LATCH.md) — dual-policy input latching that the client respects
