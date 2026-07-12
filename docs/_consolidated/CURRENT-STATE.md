# MapleCast — CURRENT STATE (Consolidated Truth)

> **What this is:** the single navigable "where are we right now" snapshot, distilled from
> the 56-doc `docs/` corpus. Read this first, then follow the pointers.
> Companions in this folder: **[INDEX.md](INDEX.md)** (doc catalog + status of every file),
> **[LESSONS-AND-GOTCHAS.md](LESSONS-AND-GOTCHAS.md)** (the anti-pattern / hard-won-lesson master list).
>
> **The render authority ledger is [`docs/RENDER-STATE.md`](../RENDER-STATE.md)** — for anything
> render-related, that file + its `render-state/01..07` appendices are the live authority; this
> section only summarizes it.
>
> _Last consolidated: 2026-07-08._

---

## 1. Architecture in one page

Full detail: **[`docs/ARCHITECTURE.md`](../ARCHITECTURE.md)** (canonical 5-pillar mental model).

MapleCast turns **Flycast (Dreamcast emulator)** into a game-streaming server for **MVC2**.
One headless flycast instance runs the authoritative game; the render output is a **TA command
stream** (GPU draw list, not pixels), so every viewer re-renders at native resolution.

**Production topology (single VPS):**
```
headless flycast (127.0.0.1:7210, loopback)
      → Rust relay (0.0.0.0:7201)
      → nginx TLS :443  (/ws /db /hub)  → browsers
SurrealDB 127.0.0.1:8000 (skins, telemetry)   maplecast-hub 127.0.0.1:7220 (/hub/api)
input UDP :7100 (public)   audio :7203   control WS :7211 (loopback ONLY)
```
- flycast is **never** publicly exposed. Footprint ≈ 322 MB RAM, ~12% of 2 vCPU, ~59.7 fps.
- **Current prod box = `149.28.44.118` (nobd.net).** `66.55.128.93` is **decommissioned** —
  CLAUDE.md still names it; trust `149.28.44.118`.

**Five pillars:** (1) emulator/authoritative sim, (2) input server (built into flycast, UDP :7100),
(3) TA-mirror stream, (4) Rust relay fan-out, (5) **Distributed Input Server Network** — anyone runs a
node; the **hub does DISCOVERY + MATCHMAKING ONLY, never the gameplay hot path**.

**Build variants (one source tree, mutually-exclusive CMake modes):**
| Variant | Build flag | What it is |
|---|---|---|
| headless server | `-DMAPLECAST_HEADLESS=ON` | VPS authoritative, ~26 MB Linux / ~9 MB Win, no GPU/SDL/X11 |
| native mirror client | `MAPLECAST_MIRROR_CLIENT=1` (env) or `-DMAPLECAST_CLIENT_ONLY=ON` | TA viewer + UDP input, no local SH4 |
| WASM renderer | `packages/renderer/build.sh` | king.html, 831 KB, TA-parser only |
| WebGPU renderer | pure JS, no build | `webgpu-test.html` / `replay.html`, dev default |

**Wire format (TA mirror):** `ZCST(4) + uncompressedSize(u32 LE) + zstd_blob`. Magic loads as
`0x5453435A` on LE. Delta frame = frameSize/frameNum/pvr_snapshot(64)/TA-delta/dirty-pages;
keyframe every 60 frames; SYNC = full 8 MB VRAM + 32 KB PVR. **Four parsers must move together:**
`maplecast_mirror.cpp`, `packages/renderer/src/wasm_bridge.cpp`, `core/network/maplecast_wasm_bridge.cpp`
(or `web/webgpu/frame-decoder.mjs`), `relay/src/protocol.rs`.

> **Current shipping render config (2026-07-11):** the default client is `webgpu-test.html`
> (render_frame over the **ZCS2 streaming-zstd** wire) — stage & effects ride the wire
> pixel-perfect, character bodies are server-stripped and drawn locally. Full split + prod flags
> in [`../RENDER-ARCHITECTURE-CHECKPOINT-2026-07-11.md`](../RENDER-ARCHITECTURE-CHECKPOINT-2026-07-11.md).

---

## 2. Proven findings / durable reference facts

Deduped, current/reference-grade only. Source doc in **bold**.

### Determinism (the foundation)
- **SH4 emulation is byte-identical across machines AND OSes** (validated 2026-05-07 five ways:
  wire 48/48, same-machine 30/30, cross-Linux 19/19, cross-OS Linux↔Windows 30/30). Prereqs: same
  savestate (SHA-256), same ROM bytes, same DC BIOS/REIOS-HLE, **same flycast commit** (cross-version
  is NOT validated). — **ARCHITECTURE.md**, **ROLLBACK-PREDICTION.md**
- The TA wire is **byte-perfect deterministic** because of **6 race fixes in commit `466d72d54`**
  (never reintroduce; see LESSONS doc). This lets rollback compare **inputs only** (~16 B/frame),
  never TA buffers. — **ARCHITECTURE.md**
- Known-good MVC2 US v1.001 ROM SHA-256 = `396548fe53f9b3641896398be563795ff190f9b0d7cc61c331901bc68f4e5392`. — **DEPLOYMENT.md**

### MVC2 memory map (source: **MVC2-MEMORY-MAP.md**, corroborated by **MARVELOUS2-RE-HANDOFF.md**)
- **Only ~42 pages (168 KB) of 26 MB change per frame** (~0.6%). Page 616 = char structs, 649 = global state, 505 = camera, 841 = frame counter.
- **Char struct:** base P1C1 `0x8C268340`, stride `0x5A4`, 6 slots. Load-bearing offsets:
  `+0x144` sprite_id (THE atlas key), `+0x154` current_cell_data ptr, `+0x15C/160` GFX1/GFX2,
  `+0x164` Dat_Pal, `+0x1D2` **real xflip** (`+0x110` is a stale COPY — reading it caused mirror asymmetry),
  `+0x420` health, `+0x25` pl_palid_match (**live tint**, use over `+0x52D`), `+0x12E` live palette-effect
  selector (the field `+0x40`/PALF was mistaken for — PALF is 0 on normal hits).
  **Never ship the engine-owned pointer cluster `+0x154..0x184`** (injection crash source).
- **Object/effect/projectile pool:** ONE pool, base `0x8C26AA54`, stride `0x1D0`, 256 nodes.
  `+0x03` category, `+0x80` owner, `+0x12C` sprite_id, `+0xC8/CC` screen x/y, `+0x130` xflip.
  **No numeric z** — draw order = active linked-list position. Capes/projectiles/supers are separate
  pool objects with their own sprite_id.
- **Globals (page 649):** in_match `0x8C289624`, meter fill/level `0x8C289646/64A`, frame_counter
  `0x8C3496B0`, RngVal `0x8C16BC2C` (determinism-relevant).
- Scale is **anisotropic CpsX=1.6667 / CpsY=2.1428**, not the old 1.75× lock.

### Sprite / render facts (source: **RENDER-STATE.md** + **render-state/01,03**, **GSTA-FINDINGS-FOR-BROWSER.md**)
- Sprite pixel codec = **flag-bit LZSS over u16 LE words** (decoder `loc_8c03552a`), NOT bespoke RLE.
- **Texel carve FINAL answer = 2-ROW BANDS** (`by=row&~1; k=by*Tw+col*bh+(row-by)`). Most re-hit bug family;
  `gstaDecodeBodies` (mirror.cpp) and `ensureBodyTextures` (body_decoder.mjs) implement the same carve and
  **must be edited in lockstep**.
- Body geometry (all byte/pixel-exact vs engine): facing=1 ⇒ faces RIGHT; texU mirror = raw
  `facing XOR 0x4000`; sprite_id **bit15 = scale-walker dispatch** (not flip); full-span (sw·8×sh·8) not
  logical-crop; atlas parts stored bottom-up ⇒ V-flip in sampling only.
- Palette: write sprite BASE COLOR at `+16` (zero face-color × modulate ⇒ black); **preserve resident
  rectab TCW PalSelect**, never override with a static even-bank formula (the "Cable-blue" bug).
  "Purple Cable" is engine-faithful — identify chars by char_id, never by color.
- Effects route **by GFX base pointer (node+0x15C)**, not sprite_id. The Effect-Poly class was
  **dissolved** (re_kb/49): measured supers have zero Effect-Poly nodes; old "garble" was slot-walk
  **over-tiling**. Real effects = sprite-machine bit15 quads + the 3D machine (both closed/captured).
- Z-order is **by list-type (OP→PT→TR) then submission order**, not depth. Flattening HUD quads to z=1.0
  causes the all-red-bars bug.
- HUD: white FONT.BIN texture modulated by **per-team-slot** colors (not a health gradient); life-bar fill =
  `currentHP/maxHP` where **maxHP is per-character** (the old `/144` was only right for the super meter).

### Wire / bandwidth (source: **render-state/07** MEASURED 2026-07-08)
- In-match wire is **6.875 Mbps** on `feat/render-replica-live` (not the Apr-2026 ~4.1 anchor) —
  78.7% of raw bytes is VRAM dirty pages, inflated by a DMA force-dirty re-ship path (56.9% of shipped
  pages are byte-identical to their last ship). **Cite the 4.1 Mbps anchor or this measured 6.875;
  never the "1.7 MB/s" or "36–88 Mbps" figures.**
- A measured zero-loss compression stack reaches **0.788 Mbps @ 0.23 ms/frame** (streaming zstd shared
  window + hash-gate + runSoA). Quickest win = **hash-gate the force-dirty page ships** (server-only).
- GSTA state wire ≈ 7 KB/frame; lockstep client ≈ **2.4 KB/s**.

### Rollback / replay state-save mechanism (source: **ROLLBACK-PREDICTION.md**, **REPLAY-SIMPLIFICATION.md**, **DC-SERIALIZE-AUDIT.md**)
- **The 253-byte gamestate is NOT a savestate** — it's for divergence detection only. Resume needs the
  full ~27 MB savestate (SH4 regs/RAM/VRAM/BIOS ptrs).
- **rollback = replay = spectator must be ONE code path with a PULL-model input read** (the SH4's own
  input function returns recorded data indexed by frame). A separate PUSH thread injecting into the live
  input atomic caused the `0x5e6bb82b5f80` SIGSEGV. That crash is **FIXED** (pull-model + restore moved to
  the `emulator.cpp` autoload point where the JIT is initialized).
- **Runtime `dc_serialize` round-trip is poison** — it drops ~14 file-scope statics → a ~3900-byte frame-1
  TA divergence. Proven workaround: **don't round-trip; use the on-disk `mvc2.state` via the existing
  `dc_loadstate` autoload path** (or power-on boot). In-memory rollback copies may sidestep the gap;
  file-replay does not. On Windows, `os_InstallFaultHandler()` is mandatory or the first vmem fault silently
  kills the process.
- **Version-gate savestates/.mcrec by build_id** — they are NOT portable across flycast commits (crash B
  was old-layout bytes into new struct shapes).

### Input latch (source: **INPUT-LATCH.md**, reference)
- DC reads the pad **once per frame at vblank (CMD9)**. Two runtime policies: **LatencyFirst** (default,
  byte-perfect vs pre-Phase-B baseline) vs **ConsistencyFirst** (edge-preserving accumulator, +≤1 frame,
  fixes the dash-eaten-input bug). Policy follows the **player** (localStorage), gated server-side by
  `getSlotForConn`. Atomic layout `[buttons:16][lt:8][rt:8][seq:32]`.

### Skins (source: **SKIN-SYSTEM.md**, current)
- ARGB4444 LE, 16 colors/palette. Bank formula `bank = 16*(char_pair+1) + 8*player_side`. Headless PVR
  palette RAM is empty, so `applyPaletteOverrides()` (every frame, before diff scan) always wins; force a
  dirty page by toggling entry 1023. 5,202 community skins in SurrealDB.

---

## 3. Workstream status

| Track | Status | Where it stands |
|---|---|---|
| **Lockstep mirror client** | **LIVE / winning path** | `maplecast_lockstep.{h,cpp}`, `MAPLECAST_LOCKSTEP=1`, commit `bc16af338`. Client runs full local SH4+ROM from the input/checksum wire (~2.4 KB/s), **bit-exact live-proven 9121/9121 checksums**. This is the endgame for ROM-holding native clients and moots render-reconstruction for them. Its arrival **triggered the rollback un-shelve condition**. |
| **Rollback / predict-live client** | **IN PROGRESS (current session — see §4)** | Byte-determinism foundation validated; predictor = the reused headless build (not the mirror client). The old `0x5e6bb82b5f80` SIGSEGV is fixed (pull-model). Gated `MAPLECAST_PREDICT_LIVE`. |
| **GSTA reconstruction / browser render** | **DEFAULT off; render_frame path byte-exact** | render_frame transpile + lockstep decoders = the byte-exact default drawer; sprite machine **CLOSED 5/5** (`189544592`). **Phase 2a native char-pass** (`MAPLECAST_GSTA_NATIVE_CHARPASS`, `483511fef`) byte-gate CLOSED offline but **SHELVED by lockstep** and **renders bodies invisible LIVE** (first live A/B 2026-07-08 — the live composite-RAM byte gate has never run). Browser `replay.html` default still `emitter` despite the standing decision to flip to render_frame — OPEN. |
| **RE / assets** | **REFERENCE-complete for bodies** | marvelous2 disasm = canonical goldmine (clean-room, gitignored). Bodies texels+geometry byte-exact; HUD closure mapped (Phase 2b, `loc_8c03012c`), byte-matched vs HUDQ oracle but not yet run char-pass-style. Stage: only **STG0B** baked; full stage_id→STGxx map incomplete. Effects captured inside the 2a TA by construction (3D re-impl campaign DEAD). |
| **Deploy** | **LIVE, disciplined** | Prod `149.28.44.118`. Always edit-local → commit → **deploy script** (`deploy-headless.sh` / `deploy-web.sh`, both back up). Never raw scp; prod may be AHEAD of git; systemd capability-stripping and Docker `/dev/shm` 64→256 MB are recurring bites (see LESSONS doc). |
| **Pixel-shipping (mirror/STAF/VCACHE)** | **DEAD** | Whole family abandoned (floors at 36–88 Mbps, ~73% geometry). Superseded by reconstruct-from-state. Do not resume TX64/STAF/VCACHE or whole-frame TA dedup. |
| **Match-data platform / matchmaking / Saturn / Option 6 ideas** | **VISION / research / shelved** | MATCH-DATA-PLATFORM + MATCHMAKING are current plans (not built); Option 6 cache-dedup is a proven dead end (bandwidth was never the user-felt bottleneck — latency was); Saturn/Ymir is a viable-but-unbuilt second-console study. |

---

## 4. CURRENT SESSION — rollback predict-live client

**This is the active workstream.** Gated behind **`MAPLECAST_PREDICT_LIVE`**.

**Achieved:**
- **Instant input** (local gamepad applied at frame N with zero added latency) and a **run-ahead lead**
  (the predictor SH4 advances ahead of confirmed server state, keeping the savestate ring for rollback).

**Last blocker:**
- **Client↔server input landing-frame alignment.** The client and server disagree on which emulated frame
  a given input packet lands on (the same START-of-frame-read vs END-of-frame-log race called out in
  ROLLBACK-PREDICTION.md / REPLAY-SIMPLIFICATION.md — there must be ONE authoritative SH4 frame counter both
  sides key off).

**Fix direction (agreed):**
- **Server assigns the landing frame and echoes it back** to the client, so both sides agree on the
  authoritative frame each input is bound to (removes the per-side pace-counter drift; the predictor then
  rolls back/re-simulates against server-confirmed inputs keyed to the same frame numbers).

Foundation this rests on (all in §2): byte-perfect SH4 determinism, pull-model input read, in-memory
rollback state copy (sidesteps the `dc_serialize` completeness gap), same-commit requirement.

---

_See [INDEX.md](INDEX.md) for the full per-doc catalog and [LESSONS-AND-GOTCHAS.md](LESSONS-AND-GOTCHAS.md)
for the complete anti-pattern list referenced throughout this page._
