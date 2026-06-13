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
| VRAM (8 MB) | part-pixel textures the TA samples | **STATIC** | decoded into VRAM; ships once with the static set |
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
- **The PoC stack scratch is neither.** The walker/setup use an r15 stack at `0x0C480000` that the
  off-SH4 caller owns and re-inits each frame; it is excluded from the wire.

## Bandwidth (the render-replica thesis number)

- **Touched bytes (single body, this dump): 681 dynamic bytes/frame.** This is the lower bound — what a
  one-character frame actually reads.
- **Whole-region DYNAMIC ship size (multi-character-safe, worst case): ~58 KB/frame**, dominated by
  the two alloc tables (idxtab 8 KB + rectab 32 KB = 40 KB) and the char structs (8.5 KB) + slot ptrs
  (6 KB). At 60 fps that is ~3.5 MB/s raw.
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
16 MB RAM image, overlays the static regions once, and calls `render_frame_ta(ram16mb, ...)`. The
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

## Next (Phase 4b/4c)

- **4b:** the browser page reads `mc_render_rec.bin`, replays frame-by-frame through
  `render_frame_ta` (wasm) → `pvr2-renderer` → motion; validates each frame vs the embedded engine TA.
- **4c:** the same dynamic region stream over a live WS (server emits the per-frame dynamic payload
  in real time instead of to a file); switch v1 raw → v2 dirty-diff for the bandwidth win.
