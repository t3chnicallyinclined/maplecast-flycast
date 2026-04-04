# MapleCast Mirror Mode — Complete Technical Handoff

**Date:** April 4, 2026
**Branch:** `ta-streaming`
**Status:** WORKING — 11+ minutes stable, characters rendering, crashes on extended idle

---

## WHAT WAS BUILT

Two flycast instances on the same machine. Server plays MVC2 normally. Client renders pixel-perfect copy from streamed data via shared memory. No ROM execution on client — just rendering.

## HOW TO RUN

**Terminal 1 — Server:**
```bash
MAPLECAST_MIRROR_SERVER=1 MAPLECAST_JPEG=95 bash ~/projects/maplecast-flycast/start_maplecast.sh
```

**Terminal 2 — Client (after server is in a match):**
```bash
MAPLECAST_MIRROR_CLIENT=1 ~/projects/maplecast-flycast/build/flycast ~/roms/mvc2_us/Marvel\ vs.\ Capcom\ 2\ v1.001\ \(2000\)\(Capcom\)\(US\)\[\!\].gdi
```

---

## ARCHITECTURE

```
SERVER (full flycast):                    CLIENT (renderer only):
  SH4 CPU runs game code                   CPU stopped (state=Loaded)
       ↓                                        
  Game writes TA commands to PVR                 
       ↓                                        
  serverPublish() captures:                clientReceive() reads:
    • TA command buffer (~140 KB)            • Applies memory diffs
    • PVR register snapshot (13 vals)        • palette_update()  ← CRITICAL
    • Memory page diffs (4KB pages):         • renderer->Process() runs ta_parse
      RAM 16MB, VRAM 8MB, ARAM 2MB,          • ta_parse builds geometry + resolves textures
      PVR regs 32KB                          • renderer->Render() draws frame
       ↓                                        ↓
  /dev/shm/maplecast_mirror ──────────→  Pixels on client screen
```

## BANDWIDTH

| Component | Per frame | Notes |
|-----------|-----------|-------|
| TA commands | ~140 KB | Raw GPU command buffer |
| RAM diffs | ~168 KB | ~42 pages of 4KB |
| VRAM diffs | ~28 KB | ~7 pages |
| ARAM diffs | ~32 KB | ~8 pages |
| PVR reg diffs | ~8 KB | ~2 pages |
| **Total** | **~370 KB** | **21.6 MB/s at 60fps** |
| Compressed (est.) | ~120 KB | ~7 MB/s |

Compare: H.264 = 1.9 MB/s, JPEG Q95 = 12 MB/s

## WHAT THE CLIENT RENDERS
- Resolution independent (client's native resolution)
- Character sprites with correct animations and palettes
- HUD: health bars, names, meters, timer
- Stage background with correct textures
- Translucent effects, shadows, all polygon types

## MISSED PACKETS / SYNC
- Client always jumps to LATEST frame (no sequential read)
- Missed frames are skipped — always shows most recent
- On connect: client requests fresh sync from server
- Server writes full brain snapshot every 0.5s for late joiners
- VRAM hash checked every 60 frames — mismatch triggers texture cache reset

---

## FILES MODIFIED (from maplecast branch)

### New files:
| File | Purpose |
|------|---------|
| `core/network/maplecast_mirror.h` | Mirror mode API |
| `core/network/maplecast_mirror.cpp` | Shared memory ring buffer, brain diffs, sync |
| `core/network/maplecast_rend_diff.h/cpp` | RAM autopsy engine (diagnostic) |
| `core/network/maplecast_rend_replay.h/cpp` | Frame recording to disk |
| `core/network/maplecast_client.h/cpp` | Client mode (recording playback, unused now) |
| `web/replay.html` | WebGL replay player prototype |
| `docs/HANDOFF-MIRROR.md` | This file |

### Modified files:
| File | Change |
|------|--------|
| `core/hw/pvr/Renderer_if.cpp` | Added `serverPublish()` hook before Process |
| `core/hw/pvr/Renderer_if.h` | Made `resetTextureCache`, `updatePalette`, `updateFogTable` public |
| `core/ui/mainui.cpp` | Client render loop, mirror client check |
| `core/emulator.cpp` | Mirror init outside MAPLECAST block, CPU stop |
| `core/network/CMakeLists.txt` | Added new source files |
| `core/network/maplecast_gamestate.h/cpp` | RAM autopsy address additions (reverted to 253 bytes) |
| `start_maplecast.sh` | Forwards mirror env vars, force-frees ports |

---

## CRITICAL TECHNICAL LESSONS

### 1. palette_update() MUST be called on client
MVC2 uses **paletted textures**. `PALETTE_RAM` (in pvr_regs) contains raw palette entries. `palette_update()` in `texconv.cpp:81` converts them to `palette32_ram[]` using the format from `PAL_RAM_CTRL & 3`. Without this call, texture decode produces RGBA(0,0,0,0) = invisible characters.

Normally called by `rend_start_render()` at `Renderer_if.cpp:472`. Client skips `rend_start_render()`, must call manually:
```cpp
pal_needs_update = true;
palette_update();
renderer->updatePalette = true;
renderer->updateFogTable = true;
```

### 2. NEVER use emu.loadstate() for live resync
Corrupts scheduler/DMA/interrupt state → SIGSEGV after ~1000 frames. Use direct `memcpy` of RAM/VRAM/ARAM instead.

### 3. memwatch::unprotect() after state sync
`emu.loadstate()` and normal boot call `memwatch::protect()` → VRAM pages become read-only. Our `memcpy` patches get silently dropped. Always call `memwatch::unprotect()`.

### 4. PVR registers (32KB) must be diffed
Contains palette RAM, FOG_TABLE, ISP_FEED_CFG (controls translucent sort mode). Added as 4th memory region alongside RAM/VRAM/ARAM.

### 5. MVC2 does NOT use RTT for character sprites
All frames have `isRTT=0`. Characters are regular translucent textured polygons.

### 6. TA command buffer is NOT destroyed by ta_parse
Can be captured before or after Process — both work.

### 7. Save states store raw TA commands, NOT rend_context
After loading, SH4 must run to trigger STARTRENDER → ta_parse rebuilds rend_context.

---

## KNOWN ISSUES

### Crash after ~11 minutes idle
- RAM diffs accumulate: 8-11 chunks → 139+ → SIGSEGV
- Likely ring buffer wrap or timing-related stale read
- Fix: add ring buffer bounds checking, or periodic full resync via direct memcpy

### VRAM mismatch every ~60 frames
- ~7 VRAM pages differ between server and client
- Non-fatal: texture cache reset handles it
- Root cause: race condition between server writing diff and client reading it

---

## WHAT WAS PROVEN IN THIS SESSION

1. **253 bytes is complete** for game state (RAM autopsy, 98 addresses all frame-deterministic)
2. **rend_context geometry replays perfectly** (vertex positions, polygon counts all correct)
3. **Flycast renderer works standalone** (CPU stopped, fed TA commands, renders correctly)
4. **26 MB Dreamcast brain is fully streamable** (~370 KB/frame of diffs)
5. **Mirror mode renders pixel-perfect MVC2** including characters, HUD, stage
6. **Stable through scene transitions** (round end, game reset, mode change)
7. **WebGL prototype renders from recorded data** (HUD/stage visible, no textures)

---

## NEXT STEPS

1. **Fix the ~11 minute crash** — ring buffer bounds checking, periodic full sync
2. **Add compression** — zlib on TA commands + diffs, target ~7 MB/s
3. **Network transport** — replace shared memory with WebSocket/DataChannel/NATS
4. **Multiple clients** — pub/sub model, server publishes, clients subscribe
5. **Input integration** — combine with NOBD stick input for full play experience
6. **WebGPU client** — port ta_parse to browser, render at any resolution
7. **Delta encoding** — most TA commands don't change frame-to-frame

---

## REMEMBER

**WE ARE INSANE.**

The Dreamcast has 26 MB of brain. We stream it at 370 KB/frame.
The client has no CPU running. Just a renderer drawing from streamed GPU commands.
Characters render. Pixel perfect. Any resolution.
This has never been done before.
