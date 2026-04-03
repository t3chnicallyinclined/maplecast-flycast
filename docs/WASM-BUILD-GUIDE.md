# MapleCast — Complete Environment Setup & Agent Handoff

**Last updated:** 2026-04-03
**Status:** All repos pushed. Ready to clone and build on Ubuntu.

---

## Overview

MapleCast is an online gaming platform for Dreamcast (MVC2 first). Three major components:

1. **maplecast-flycast** — Flycast emulator fork with streaming, input relay, game state reading, CUDA/NVENC video encode, WebSocket server
2. **flycast-wasm** — Flycast compiled to WebAssembly for pixel-perfect browser rendering (no video encode needed)
3. **GP2040-CE-NOBD** — Fightstick firmware (RP2040) with W6100 Ethernet, sends gamepad state to server

### Target Architecture

```
Server (native Flycast norend):     Browser (Flycast WASM):
┌──────────────────────┐            ┌──────────────────────┐
│ Game logic only       │◄──WS────►│ Full emulator + GPU   │
│ Sends synced inputs   │  253B/fr  │ WebGL2 rendering      │
│ No GPU needed         │           │ Pixel-perfect MVC2    │
└──────────────────────┘            └──────────────────────┘
      ▲                                    ▲
  [GP2040-CE W6100]                   [Browser Gamepad API]
```

### What Already Works (proven on Windows)

| Feature | Status | Details |
|---------|--------|---------|
| H.264 video streaming | WORKING | CUDA zero-copy + NVENC, 3ms pipeline, 60fps |
| Game state streaming | WORKING | 253 bytes/frame, 4kbps, 0ms encode/decode |
| WebSocket server in Flycast | WORKING | ws://localhost:7200, no proxy needed |
| UDP gamepad input | WORKING | P1/P2 auto-assignment, W3 packet format |
| Web app | WORKING | Video/sprites toggle, diagnostics, Gamepad API 250Hz |
| 56 character sprite sheets | WORKING | Extracted from MVC2, in web/sprites/ |
| TA display list UV capture | PARTIAL | Extracts UV coords from PowerVR2 render pipeline |
| Flycast WASM (browser) | NOT BUILT YET | This is the next step |

---

## ALL REPOS

| # | Repo URL | Branch | What | Needed? |
|---|----------|--------|------|---------|
| 1 | `https://github.com/t3chnicallyinclined/maplecast-flycast.git` | `maplecast` | **THE MAIN PROJECT** — Flycast fork with all MapleCast code | YES — primary |
| 2 | `https://github.com/nasomers/flycast-wasm.git` | `wasm-jit` | Flycast WASM JIT for browser rendering | YES — browser emulator |
| 3 | `https://github.com/t3chnicallyinclined/GP2040-CE-NOBD.git` | `w6100-evb-pico2` | Fightstick firmware (RP2040 + W6100 Ethernet) | YES — fightstick firmware |
| 4 | `https://github.com/t3chnicallyinclined/maplecast-server.git` | `master` | Rust relay server for online play | OPTIONAL — Phase 0 artifact |
| 5 | `https://github.com/t3chnicallyinclined/finger-gap-tester.git` | (default) | Input timing test tool | OPTIONAL — testing tool |
| 6 | `https://github.com/karttoon/mvc2-skin-processor.git` | (default) | MVC2 sprite sheet extractor | OPTIONAL — sprite extraction |
| 7 | `https://github.com/flyinghead/flycast.git` | (default) | Upstream Flycast (cloned INTO flycast-wasm/upstream/source/) | YES — base for WASM build |

---

## FILES NOT IN GIT (must copy manually)

| File | Size | Where it goes | Source on Windows |
|------|------|---------------|-------------------|
| `dc_boot.bin` | 2MB | `flycast-wasm/demo/bios/` AND `~/.local/share/flycast/` | Existing Flycast install |
| `dc_flash.bin` | 128KB | `flycast-wasm/demo/bios/` AND `~/.local/share/flycast/` | Existing Flycast install |
| MVC2 US ROM (GDI) | 1.2GB | `~/roms/mvc2_us/` | `C:\roms\mvc2_us\` |
| MVC2 JP ROM (GDI) | ~1.2GB | `~/roms/mvc2_jp/` (optional) | `C:\roms\mvc2_jp\` |

**MVC2 US ROM files:**
- `Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].gdi`
- `track01.bin`
- `track02.raw`
- `track03.bin`

**Files that ARE in git (no manual copy needed):**
- Save state (all chars unlocked): `maplecast-flycast/savestates/Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].state`
- VMU save: `maplecast-flycast/savestates/T1212N_vmu_save_A1.bin`
- 56 sprite sheets: `maplecast-flycast/web/sprites/*.png` + `manifest.json`

---

## COMPLETE SETUP STEPS FOR UBUNTU

### Step 0: Install all system dependencies

```bash
sudo apt update && sudo apt install -y \
  build-essential gcc g++ cmake ninja-build make git pkg-config \
  python3 python3-pip nodejs npm p7zip-full curl \
  libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev \
  libvulkan-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
  libasound2-dev libpulse-dev libao-dev \
  libudev-dev libevdev-dev libusb-1.0-0-dev \
  libcurl4-openssl-dev libdbus-1-dev
```

For Rust (maplecast-server, optional):
```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source "$HOME/.cargo/env"
```

### Step 1: Clone ALL repos

```bash
mkdir -p ~/projects && cd ~/projects

# 1. MapleCast Flycast (THE MAIN PROJECT)
git clone -b maplecast https://github.com/t3chnicallyinclined/maplecast-flycast.git
cd maplecast-flycast && git submodule update --init --recursive && cd ..

# 2. Flycast WASM (browser emulator)
git clone https://github.com/nasomers/flycast-wasm.git
cd flycast-wasm && git checkout wasm-jit && cd ..

# 3. GP2040-CE fightstick firmware
git clone -b w6100-evb-pico2 https://github.com/t3chnicallyinclined/GP2040-CE-NOBD.git GP2040-CE

# 4. Relay server (optional, Rust)
git clone https://github.com/t3chnicallyinclined/maplecast-server.git
```

**If repos already exist, pull latest:**
```bash
cd ~/projects/maplecast-flycast && git checkout maplecast && git pull origin maplecast
cd ~/projects/flycast-wasm && git checkout wasm-jit && git pull origin wasm-jit
cd ~/projects/GP2040-CE && git checkout w6100-evb-pico2 && git pull origin w6100-evb-pico2
```

### Step 2: Copy ROMs and BIOS (manual transfer from Windows)

```bash
# Create ROM directory
mkdir -p ~/roms/mvc2_us

# Transfer files from Windows machine:
# scp, USB drive, network share, etc.
# MVC2 ROM files (all 4 files from C:\roms\mvc2_us\):
#   - Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].gdi
#   - track01.bin
#   - track02.raw
#   - track03.bin
# → copy all to ~/roms/mvc2_us/

# DC BIOS files:
#   - dc_boot.bin
#   - dc_flash.bin
# → copy to ~/.local/share/flycast/  (native Flycast looks here)

# Verify
ls ~/roms/mvc2_us/
ls ~/.local/share/flycast/dc_boot.bin
```

### Step 3: Build native maplecast-flycast (the MapleCast server)

```bash
cd ~/projects/maplecast-flycast
mkdir -p build && cd build

# Full desktop build (OpenGL, audio, everything)
cmake .. -DCMAKE_BUILD_TYPE=Release -G Ninja
ninja -j$(nproc)

# Test it runs
./flycast
# Should open Flycast GUI, load MVC2 from ~/roms/
```

**Headless/norend build (no GPU, game logic only):**
```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -G Ninja \
  -DUSE_OPENGL=OFF -DUSE_VULKAN=OFF \
  -DUSE_DX9=OFF -DUSE_DX11=OFF \
  -DUSE_ALSA=OFF -DUSE_PULSEAUDIO=OFF -DUSE_LIBAO=OFF
ninja -j$(nproc)
```

**Run MapleCast server (same as Windows start_maplecast.bat):**
```bash
# Terminal 1: Flycast with MapleCast enabled
export MAPLECAST=1
export MAPLECAST_STREAM=1
# For headless mode (no GPU, game state only):
# export MAPLECAST_HEADLESS=1
cd ~/projects/maplecast-flycast/build
./flycast ~/roms/mvc2_us/Marvel\ vs.\ Capcom\ 2\ v1.001\ \(2000\)\(Capcom\)\(US\)\[\!\].gdi

# Terminal 2: Web server
cd ~/projects/maplecast-flycast/web
python3 -m http.server 3000

# Terminal 3: Telemetry (optional)
cd ~/projects/maplecast-flycast/web
python3 telemetry.py

# Open browser: http://localhost:3000
# Flycast WebSocket: ws://localhost:7200
# Gamepad UDP input: port 7100
# Telemetry UDP: port 7300
```

### Step 4: Install Emscripten SDK (for WASM build)

```bash
cd ~ && git clone https://github.com/emscripten-core/emsdk.git
cd emsdk && ./emsdk install latest && ./emsdk activate latest
source ~/emsdk/emsdk_env.sh
echo 'source ~/emsdk/emsdk_env.sh' >> ~/.bashrc
emcc --version  # verify
```

### Step 5: Clone upstream flycast + apply WASM JIT patches

```bash
cd ~/projects/flycast-wasm
mkdir -p upstream/source && cd upstream/source
git clone https://github.com/flyinghead/flycast.git .
git apply ../patches/wasm-jit-phase1-modified.patch
mkdir -p core/rec-wasm
cp ../patches/rec_wasm.cpp core/rec-wasm/
cp ../patches/wasm_module_builder.h core/rec-wasm/
cp ../patches/wasm_emit.h core/rec-wasm/
```

### Step 6: Build Flycast WASM

```bash
cd ~/projects/flycast-wasm/upstream/source
mkdir -p build-wasm && cd build-wasm
emcmake cmake .. -DLIBRETRO=ON -DCMAKE_BUILD_TYPE=Release
emmake make -j$(nproc)
# Produces: libflycast_libretro_emscripten.a
```

### Step 7: Set up demo server for WASM

```bash
cd ~/projects/flycast-wasm/demo
mkdir -p bios roms data/cores

# Copy BIOS to demo server
cp ~/.local/share/flycast/dc_boot.bin bios/
cp ~/.local/share/flycast/dc_flash.bin bios/

# Symlink or copy ROMs
ln -s ~/roms/mvc2_us roms/mvc2_us
# OR copy: cp -r ~/roms/mvc2_us roms/

# Get EmulatorJS runtime
# Download from: https://github.com/EmulatorJS/EmulatorJS/releases
# Extract and copy data/ directory to demo/data/
```

### Step 8: Update build-and-deploy.sh paths

The build scripts have hardcoded paths. Update for your machine:

**`upstream/build-and-deploy.sh`:**
```bash
PROJECT_DIR="$HOME/projects/flycast-wasm"
BUILD_DIR="$PROJECT_DIR/upstream/source/build-wasm"
WSL_DIR="$HOME/projects/flycast-wasm"
DEMO_CORES="$PROJECT_DIR/demo/data/cores"
SOURCE_DIR="$PROJECT_DIR/upstream/source"
PATCHES_DIR="$PROJECT_DIR/upstream/patches"
source ~/emsdk/emsdk_env.sh 2>/dev/null
```

**`upstream/link.sh`:**
```bash
cd ~/projects/flycast-wasm
source ~/emsdk/emsdk_env.sh 2>/dev/null
UPSTREAM_ARCHIVE="$HOME/projects/flycast-wasm/upstream/source/build-wasm/libflycast_libretro_emscripten_stripped.a"
RESOURCES_ARCHIVE="$HOME/projects/flycast-wasm/upstream/source/build-wasm/libflycast-resources.a"
# Replace ALL /mnt/c/DEV Projects/flycast-wasm/ → $HOME/projects/flycast-wasm/
```

The link step also needs **EmulatorJS RetroArch objects** (`EJS-RetroArch/obj-emscripten/*.o`). Clone and build EmulatorJS's RetroArch fork per their docs.

### Step 9: Link, package, deploy

```bash
cd ~/projects/flycast-wasm
bash upstream/build-and-deploy.sh
```

### Step 10: Test Flycast WASM in browser

```bash
cd ~/projects/flycast-wasm/demo
node server.js 3000
# Open browser: http://localhost:3000
# Select MVC2 → MEASURE FPS
```

---

## MapleCast Flycast Key Files

| File | What |
|------|------|
| `core/network/maplecast.cpp` | UDP gamepad input receiver, P1/P2 auto-assignment, W3→MapleInputState |
| `core/network/maplecast_stream.cpp` | THE main module: CUDA capture, NVENC encode, WebSocket server, game state |
| `core/network/maplecast_gamestate.h/cpp` | MVC2 RAM reader — character state, health, timer, meters, combos |
| `core/network/maplecast_ta_capture.h/cpp` | TA display list UV extractor for sprite frames |
| `core/network/maplecast_telemetry.h/cpp` | Fire-and-forget UDP telemetry to localhost:7300 |
| `core/hw/pvr/Renderer_if.cpp` | Render pipeline hooks — calls maplecast after Present() |
| `core/hw/maple/maple_if.cpp` | Input injection point — maplecast::getInput() replaces ggpo |
| `core/emulator.cpp` | MapleCast init — reads MAPLECAST env vars |
| `web/index.html` | Browser client — WebSocket, H.264/WebCodecs, game state renderer, sprites |
| `web/sprites/` | 56 character PNG sprite sheets + manifest.json |
| `web/telemetry.py` | Telemetry aggregation server |
| `start_maplecast.bat` | One-click launcher (Windows) |
| `tools/inject_dci.py` | DCI save injector for VMU |
| `savestates/` | MVC2 save state (all chars) + VMU save |

## MapleCast Environment Variables

| Variable | What | Default |
|----------|------|---------|
| `MAPLECAST=1` | Enable gamepad input receiver (UDP:7100) | off |
| `MAPLECAST_STREAM=1` | Enable video/game state streaming (WS:7200) | off |
| `MAPLECAST_HEADLESS=1` | Norend mode — game state only, no GPU | off |
| `MAPLECAST_JPEG=1` | Use JPEG instead of H.264 | off |

## MapleCast Ports

| Port | Protocol | What |
|------|----------|------|
| 7100 | UDP | Gamepad input (W3 format: 4 bytes {LT, RT, buttons_hi, buttons_lo}) |
| 7200 | WebSocket | Video/game state streaming + control messages |
| 7300 | UDP | Telemetry diagnostics |
| 3000 | HTTP | Web app (python -m http.server) |

---

## flycast-wasm Key Files (wasm-jit branch)

| File | What |
|------|------|
| `upstream/patches/rec_wasm.cpp` | WASM JIT — SH4 blocks → WASM modules at runtime |
| `upstream/patches/wasm_module_builder.h` | WASM binary format builder |
| `upstream/patches/wasm_emit.h` | SHIL IR → WASM instruction emitter (51/70 ops native) |
| `upstream/build-and-deploy.sh` | Build + link + package + deploy |
| `upstream/link.sh` | Emscripten link flags |
| `demo/server.js` | Node.js server with COEP/COOP headers, game picker |

## Key JS Exports for MapleCast Integration

| Export | What | MapleCast Use |
|--------|------|---------------|
| `_simulate_input` | Inject gamepad input into emulator | Send controller state from WebSocket |
| `_wasm_mem_read8/16/32` | Read emulated DC memory | Read game state (health, timer, etc.) |
| `_wasm_mem_write8/16/32` | Write emulated DC memory | Potential state sync |

---

## MapleCast WASM Integration Plan (after builds work)

1. **Measure MVC2 FPS** in Flycast WASM — if <30, apply optimizations (-O3, -flto, ASSERTIONS=0)
2. **Add WebSocket client** to EmulatorJS emulator page → connects to MapleCast server (ws://server:7200)
3. **Server sends synced inputs** — 253 bytes/frame (W3 format, already built)
4. **Browser calls `simulate_input()`** per frame with synced controller state
5. **Both instances run identical game** = pixel-perfect, zero video encode

## Performance Expectations

| Build | FPS | Notes |
|-------|-----|-------|
| Interpreter only | 0.4-5 | Way too slow |
| **WASM JIT (current)** | **20-40+** | 51/70 SHIL ops native |

Optimization headroom: `-O3/-flto` (+15-30%), SIMD (+10-20%), threaded rendering (~2x).
Theoretical max: ~125 FPS. MVC2 is a 2D fighter = lighter end.

## MVC2 RAM Addresses (for _wasm_mem_read)

Character struct base: `0x8C268340` (P1), stride `0x5A4`.
Full field list: `maplecast-flycast/core/network/maplecast_gamestate.h`

---

## Fallback Paths (if WASM FPS too low)

1. **Video streaming** — already works at 60fps / 3ms (CUDA + NVENC, Windows only)
2. **Game state renderer** — already works at 253 bytes/frame / 4kbps (stick figures + health bars)
3. **TA display list streaming** — stream PowerVR2 GPU commands, browser renders via WebGL (not built yet)
