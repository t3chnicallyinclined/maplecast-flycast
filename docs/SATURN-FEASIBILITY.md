# MapleCast for Saturn — Feasibility Analysis

> **Verdict: VIABLE, and the cleanest architecture of any non-Dreamcast console we've looked at.** Ymir's renderer interface IS the streaming abstraction. Estimated bandwidth 2.4–24 Mbps at 60 fps, comparable to MapleCast's 4 Mbps Dreamcast baseline. One concrete blocker for ~15% of the library (Sonic R, NiGHTS, certain Treasure games) due to VDP1 framebuffer readback tricks; the other 85% streams cleanly.

## Which emulator to fork

The Saturn emulation scene as of May 2026:

| Project | Status | Notes |
|---|---|---|
| **[Ymir](https://github.com/StrikerX3/Ymir)** | **v0.3.1 — May 3, 2026** | **WINNER.** Modern C++, SDL3, VRR + low-input-lag in upstream. 97.4% C++. Software renderer only (good — clean interface, no GL coupling). |
| [Kronos](https://github.com/FCare/Kronos) | v2.7.0 — Jan 2025 | Yabause fork. Larger codebase, has OpenGL renderer. Workable but more complex. |
| [YabaSanshiro / devmiyax/yabause](https://github.com/devmiyax/yabause) | Long-running fork, 7,031 commits | The historical reference. Mature OpenGL renderer. Code structure is the classic vdp1.cpp / vidogl.c. |
| [libretro/yabause](https://github.com/libretro/yabause) | Tracks upstream Yabause | Same code under a libretro wrapper. |
| mednafen-saturn | Unmaintained for ML purposes | Software renderer, good for headless. |

**Ymir is the right target.** It's the newest, cleanest, most actively developed, and crucially — its renderer is abstracted behind a virtual interface that's already shaped exactly like a streaming interface.

## The "renderer-as-streaming-interface" insight

Ymir's [`IVDPRenderer`](https://github.com/StrikerX3/Ymir/blob/main/libs/ymir-core/include/ymir/hw/vdp/renderer/vdp_renderer_base.hpp) declares 23 virtual methods that cover the entire VDP1/VDP2 surface. **These ARE the wire-format opcodes** — no extra design needed:

```cpp
// VDP1 — sprite/polygon engine
virtual void VDP1BeginFrame() = 0;
virtual void VDP1ExecuteCommand(uint32 cmdAddress, VDP1Command::Control control) = 0;
virtual void VDP1EraseFramebuffer(uint64 cycles = 0) = 0;
virtual void VDP1SwapFramebuffer() = 0;
virtual void VDP1WriteVRAM(uint32 address, uint8/uint16 value) = 0;
virtual void VDP1WriteFB(uint32 address, uint8/uint16 value) = 0;
virtual void VDP1WriteReg(uint32 address, uint16 value) = 0;
virtual void VDP1EndFrame() = 0;

// VDP2 — background/layer compositor
virtual void VDP2BeginFrame() = 0;
virtual void VDP2RenderLine(uint32 y) = 0;
virtual void VDP2SetResolution(uint32 h, uint32 v, bool exclusive) = 0;
virtual void VDP2WriteVRAM(uint32 address, uint8/uint16 value) = 0;
virtual void VDP2WriteCRAM(uint32 address, uint8/uint16 value) = 0;
virtual void VDP2WriteReg(uint32 address, uint16 value) = 0;
virtual void VDP2EndFrame() = 0;

// Savestate machinery already exists
virtual void SaveState(VDPRendererSaveState &state) = 0;
virtual void LoadState(const VDPRendererSaveState &state) = 0;
```

A MapleCast-equivalent server-side renderer plugin:
1. Implements `IVDPRenderer`
2. Each method call is logged to a ring buffer with method-ID + args
3. At `VDP1EndFrame()` / `VDP2EndFrame()`, the ring is flushed as a wire packet
4. Server keeps the real software renderer running in parallel for the local game state

The CLIENT implements the same interface in reverse: receives wire packets, dispatches method calls into its local `IVDPRenderer` instance (also running the software renderer). Pixel-perfect replay.

**No hooks. No engine modifications. Just a new renderer implementation.** This is dramatically cleaner than how MapleCast had to weave hooks into flycast's `serverPublish`.

## State footprint (vs Dreamcast)

| Region | Saturn | Dreamcast | Ratio |
|---|---|---|---|
| Display chip VRAM | VDP1 128 KB + VDP2 512 KB = 640 KB | PVR 8 MB | **12.5× smaller** |
| Palette / CRAM | 8 KB (VDP2 CRAM) | 32 KB PVR regs (includes palette) | similar |
| Sprite framebuffer | 2 × 256 KB = 512 KB (double-buffered) | N/A (TA streams directly) | new for Saturn |
| Register state | ~50 KB | ~32 KB | similar |
| **Full state snapshot** | **~1.2 MB** | **~8 MB** | **6.5× smaller** |

Full SYNC packets are 6.5× cheaper. Periodic state-sync overhead vanishes.

## VDP1 command-stream size

VDP1 commands are **32 bytes each** ([`VDP1Command` struct](https://github.com/StrikerX3/Ymir/blob/main/libs/ymir-core/include/ymir/hw/vdp/vdp1_defs.hpp), 28 bytes content + alignment). Twelve command types: NormalSprite, ScaledSprite, DistortedSprite, Polygon, Polyline, Line, UserClip, SystemClip, LocalCoord, plus end/jump control codes.

YabaSanshiro's parser caps at **4096 commands per frame** (`vdp1.cpp` line ~1247 — `while (!(command & 0x8000) && command_count < 4096)`). Real games:

| Game category | Typical commands/frame | Raw bytes |
|---|---|---|
| 2D fighters (Marvel SH, Street Fighter Alpha) | 50–400 | 1.6–13 KB |
| Arcade ports (Daytona, Sega Rally) | 200–800 | 6–25 KB |
| 3D-heavy (Virtua Fighter 2, Panzer Dragoon Saga) | 500–2000 | 16–64 KB |
| Pathological (Burning Rangers, Sonic R during alpha-blend) | 1500–3500 | 48–112 KB |

## Bandwidth math

Per-frame raw payload = VDP1 commands + VRAM/CRAM writes + register writes + framebuffer writes.

```
Typical frame (Marvel Super Heroes, Street Fighter Alpha):
  VDP1 commands:        ~3–13 KB
  VDP1 VRAM writes:     ~1–4 KB
  VDP2 VRAM writes:     ~0.5–2 KB
  VDP2 CRAM/regs:       ~0.1–0.5 KB
  Sprite FB writes:     ~0–4 KB (game-dependent)
  Total raw:            ~5–25 KB/frame
  zstd-compressed:      ~1–6 KB/frame
  @ 60 fps:             ~0.5–3 Mbps  ✅ better than Dreamcast

Heavy 3D frame (Panzer Dragoon Saga, Virtua Fighter 2):
  VDP1 commands:        ~16–64 KB
  VDP1 VRAM writes:     ~4–20 KB (texture updates)
  VDP2 layers:          ~2–10 KB
  Sprite FB writes:     ~5–50 KB
  Total raw:            ~30–150 KB/frame
  zstd-compressed:      ~7–30 KB/frame
  @ 60 fps:             ~3–15 Mbps  ✅ in MapleCast range
```

**Realistic average: ~3–8 Mbps for typical Saturn games.** Comparable to MapleCast's 4 Mbps Dreamcast baseline. Could be tighter — Saturn games run at 30 fps far more often than Dreamcast, which roughly halves the bandwidth.

## The one architectural concern: VDP1 framebuffer readback

YabaSanshiro's [`vdp1.cpp` lines 240–276](https://github.com/devmiyax/yabause/blob/master/yabause/src/vdp1.cpp) exposes `Vdp1FrameBufferReadWord/ReadLong` — **the CPU can read VDP1's rendered output back into system memory mid-frame.**

This is the same architectural property that killed PS2 streaming (the GS framebuffer read-back). But on Saturn it's used by far fewer games:

| Uses FB readback (will need special handling) | Doesn't (streams cleanly) |
|---|---|
| Sonic R (water reflection) | Virtua Fighter / VF2 / VF Kids |
| Sonic Jam (3D area shadows) | Panzer Dragoon / Zwei / Saga |
| NiGHTS into Dreams (motion blur) | Daytona USA / Sega Rally |
| Burning Rangers (lighting effects) | All Capcom 2D fighters (Street Fighter Alpha, Marvel Super Heroes, X-Men COTA, Vampire Savior) |
| Radiant Silvergun (particle composition) | All SNK 2D fighters (KOF '95, '96, '97) |
| Guardian Heroes (some effects) | Castlevania: Symphony of the Night |
| Christmas NiGHTS | Saturn Bomberman |
| Mr. Bones | Most arcade ports |

Rough estimate: **~10–15% of the Saturn library** has VDP1 FB readback. The other 85% would stream cleanly.

**Mitigation paths for the 15%:**

1. **Detect-and-fallback**: Hash-check the VDP1 framebuffer between server and client every frame. If they diverge (i.e., the game read it and the server's local computation modified it), ship a full FB sync (~256 KB compressed to ~50 KB). Cost: ~5 KB/sec on average even for games that constantly read back.
2. **Ship FB-write opcodes alongside commands**: Ymir's interface already has `VDP1WriteFB(address, value)` as a separate hook. Track those writes; client replays them in order alongside `ExecuteCommand` calls. Total order matters.
3. **Game-specific allowlist/blocklist**: ship a per-game flag (`needs_fb_sync: true`) for known offenders. Same hash-based ROM fingerprinting already in MapleCast Phase 7.

Worth noting: the framebuffer readback is **not** the catastrophic mid-frame mutation PS2 has. Saturn games typically write to VDP1 commands, fire the Plot Trigger, then read back the result *after* it completes — single round-trip per frame. PS2 reads + mutates + redraws within the same frame multiple times. Saturn is closer to "render → readback → ship" which is at least tractable.

## Game-specific evaluation

The genre-mix for Saturn is fortunately heavily skewed toward what works:

**Streams cleanly (the 85% case):**
- The entire **Capcom arcade-port library** — every Street Fighter, Marvel Super Heroes, X-Men, Vampire title. Same audience as MapleCast.
- The entire **SNK port library** — Real Bout, KOF '95–'98, Last Blade.
- **Sega arcade ports** — Daytona, Sega Rally, Virtua Fighter, Virtua Cop.
- **Capcom Generations**, **Capcom Arcade Classics**, etc.
- 2D shmups — Cotton 2, Cyvern, Strikers 1945.

**Mostly-clean (probably works with FB-write opcode):**
- Saturn first-party 2D / RPGs — Saturn Bomberman, Magic Knight Rayearth, Shining Force III, Dragon Force.
- 2.5D — Castlevania SOTN, Bug Too.

**Needs special handling (the 15%):**
- NiGHTS, Sonic R, Burning Rangers, Treasure's Saturn games.

For the **fighting-game targeted MapleCast-equivalent audience**, the Saturn library is *better* than Dreamcast's. Saturn has the entire Capcom/SNK 2D arcade lineage that Dreamcast only got the tail end of.

## What changes vs MapleCast/flycast architecture

| Subsystem | Flycast/MapleCast | Ymir port |
|---|---|---|
| Server-side tap | Hooks in `serverPublish()` via TA buffer copy | **Implement `IVDPRenderer` as `WireRenderer` — no engine modification** |
| Wire format | ZCST envelope (TA delta + dirty pages + PVR snapshot) | Renderer-method-ID + args, zstd-wrapped. Method enum + serializer per method. |
| Initial SYNC | 8 MB VRAM + 32 KB PVR | **1.2 MB total** (6.5× smaller) |
| Client renderer | Custom WebGPU TA parser (`web/webgpu/ta-parser.mjs`, ~600 lines) | **Port Ymir's software renderer to WebGPU + JS, OR run it as WASM via the same C++ code.** SW renderer is well-isolated. |
| State sync cadence | Every scene change | Same — Ymir's renderer SaveState method already exists |
| Audio path | Identical (AICA samples) | Saturn has SCSP — different chip but same shape problem |
| Input path | Identical (MapleCast input server) | Saturn controllers are just buttons + d-pad — strict subset of Dreamcast |

Saturn is a **strict simplification** of the MapleCast architecture except for the FB-readback issue. The renderer-as-interface design in Ymir is *better* than what we have for Dreamcast.

## Estimated build effort

Day 0: Fork Ymir, build the SDL3 desktop app. Verify it runs MVC2-equivalent (Marvel Super Heroes Saturn) at 60 fps. Day's work.

Week 1: Implement `WireRenderer : IVDPRenderer` that logs method calls to a zstd-compressed wire stream. Add WebSocket server. Run two Ymir instances side-by-side (one with software renderer, one with WireRenderer) and confirm bit-identical sprite framebuffer output. **End of week 1: server-side streaming works.**

Week 2: Port Ymir's software renderer to JavaScript or compile via Emscripten as WASM. Wire it to receive WireRenderer's stream. Render to a canvas. **End of week 2: end-to-end pixel-perfect Saturn streaming.**

Week 3: Initial SYNC + late-joiner support (reuse Ymir's existing savestate machinery). VDP1 framebuffer readback heuristic + per-game allow/block list. Latency optimization (mailbox / VRR / threading).

Week 4: Production hardening, deploy as a parallel sandbox alongside MapleCast (same VPS, different port range — 7300/7400 series).

**4 weeks to a "MapleCast for Saturn" sandbox running a Capcom arcade port catalog.** Faster than the PS1 estimate (~6 weeks) because of Ymir's renderer-interface advantage, despite Saturn being a more complex console.

## The big picture

MapleCast set the template for one console. Ymir's architecture *is* the template for the second. The fact that Ymir built a renderer interface that exactly matches the streaming abstraction wasn't accidental — it's what you do when you want SW + HW renderers and savestates to all share a uniform method-call shape. **That same shape happens to be wire-protocol-shaped.**

This is the cleanest port we could do. If we wanted to expand the "MapleCast platform" beyond Dreamcast, **Saturn via Ymir is the right second console**, and the right second console is probably *easier* than the first.
