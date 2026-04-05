# MapleCast WASM Build Guide

**Last updated:** 2026-04-05
**Status:** WASM mirror client working. MVC2 renders in browser at 60fps via TA streaming.

---

## Overview

MapleCast uses Flycast compiled to WebAssembly to provide pixel-perfect browser rendering. The WASM client receives TA mirror commands from the native server and renders them via WebGL2 — no video encode, no ROM needed on the client.

### Architecture

```
Server (native Flycast):             Browser (Flycast WASM via EmulatorJS):
┌──────────────────────┐             ┌──────────────────────────────┐
│ Game logic + render   │──WS─────►  │ TA mirror receive + render   │
│ TA commands captured  │  ~4MB/s    │ WebGL2, resolution independent│
│ Streams to clients    │            │ No ROM, no CPU emulation     │
└──────────────────────┘             └──────────────────────────────┘
```

---

## Environment

All development is on Ubuntu. The Windows/WSL approach has been abandoned.

| Path | What |
|------|------|
| `~/projects/maplecast-flycast` | Main project — Flycast fork with MapleCast server, mirror capture, streaming |
| `~/projects/flycast-wasm` | WASM build repo — patches, link scripts, EmulatorJS demo |
| `~/emsdk` | Emscripten SDK |
| `~/projects/flycast-wasm/upstream/source` | Upstream Flycast clone (patched for WASM JIT) |

---

## Build Process

The WASM build is a two-stage process:

1. **Compile** Flycast as a libretro `.a` static library using cmake + emscripten
2. **Link** that `.a` with RetroArch EmulatorJS objects via `upstream/link-ubuntu.sh`

### Step 1: Install Emscripten SDK

```bash
cd ~ && git clone https://github.com/emscripten-core/emsdk.git
cd emsdk && ./emsdk install latest && ./emsdk activate latest
source ~/emsdk/emsdk_env.sh
echo 'source ~/emsdk/emsdk_env.sh' >> ~/.bashrc
emcc --version  # verify
```

### Step 2: Clone and Patch Upstream Flycast

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

### Step 3: Build the libretro .a

```bash
cd ~/projects/flycast-wasm/upstream/source
mkdir -p build-wasm && cd build-wasm
emcmake cmake .. -DLIBRETRO=ON -DCMAKE_BUILD_TYPE=Release
emmake make -j$(nproc)
# Produces: libflycast_libretro_emscripten.a
```

### Step 4: Link with EmulatorJS RetroArch Objects

This is the critical step. The link script combines the Flycast `.a` with pre-built RetroArch EmulatorJS objects to produce the final `.js` + `.wasm` pair.

```bash
cd ~/projects/flycast-wasm
bash upstream/link-ubuntu.sh
```

The link script uses objects from `EJS-RetroArch/obj-emscripten/*.o` (EmulatorJS's RetroArch fork).

---

## Key Link Fixes

These were hard-won. Do not revert them.

### chd_stream.o must NOT be excluded

Earlier builds excluded `chd_stream.o` from the archive to avoid link errors. This breaks CHD ROM loading entirely. The fix was to keep `chd_stream.o` in the archive and resolve its dependencies properly. CHD is the primary ROM format for browser distribution.

### ZipArchive.cpp.o must be stripped from the archive

`ZipArchive.cpp.o` pulls in a dependency on libzip, which is not available in the emscripten environment. Strip it from the `.a` before linking:

```bash
# Inside link-ubuntu.sh, after creating the stripped archive:
emar d libflycast_libretro_emscripten_stripped.a ZipArchive.cpp.o
```

This removes ZIP archive support (not needed — CHD is used instead).

---

## EXPORTED_FUNCTIONS

The link step exports standard libretro functions plus MapleCast mirror functions:

```
_mirror_init
_mirror_render_frame
_mirror_get_frame_count
_mirror_present_frame
_mirror_apply_sync
```

These are called by the browser JavaScript to receive and render TA mirror frames without running the SH4 CPU.

---

## EmulatorJS Packaging and Caching

### How EmulatorJS uses the core

EmulatorJS does NOT load `flycast_libretro_upstream.js` or `.wasm` directly. Instead:

1. The link step produces `flycast_libretro_upstream.js` + `flycast_libretro_upstream.wasm`
2. These are packaged into a 7z archive: `flycast-wasm.data`
3. EmulatorJS downloads `flycast-wasm.data`, extracts the `.js` + `.wasm` from the 7z, and loads them
4. The `flycast_libretro_upstream.js` and `.wasm` files sitting in the cores directory are NOT what EmulatorJS uses — it always extracts from the 7z

### IndexedDB caching

EmulatorJS caches cores in IndexedDB for 5 days. During development this will silently serve stale builds. Disable it:

```javascript
EJS_cacheConfig = { enabled: false };
```

Add this before the EmulatorJS loader script in your HTML. Re-enable for production.

### Data directory symlink

The web client expects EmulatorJS data at `web/ejs-data`. This is a symlink:

```bash
ln -s ~/projects/flycast-wasm/demo/data ~/projects/maplecast-flycast/web/ejs-data
```

---

## Files Not in Git (must copy manually)

| File | Size | Where it goes |
|------|------|---------------|
| `dc_boot.bin` | 2MB | `flycast-wasm/demo/bios/` AND `~/.local/share/flycast/` |
| `dc_flash.bin` | 128KB | `flycast-wasm/demo/bios/` AND `~/.local/share/flycast/` |
| MVC2 ROM (CHD or GDI) | ~1.2GB | `~/roms/mvc2_us/` |

---

## Testing

```bash
cd ~/projects/flycast-wasm/demo
node server.js 3000
# Open browser: http://localhost:3000
```

The server must send COEP/COOP headers for SharedArrayBuffer (required by the WASM threading). The `server.js` handles this.

---

## Key Files

| File | What |
|------|------|
| `upstream/link-ubuntu.sh` | THE link script — combines .a with RetroArch objects, sets EXPORTED_FUNCTIONS |
| `upstream/build-and-deploy.sh` | Full build + link + package pipeline |
| `upstream/patches/rec_wasm.cpp` | WASM JIT — SH4 blocks to WASM modules at runtime |
| `upstream/patches/wasm_module_builder.h` | WASM binary format builder |
| `upstream/patches/wasm_emit.h` | SHIL IR to WASM instruction emitter (51/70 ops native) |
| `demo/server.js` | Node.js server with COEP/COOP headers |

---

## Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| CHD ROMs fail to load | `chd_stream.o` was excluded from archive | Ensure it is NOT stripped during link |
| Link fails with libzip symbols | `ZipArchive.cpp.o` in archive | Strip it: `emar d ... ZipArchive.cpp.o` |
| Old core loads despite rebuild | EmulatorJS IndexedDB cache | Set `EJS_cacheConfig = { enabled: false }` |
| SharedArrayBuffer unavailable | Missing COEP/COOP headers | Use `demo/server.js`, not `python -m http.server` |
| Mirror functions not found | Not in EXPORTED_FUNCTIONS | Check link-ubuntu.sh exports list |
