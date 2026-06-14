# Render-Replica Recording Format (Live-Wire Phase 4a)

This is the **live-wire spec** for the off-SH4 render-replica: exactly what RAM the transpiled
`render_frame()` reads each frame, partitioned into **STATIC** (ship once) vs **DYNAMIC** (ship
per frame), and the on-disk format the headless `MAPLECAST_REPLICA_RECORD` hook writes so the
browser page (Phase 4b) and a live WS feed (Phase 4c) can drive the fighters in motion with the
SH4 OFF.

The partition is **derived, not guessed**: `tools/render-replica-poc/trace_readset.c` instruments
every guest-RAM read of one `render_frame()` over the matched dump (`_ryu_capture/mc_ram_dump.bin`,
the same frame Phase 2 proved byte-exact) and classifies each touched region. Result: **UNCLASS = 0**
— every byte `render_frame` reads falls in exactly one STATIC or DYNAMIC (or PoC-scratch) band.
That zero is the completeness proof: nothing is read that isn't shipped.

## The read-set (one frame, matched single-Cable dump)

| guest_addr | what | kind | why |
|---|---|---|---|
| `0x8C2895E0` (0x10) | slot-table count array (16 layers) | **DYNAMIC** | engine rebuilds the draw list every frame (`loc_8c0308c2`) |
| `0x8C287DE0` (16×0x180) | slot-table ptr arrays | **DYNAMIC** | per-frame draw list |
| `0x8C268340` (6×0x5A4) | char structs P1C1..P2C3 | **DYNAMIC** | pos/scale/sprite_id/facing/+0xDC change every frame |
| `0x8C1F9D80` (0x20) | arena-control globals (cursor/base) | **DYNAMIC** | per-frame alloc cursor ping-pong (`loc_8c033950`) |
| `0x8C1F9F9C` (0x200) | tile-descriptor scratch table | **DYNAMIC** | refilled per object per frame (re_kb `body_walker_tiling`) |
| `0x8C2D6AD8` (0xC0) | camera matrices M2/M1 (+0x6B58) | **DYNAMIC** | rebuilt once/frame by `loc_8c1216c0` |
| `0x8C26A510` (0x40) | camera-Z scale block (camZ @+0x20) | **DYNAMIC** | zoom changes per frame |
| `0x8C26823C` (0x04) | GameGlobalPointer | **DYNAMIC** | base ptr for the global-accum struct |
| `0x8C268240` (0x40) | `*(GGP)` global-accum struct (+0x24 RMW) | **DYNAMIC** | running accumulator advanced per object (`loc_8c034bea`) |
| `0x8C26A974` (0x100) | per-char render-param table | **DYNAMIC** | indexed by char `+0x24`/`+0x36`; per-char state |
| `0x8C2DAD30` (0x40) | rectab/idxtab pointer pair | **DYNAMIC** | the two alloc-table pointers (`*0x8C2DAD3C` idx, `*0x8C2DAD4C` rec) |
| `0x8C2AA4C0` (0x10) | global render-mode word (`0x8C2AA4C4`) | **DYNAMIC** | TSP filter source |
| `*(0x8C2DAD3C)` (0x2000) | **idxtab** alloc-index table | **DYNAMIC** | per-frame allocation map (u16/slot) |
| `*(0x8C2DAD4C)` (0x8000) | **rectab** PVR poly-param records | **DYNAMIC** | per-frame finalized 0x20-byte templates |
| `node+0x15C → GFX1` (0x20000) | GFX1 tile-dim/header table | **STATIC** | load-time character art, frame-invariant |
| `node+0x160 → GFX2` (0x20000) | GFX2 cell-record table | **STATIC** | load-time character art, frame-invariant |
| VRAM (8 MB) | part-pixel textures the TA samples | **STATIC** (full image, once) | decoded into VRAM; ships once with the static set |
| VRAM `[0x410000, 0x460000)` (0x50000) | live BODY sprite texture band | **DYNAMIC** (`bodytex`) | the engine re-decodes the animated body parts into this VRAM span every frame; the static VRAM image goes stale → garbling. Ships per frame. |
| PVR regs (0x8000) | palette/render state | **STATIC** | ships once (palette deltas, if any, are a future per-frame extension) |
| `0x0C47FE00..0x0C480200` | PoC r15 stack | SCRATCH | caller re-inits each frame; not engine state |

Notes on the partition (cited to the transpile + disasm, reconciled with re_kb):
- **idxtab/rectab are DYNAMIC.** They are per-frame allocation tables (re_kb `submit_rectab_alloc_index`,
  `submit_arena_base`): the engine writes the per-tile alloc index into idxtab and the finalized poly
  params into rectab every frame. They are sized to whole regions (0x2000/0x8000) so a multi-character
  frame stays inside (correctness over size).
- **GFX1/GFX2 + VRAM textures are STATIC.** Character art is decoded at match-load and frame-invariant
  for the match. They ship ONCE in the static block. (VRAM is the part pixels; PVR regs the palette.)
- **`0x8C1F9F9C` is DYNAMIC, not static.** re_kb `body_walker_tiling` (2026-06-12 correction): it is a
  rolling per-frame scratch table the render path refills per object — so it MUST ship per frame.
  Sized 0x1800 (the live descriptor table spans ~5135 B; the earlier 0x200 truncated it → garbling).
- **`rectab` is sized 0x10000, not 0x8000.** The max record index reached 461/1024 with two bodies on
  busy frames; 0x8000 truncated the table mid-frame. 0x10000 gives headroom for a 3rd/4th body.
- **The `bodytex` DYNAMIC region is a VRAM SLICE, not guest RAM.** The body part-pixel textures live in
  VRAM. The full VRAM image ships ONCE in the STATIC block, but the engine re-decodes the *animated body
  parts* into a sub-band of VRAM every frame, so the static image goes stale and the client samples
  empty/old texels → garbled body. Fix: ship VRAM `[0x410000, 0x460000)` per frame as a DYNAMIC region.
  **Encoding:** it reuses the SAME `{addr,len,tag[8]}` region-table struct as RAM regions, but is
  distinguished purely by **`tag == "bodytex"`**, and its `addr` field holds the **VRAM offset**
  (`0x410000`) rather than a `0x8C…` guest address. The server (`maplecast_replica_live.cpp`) copies it
  from the resident `vram[]` array; the client (`replay.html` `liveApplyFrame`/`applyDynamic`) routes it
  to `pane.vram` at `addr` (NOT `ram[]` at `addr&0xFFFFFF`). Routing is by tag alone — no addr-range
  assumption — so a region tagged `bodytex` is always a VRAM slice applied to VRAM.
- **The PoC stack scratch is neither.** The walker/setup use an r15 stack at `0x0C480000` that the
  off-SH4 caller owns and re-inits each frame; it is excluded from the wire.

## Bandwidth (the render-replica thesis number)

- **Touched bytes (single body, this dump): 681 dynamic bytes/frame.** This is the lower bound — what a
  one-character frame actually reads.
- **Whole-region DYNAMIC ship size (multi-character-safe, worst case): ~410 KB/frame raw**, dominated by
  the `bodytex` VRAM band (0x50000 = 320 KB), the two alloc tables (idxtab 8 KB + rectab 64 KB = 72 KB),
  the char structs (8.5 KB) + slot ptrs (6 KB) + tiledesc (6 KB). At 60 fps that is ~24 MB/s **raw** —
  but the bodytex band is sparse (mostly the active body's part pixels; the rest of the 320 KB span is
  zero/stable), so the whole message zstd-compresses well. Earlier figures (~58 KB/frame) predate the
  bodytex VRAM band and the rectab/tiledesc enlargements that fixed the garbling.
- **With dirty-diff (v2): far smaller.** Most of rectab/idxtab/char-struct bytes are stable frame-to-frame;
  a per-region XOR/RLE diff collapses the per-frame payload toward the ~681 B touched floor (~0.04 MB/s),
  i.e. **GSTA-scale**, which is the thesis: the render-replica feed is state-sized, not pixel-sized.
- v1 ships raw whole regions (correctness first). v2 = dirty-diff the dynamic regions (the wire already
  has a region table to diff against the previous frame).

## On-disk format (`/dev/shm/mc_render_rec.bin`)

Little-endian throughout (matches the LE guest RAM model in `sh4ctx.h`).

```
HEADER (32 bytes):
  u32 magic     = "MCRR" (0x5252434D LE)
  u32 version   = 1
  u32 nStatic                       // # static regions
  u32 nDynamic                      // # dynamic regions
  u32 nFrames                       // # frame records that follow
  u32 vramBytes = 0x800000          // 8 MB VRAM in the static block
  u32 pvrBytes  = 0x8000            // PVR regs in the static block
  u32 reserved  = 0

STATIC REGION TABLE  : nStatic × { u32 guest_addr; u32 len; char tag[8]; }
DYNAMIC REGION TABLE : nDynamic × { u32 guest_addr; u32 len; char tag[8]; }

STATIC PAYLOAD (once):
  u8 vram[vramBytes]
  u8 pvr_regs[pvrBytes]
  for each static region (table order): u8 bytes[len]

PER-FRAME RECORDS × nFrames:
  u32 magic   = "FRMx" (0x784D5246 LE)
  u32 vframe                        // 0x8C3496B0 video-frame counter
  u32 taSize                        // engine TA byte count (ground truth)
  for each dynamic region (table order): u8 bytes[len]
  u8 engine_ta[taSize]              // the engine's PVR param stream for this frame
```

The dynamic regions appear in the **table order** every frame — the player reads the region table
once, then for each frame slurps the regions back to their guest addresses (`addr & 0xFFFFFF`) into a
16 MB RAM image, overlays the static regions once, and calls `render_frame_ta(ram16mb, ...)`. **The one
exception is the `bodytex` region** (tag-identified): it is a VRAM slice, applied to the VRAM image at
its `addr` (a VRAM offset) instead of to area-3 RAM — see the encoding note above. The
embedded `engine_ta[]` is GROUND TRUTH: playback validates `render_frame` output == `engine_ta`
byte-exact per frame (the same diff Phase 2 already passes at frame 0).

## How to capture a recording on prod

The hook lives in `core/network/maplecast_oracle_hook.cpp` (`mc_oracle_charPassCapture`, the
STARTRENDER pre-QueueRender phase — the character pass, same plumbing as `MAPLECAST_DUMP_RAM`).
READ-ONLY (addrspace + array reads only) → determinism-safe; gated entirely OFF by default.

1. **Build** the headless server (Linux): `cmake -DMAPLECAST_HEADLESS=ON` build, deploy via
   `./deploy/scripts/deploy-headless.sh` (backup-safe per CLAUDE.md). The recording code compiles
   in unconditionally but does nothing unless the env var is set.
2. **Run with the gate.** In the systemd unit / launch env for the headless flycast:
   ```
   MAPLECAST_REPLICA_RECORD=300            # record 300 in-match STARTRENDER frames (~5s)
   MAPLECAST_REPLICA_RECORD_PATH=/dev/shm/mc_render_rec.bin   # optional, this is the default
   ```
   It begins recording on the first in-match (`0x8C289624 != 0`) STARTRENDER frame, writes the static
   block + region tables once, then appends one frame record per subsequent in-match STARTRENDER frame
   until `nFrames` is reached, then closes the file and latches off. Stderr logs
   `[REPLICA-REC] open: N static + M dynamic regions, K frames` then `[REPLICA-REC] done: wrote K frames`.
3. **Have the user play a round** while it records (the gate ensures only in-match frames count).
4. **Transfer the file off prod** (it is ROM-derived → gitignored, never committed):
   ```
   scp root@<prod>:/dev/shm/mc_render_rec.bin ./mc_render_rec.bin
   ```
   Sizes: header+tables (tiny) + 8 MB VRAM + 0x8000 PVR + ~256 KB static GFX, then ~58 KB × nFrames
   raw dynamic + the per-frame engine TA. 300 frames ≈ 8 MB static + ~18 MB dynamic+TA ≈ 26 MB.

## Live-wire frame record (Phase 4c) — FRMx + variable tails

The live WS feed (`maplecast_replica_live.cpp` `captureFrame`) emits a per-frame inner payload that
is the MCRR `FRMx` record plus a strict sequence of **variable tails**, then wraps it in the ZCST
zstd envelope. The tails always appear in this fixed order; each is self-describing (length/magic
prefix) so an older client that stops after an earlier tail simply ignores the trailing bytes.

```
FRAME RECORD (inner payload, LE):
  u32 magic   = "FRMx" (0x784D5246)
  u32 vframe                              // 0x8C3496B0 video-frame counter
  u32 taSize  = 0                         // (live wire ships state, not engine TA)
  for each dynamic region (table order): u8 bytes[len]

  --- GFX TAIL (always present) ---
  u32 nGfx
  nGfx × { u32 base; u32 len; u8 bytes[len] }   // fresh body GFX1/GFX2 regions, on-change

  --- PALETTE TAIL (always present; pvrPalLen=0 in steady state) ---
  u32 pvrPalLen
  pvrPalLen bytes of pvr_regs                    // full 32KB block when the palette sig changed

  --- HUDQ TAIL (present ONLY when MAPLECAST_HUD_TA armed AND nHud>0) ---
  u32 magic = "HUDQ" (0x48554451)
  u32 nHud
  nHud × HudQuad                                 // 96 bytes each (struct below)
```

**`HudQuad` (96 bytes, LE)** — the engine's REAL HUD/composite quad, captured this frame from the
surviving TA pass by `maplecast_oracle_hook::collectHudQuads`. This is the interface the
render-replica client (sprite-render expert) consumes to draw the actual MVC2 HUD pixel-perfect,
replacing the hand-coded reconstruction:

```c
struct HudQuad {
  f32 x[4], y[4];   // 4 screen corners, SUBMIT order (HUD bars are angled parallelograms, NOT bbox)
  f32 u[4], v[4];   // 4 UVs, matching corner order
  u32 col[4];       // 4 per-vertex base color words, repacked to ARGB8888 (see Col_Type note)
  u32 pcw, isp, tsp, tcw;   // PVR control words verbatim
};
```

Capture details (cited to the live QDIAG HUD-pass inventory, `re_kb finding:replica_live_hud_real_ta`):
- The surviving STARTRENDER pass that reaches `serverPublish` on headless **is** the MVC2 HUD/composite
  pass (`rend_norend` runs `ta_parse` and builds the full `ctx->rend`, so the HUD polys are parsed every
  frame). `collectHudQuads` runs on that already-parsed `ctx->rend` — **zero extra `ta_parse`** beyond the
  one `mc_oracle_charPassCapture` already does — inside the same per-pass call as the replica capture, so
  the HUDQ tail and the frame body are the same video frame.
- **Discriminator (env-overridable):** keep `(cy<TOPY=120 || cy>=BOTY=420) && w<MAXW=320 && h<MAXH=200 &&
  fmt!=7`; drop the oversized full-screen composite/backdrop (w 13003..14.5M px) and `fmt==7`. On a live
  in-match frame this kept **78 of 82** polys (the 4 dropped are the composite/backdrop). No `cy<=20`
  strip, no textured-only gate.
- **`col[]` semantics:** flycast stores per-vertex color as `col[0..3]={R,G,B,A}` (`ta_vtx.cpp`
  `vert_packed_color_`); the tail repacks to ARGB8888 = `(A<<24)|(R<<16)|(G<<8)|B`. For MVC2's HUD the
  polys are `Col_Type 1/2/3` (intensity) with `Offset=1`, so this base color is the float intensity
  `0x3f800000`=1.0 — the bar tint comes from the FONT/glyph **texture**, not vertex modulate. The client
  decodes via the shipped `pcw` `Col_Type` bits; `col[]` is the verbatim engine value, never guessed.
- The HUD textures (FONT.BIN / portrait glyphs) the bars sample are decoded into VRAM at match-load,
  before STARTRENDER, so they resolve against the already-shipped static 8MB VRAM prefix (+ palette tail)
  via each quad's `tcw`.
- **OPEN (tier-2):** `collectHudQuads` emits ONE `HudQuad` per PVR poly, using the first 4 submit-order
  verts as the corners. The high-value HUD (life bars, meters, timer/combo digits) are single 4-vert
  parallelograms and are exact. A few longer tri-strips (verts 8..32 — multi-glyph name plates) capture
  only their first quad; pixel-perfect HUD text needs the collector to expand a strip into per-quad
  `HudQuad`s. See `re_kb finding:replica_live_hud_real_ta_open_multistrip`.

## Next (Phase 4b/4c)

- **4b:** the browser page reads `mc_render_rec.bin`, replays frame-by-frame through
  `render_frame_ta` (wasm) → `pvr2-renderer` → motion; validates each frame vs the embedded engine TA.
- **4c:** the same dynamic region stream over a live WS (server emits the per-frame dynamic payload
  in real time instead of to a file); switch v1 raw → v2 dirty-diff for the bandwidth win.
