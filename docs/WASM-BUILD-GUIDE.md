# MapleCast WASM Build Guide

**Last updated:** 2026-04-06
**Status:** Two WASM paths both shipping — standalone renderer (primary, king.html) and EmulatorJS core (legacy, emulator.html).

---

## Overview

MapleCast ships **two** WebAssembly clients that both render MVC2 in the browser:

1. **Standalone renderer** (primary, used by `king.html`)
   Just the flycast GLES renderer + TA parser, ~28 flycast source files plus
   ~40 stubs. ~831KB `.wasm`. No ROM, no BIOS, no CPU emulation. Built from
   `packages/renderer/` inside this repo.
2. **EmulatorJS core** (legacy, used by `emulator.html`)
   Full flycast WASM core compiled as a libretro archive, then linked into
   EmulatorJS's RetroArch shell. Runs MVC2 CPU-first and then enters mirror
   mode. ~3.4MB `.wasm` (7z packaged). Built from the sibling repo
   `~/projects/flycast-wasm/`.

Both clients receive the same TA mirror wire format (delta TA commands +
ZCST-compressed frames) from the native server and render via WebGL2.

### Architecture

```
Server (native Flycast):         Browser (two client paths):
┌──────────────────────┐         ┌──────────────────────────────────────┐
│ Game logic + render   │ ──WS──► │ Path A: packages/renderer/            │
│ TA commands captured  │ ~4MB/s  │   renderer.wasm ~831KB                │
│ Streams to clients    │         │   standalone, used by king.html       │
└──────────────────────┘         │                                       │
                                 │ Path B: flycast-wasm (sibling repo)   │
                                 │   libretro.wasm ~3.4MB (7z)           │
                                 │   EmulatorJS shell, used by           │
                                 │   emulator.html (iframe under         │
                                 │   index.html)                         │
                                 └──────────────────────────────────────┘
```

Both paths are resolution-independent — the server ships TA commands, not
pixels, so the browser renders natively at whatever resolution the canvas is.

---

## Path A: Standalone Renderer (Primary)

Built from `packages/renderer/` in this repo. This is what `web/king.html`
loads. See [WORKSTREAM-KING-OF-MARVEL.md](WORKSTREAM-KING-OF-MARVEL.md) for the
exported C API and the quality-option table.

### Build

```bash
cd ~/projects/maplecast-flycast/packages/renderer
./build.sh   # requires emsdk; runs emcmake + emmake
# Output: dist/renderer.mjs, dist/renderer.wasm
cp dist/renderer.{mjs,wasm} ../../web/
```

Deploy to VPS:

```bash
scp web/renderer.{mjs,wasm} root@<your-vps>:/var/www/maplecast/
# Then bump the ?v=... cache buster in web/js/renderer-bridge.mjs and upload that too.
```

### Critical patches (already applied in `packages/renderer/src/`)

- **`FillBGP()` call before `renderer->Process()`** — server gets it free via
  `rend_start_render()`; standalone client must call it manually or the
  background plane renders wrong.
- **GLSM patched for WebGL2** (`glsm_patched.c`) — filters `GL_FOG`,
  `GL_ALPHA_TEST`, and other caps that are valid on desktop GL but invalid in
  WebGL2.
- **JS-level GL enable hack** — see `web/js/renderer-bridge.mjs`. Emscripten's
  C-level `glEnable` inside the GLSM does not propagate to the WebGL2 context;
  JS must re-issue `gl.enable(gl.BLEND|DEPTH_TEST|STENCIL_TEST|SCISSOR_TEST)`
  before every `_renderer_frame()` or transparency breaks. This is the single
  most important thing to not lose.

### Build inputs (~28 flycast source files + ~40 stubs)

See `packages/renderer/CMakeLists.txt` for the current file list. Rough shape:
`core/hw/pvr/` (TA parser, regs), `core/rend/gles/` (GLES renderer), `core/rend/`
(TexCache, texconv, sorter, osd), plus xxHash + xBRZ from `core/deps/`.
`packages/renderer/src/stubs.cpp` provides ~40 no-op implementations of the
flycast infrastructure the renderer thinks it needs (mprotect, holly_intc,
rec_* JIT, scheduler, etc.) to avoid pulling in the rest of the emulator.

---

## Path B: EmulatorJS Core (Legacy)

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

## EmulatorJS Runtime Gotchas (hard-learned)

These are the bugs we already paid for while wiring `emulator.html`. Each one
looks like "it just doesn't work" in the browser console:

- **Canvas presentation.** `_mirror_render_frame` renders to flycast's FBO,
  but RetroArch's `video_cb` won't present it on its own. Fix: `mirror_present_frame`
  in `shell/libretro/libretro.cpp` explicitly calls `video_cb` after the mirror
  renders. Without this, the browser runs happily but draws nothing.
- **Dual render flicker.** When mirror mode is active alongside the running
  game, both render to the same FBO and flicker. Fix: `emu.pause()` stops the
  game's render loop while keeping the RetroArch main loop alive so `video_cb`
  still fires.
- **Core options for MVC2.** Set in `defaultOptions`:
  `reicast_enable_rttb = enabled` (critical for MVC2 sprites),
  `reicast_hle_bios = enabled` (skips the BIOS boot screen).
- **BIOS setup via startGame patch.** EmulatorJS does NOT natively set up
  Dreamcast BIOS. The `startGame` function is patched to (1) fetch
  `dc_flash.bin` from the server, (2) create `/dc/` in the WASM filesystem,
  (3) write the core options file, (4) set `system_directory` so flycast
  finds the BIOS. Without it, flycast boots to a BIOS error screen.
- **Auto-start flow.** MVC2 CHD loads → game boots → `EJS_onGameStart` fires
  → 1s delay → `_startMirror()` auto-called. No manual console commands.

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
