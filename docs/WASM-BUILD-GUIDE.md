# MapleCast WASM — Build Guide & Agent Handoff

**Last updated:** 2026-04-03
**Status:** Ready to build on Ubuntu. All source repos pushed.

## What We're Building

Flycast (Dreamcast emulator) compiled to WebAssembly, running MVC2 in the browser with input sync from a MapleCast server. Pixel-perfect rendering, zero video encoding, zero video bandwidth.

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

Both instances receive identical inputs → identical game state → pixel-perfect rendering with zero video encoding and zero bandwidth for video.

---

## Repos

| Repo | Branch | Latest Commit | What |
|------|--------|---------------|------|
| `t3chnicallyinclined/GP2040-CE-NOBD` | `w6100-evb-pico2` | `3d94f2cc` | Fightstick firmware + CMD9 telemetry |
| `t3chnicallyinclined/maplecast-flycast` | `maplecast` | `8aff10dec` | Flycast fork with MapleCast streaming/input/gamestate |
| `nasomers/flycast-wasm` | `wasm-jit` | (upstream, read-only) | Flycast WASM JIT — the browser emulator |

---

## flycast-wasm Key Files (wasm-jit branch)

| File | What |
|------|------|
| `upstream/patches/rec_wasm.cpp` | WASM JIT backend — compiles SH4 basic blocks to WASM modules at runtime |
| `upstream/patches/wasm_module_builder.h` | WASM binary format builder (no deps) |
| `upstream/patches/wasm_emit.h` | SHIL IR → WASM instruction emitter (51/70 ops native, rest fallback) |
| `upstream/build-and-deploy.sh` | Full build + link + package + deploy script (needs path updates) |
| `upstream/link.sh` | Emscripten link flags — ASYNCIFY, WebGL2, 256MB memory, key exports |
| `demo/server.js` | Node.js server with COEP/COOP headers, game picker, EmulatorJS wrapper |

---

## COMPLETE SETUP STEPS (Ubuntu, run in order)

### Step 0: Prerequisites

```bash
sudo apt update && sudo apt install -y build-essential git python3 cmake ninja-build nodejs npm p7zip-full
```

### Step 1: Clone ALL project repos

```bash
mkdir -p ~/projects && cd ~/projects

# GP2040-CE fightstick firmware (w6100-evb-pico2 branch)
git clone -b w6100-evb-pico2 https://github.com/t3chnicallyinclined/GP2040-CE-NOBD.git GP2040-CE

# MapleCast Flycast fork (maplecast branch)
git clone -b maplecast https://github.com/t3chnicallyinclined/maplecast-flycast.git

# Flycast WASM (wasm-jit branch)
git clone https://github.com/nasomers/flycast-wasm.git
cd flycast-wasm && git checkout wasm-jit && cd ..
```

If repos already exist, pull latest:

```bash
cd ~/projects/GP2040-CE && git pull origin w6100-evb-pico2
cd ~/projects/maplecast-flycast && git pull origin maplecast
cd ~/projects/flycast-wasm && git checkout wasm-jit && git pull origin wasm-jit
```

### Step 2: Install Emscripten SDK

```bash
cd ~ && git clone https://github.com/emscripten-core/emsdk.git
cd emsdk && ./emsdk install latest && ./emsdk activate latest
source ~/emsdk/emsdk_env.sh
echo 'source ~/emsdk/emsdk_env.sh' >> ~/.bashrc
emcc --version  # verify — should show 5.0.4 or later
```

### Step 3: Clone upstream flycast + apply WASM JIT patches

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

### Step 4: Build Flycast WASM

```bash
cd ~/projects/flycast-wasm/upstream/source
mkdir -p build-wasm && cd build-wasm
emcmake cmake .. -DLIBRETRO=ON -DCMAKE_BUILD_TYPE=Release
emmake make -j$(nproc)
```

Produces `libflycast_libretro_emscripten.a` — the Flycast WASM core archive.

### Step 5: Copy ROMs, BIOS, and saves (MANUAL)

These files are too large for git. Copy from Windows machine or other source:

```bash
cd ~/projects/flycast-wasm/demo
mkdir -p bios roms

# DC BIOS (REQUIRED for Flycast to boot)
# Copy dc_boot.bin and dc_flash.bin → demo/bios/
# Source: existing Flycast install or Dreamcast BIOS dump

# MVC2 ROM (1.2GB GDI format)
# Copy entire GDI folder contents → demo/roms/
# Windows source: C:\roms\mvc2_us\
# Files needed: *.gdi + track01.bin + track02.raw + track03.bin

# Save state + VMU (already in git — verify they exist)
ls ~/projects/maplecast-flycast/savestates/
# Should contain:
#   Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].state  (all chars unlocked)
#   T1212N_vmu_save_A1.bin  (MVC2 VMU save)
```

### Step 6: Get EmulatorJS runtime

```bash
cd ~/projects/flycast-wasm/demo
mkdir -p data

# Download EmulatorJS and copy its data/ directory here
# Source: https://github.com/EmulatorJS/EmulatorJS/releases
# You need at minimum: data/loader.js and associated runtime files
```

### Step 7: Update build scripts for this machine

The build scripts have hardcoded paths from the original author's machine. Update them:

**`upstream/build-and-deploy.sh`** — change these variables:
```bash
PROJECT_DIR="$HOME/projects/flycast-wasm"
BUILD_DIR="$PROJECT_DIR/upstream/source/build-wasm"
WSL_DIR="$HOME/projects/flycast-wasm"
DEMO_CORES="$PROJECT_DIR/demo/data/cores"
SOURCE_DIR="$PROJECT_DIR/upstream/source"
PATCHES_DIR="$PROJECT_DIR/upstream/patches"
# Also update emsdk source line:
source ~/emsdk/emsdk_env.sh 2>/dev/null
```

**`upstream/link.sh`** — change these:
```bash
# Line 6: cd to your flycast-wasm directory
cd ~/projects/flycast-wasm
# Line 7: emsdk path
source ~/emsdk/emsdk_env.sh 2>/dev/null
# Lines 9-10: archive paths (update /mnt/c/DEV Projects/ to $HOME/projects/)
UPSTREAM_ARCHIVE="$HOME/projects/flycast-wasm/upstream/source/build-wasm/libflycast_libretro_emscripten_stripped.a"
RESOURCES_ARCHIVE="$HOME/projects/flycast-wasm/upstream/source/build-wasm/libflycast-resources.a"
# All other /mnt/c/DEV Projects/flycast-wasm/ → $HOME/projects/flycast-wasm/
```

The link step also needs EmulatorJS RetroArch objects (`EJS-RetroArch/obj-emscripten/*.o`). These come from building EmulatorJS's RetroArch fork. See the EmulatorJS build docs.

### Step 8: Link + package + deploy

```bash
cd ~/projects/flycast-wasm
bash upstream/build-and-deploy.sh
```

This: compiles → strips conflicting objects → links with RetroArch → packages as .data → deploys to demo server.

### Step 9: Run demo server and test MVC2

```bash
cd ~/projects/flycast-wasm/demo
node server.js 3000
# Open browser: http://localhost:3000
# Select MVC2 from game picker
# MEASURE FPS — this is the critical number we need
```

---

## Key JS Exports for MapleCast Integration

The Emscripten link exports these functions callable from JavaScript:

| Export | What | MapleCast Use |
|--------|------|---------------|
| `_simulate_input` | Inject gamepad input into emulator | Send controller state from WebSocket |
| `_wasm_mem_read8/16/32` | Read emulated DC memory | Read game state (health, timer, etc.) |
| `_wasm_mem_write8/16/32` | Write emulated DC memory | Potential state sync |

### MapleCast Integration Plan (after WASM build works)

1. **Measure MVC2 FPS** — if <30, apply Tier 1 optimizations first (-O3, -flto, ASSERTIONS=0)
2. **Add WebSocket client** to EmulatorJS emulator page → connects to MapleCast server (ws://server:7200)
3. **Receive synced inputs** — server sends 253 bytes/frame (W3 format, already built in maplecast-flycast)
4. **Inject via `simulate_input()`** — browser calls this per frame with the synced controller state
5. **Both instances run identical game** = pixel-perfect, zero video encode

---

## Performance Expectations

| Build | FPS | Notes |
|-------|-----|-------|
| Interpreter only | 0.4-5 | Way too slow |
| Interpreter + ASYNCIFY_REMOVE | 0.5-6.7 | +37% but still too slow |
| **WASM JIT (current)** | **20-40+** | Active development, 51/70 SHIL ops native |

### Optimization headroom (not yet applied):

| Optimization | Effort | Expected Gain |
|---|---|---|
| `-O3`, `-flto`, `ASSERTIONS=0` | One rebuild | +15-30% |
| WASM SIMD (`-msimd128`) | Full rebuild | +10-20% |
| Threaded rendering (pthreads) | Substantial | ~2x (CPU+GPU parallel) |
| Complete remaining 19 JIT ops | Ongoing | Eliminates interpreter fallback |

Theoretical max with all opts: 40 × 1.3 × 1.2 × 2.0 ≈ **125 FPS**. MVC2 is a 2D fighter (lighter end).

---

## What Already Works (maplecast-flycast, native)

- CUDA zero-copy + NVENC H.264 streaming (3ms pipeline, 60fps)
- Game state streaming (253 bytes/frame, 4kbps, 0ms encode)
- WebSocket server built into Flycast (ws://localhost:7200)
- UDP gamepad input receiver (P1/P2 auto-assignment)
- TA display list UV capture (sprite frame extraction from PowerVR2 render pipeline)
- Web app: video/sprites toggle, diagnostics panel, Gamepad API at 250Hz
- `start_maplecast.bat` one-click launcher (Windows)

### MVC2 RAM Addresses (for _wasm_mem_read)

| Address | Field | Size |
|---------|-------|------|
| `0x8C268340` | P1 character struct base (stride 0x5A4) | - |
| Verified fields: active, character_id, pos_x/y, facing, sprite_id, animation_state, health | See `maplecast-flycast/core/network/maplecast_gamestate.h` | - |

---

## Fallback Path

If WASM FPS is too low even after optimization:
- **Video streaming already works** at 60fps / 3ms pipeline (maplecast-flycast native)
- **Game state renderer already works** at 253 bytes/frame / 4kbps (stick figures + health bars)
- **TA display list streaming** (PowerVR2 GPU commands over WebSocket, browser renders via WebGL) — not built yet, but would give pixel-perfect rendering without full SH4 emulation in browser
