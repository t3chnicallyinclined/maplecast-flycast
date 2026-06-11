# MapleCast Emitter — Session Handoff (2026-06-09)

Pick-up doc for the next agent. The off-SH4 **emitter** (reconstruct-from-state, the pixel-perfect render path) is ~95% done and **blocked only on a build host**, not on design. Read this + `docs/PER-OBJECT-QUAD-SPEC.md` + `docs/MARVELOUS2-GFX-NOTES.md §3a` + memory `project_render_pipeline_state` + the new expert `.claude/agents/mvc2-sprite-render-expert.md` before touching render code.

---

## TL;DR — where we are
- **Emitter pipeline is proven stage-by-stage from the disasm**, and the part-pixel SOURCE is finally identified and coded. The only thing between us and a clean Ryu is **building + deploying one server change.**
- **BLOCKER:** the dev/build box `65.109.77.178` is DOWN (100% ping loss, SSH timeout). The fix is committed + pushed but NOT built/deployed. Operator is restoring the build infra. Local build options on the Windows box: git-bash = no compiler; **WSL = broken ("Class not registered")**; **Docker = present (unverified)** — or spin a throwaway Vultr build VPS.
- **Prod (`149.28.44.118`) is HEALTHY and byte-stock** (binary md5 `0826b807260e73906e67b201828e8118`). Branch `feat/state-replica-client`.

---

## The emitter pipeline (the endgame render)
Replaces the whole-sprite baked path (which *guesses* z/anchor/blend) by porting MVC2's own render so flycast's rasterizer handles z/blend (no guessing). The chain:
```
sprite_id (GSTA, node+0x144)
  → cell      : GFX2[sprite_id & 0x7FFF]   (GFX2 = *(node+0x160))
  → records   : u16 count, then 8-byte records [dx s16][dy s16][pal u16][GFX-selector u16 @+6]
  → geometry  : cumulative PEN (below)
  → pixels    : decoded parts from the emulator (0x0CE60000 at load decode)
  → render    : via sprite-gpu now; endgame = emit a TA → pvr2-renderer (flycast raster)
```

## Status by stage
| Stage | State | Evidence / notes |
|---|---|---|
| **Assembly** | ✅ CONFIRMED | cell key = `sprite_id & 0x7FFF`. The "index is node+0x244" scare was the **cross-checker's own sign-extension error** (`add 0xE4,r0` = `+(-0x1C)` = 0x144, not +0x244). `tools/rip_gfx2_assembly.py` does this offline. The +6 selector indexes **GFX1** (`*(node+0x15c)`). |
| **Geometry** | ✅ CONFIRMED (disasm) | Cumulative pen, cited to `bank03 loc_8c0344d4`: X-acc `±dx` gated by facing(@0x110); Y-acc `−=dy`; seeded at hotspot node+0x134/0x136; final `screen = node+0xE0/E4 + (acc+tile)·scale`, **scale = node+0xEC/F0** (resolved screen-space, NOT world +0x50/54). Inner tile-table expansion (the keystone ~2:1 group→quad) is data-driven (`@(0x1,r13)+1`) and a **no-op for the lean client** (pack the full part rect). **NOT yet live-validated against body quads** (see gaps). |
| **Decode mechanism** | ✅ PROVEN | LZSS → de-twiddle → PAL4 → ARGB4444 palette → magenta-key PPM works — **the DM00 PAL4 32×32 entries decoded to clean recognizable portraits.** The chain is correct; reuse `partDecodeToPPM`. |
| **Gameplay-part SOURCE** | ⚙️ CODED, NOT DEPLOYED | Parts are decoded **contiguously into `0x0CE60000` at the one-shot LOAD decode** (confirmed `bank03 loc_8c032ae0`: dest ptr advances by each part's size). DM00 (`*(0x0CE80008)`, region `0cd8d000`) is the **WRONG** source — it's portrait/UI textures keyed by `char_base+ordinal`, not the +6 selector. Fix `MAPLECAST_GFX1DUMP` (`core/network/maplecast_gamestate.cpp` `gfx1Dump`, commit `3066fe455`) walks the load-window contiguous parts → `/dev/shm/PL00_part_*.ppm` keyed by selector. |

---

## Cross-check ledger — the POLISH gaps (real, scoped, not done)
A full expert cross-check (mvc2-sh4-re-expert) against marvelous2 + anotak + the live Oracle confirmed the pipeline AND caught these (it also produced the 0x244 false alarm above — verify the verifier):
1. **Mirror bits are `0x4000` (X) / `0x8000` (Y) on the record+4 word** — NOT `0x10/0x20` (those are output PVR-mode flags). `rip_gfx2_assembly.py` currently uses `pal & 0x10` → **fix to the +4 word & 0x4000/0x8000**, XOR with owner facing.
2. **Palette row `(pal & 0x3ff)>>4` is a GUESS** — not located in disasm. The 256×256 PAL8 body sheet decodes wrong (PAL8 path/row). Trace the palptr/row math; cross-ref `tcw` + decode `dasm_PLDAT/Output/PL00_DAT/PL00_DAT_PALETTE_DATA.BIN`.
3. **Z / draw order is a HEURISTIC** — the "Cap-shield-behind-him" bug. Source draw order from `category@+0x3` + priority/linked-list (`bank04 loc_8c0450c0`), not the projected z.
4. **Per-part blend/additive flags not read** — render-state OR-bits `0x05/0x07/0x0D/0x0F` (+`0x10/0x20`). Without them additive parts mis-render.
5. **Bodies never live-validated** — every `kind:body` in `_oracle/mc_oracle_hook.jsonl` has empty `screen_quads`; the PER-OBJECT-QUAD count-segmentation was specced but never run on bodies. Geometry is disasm-only.
6. **Effects/supers travel a DIFFERENT path** (Effect Poly `0x0CED0000`, resolved by GFX pointer not sprite_id) — the GFX2 cell walk does NOT cover them; they're streamed (correct — don't reconstruct).

---

## RESUME STEP (do this when a Linux build host is available)
1. Build `feat/state-replica-client` headless (dev box restored, OR WSL/Docker/new Vultr VPS):
   `cmake -DMAPLECAST_HEADLESS=ON -G Ninja && ninja` → `flycast`.
2. Back up `/usr/local/bin/flycast` on prod, cat-pipe the new binary (plain scp truncates over the hop — verify md5), install, `systemctl restart maplecast-headless`, verify active + 7200 + md5.
3. Add `MAPLECAST_GFX1DUMP=1` to `/etc/maplecast/headless.env`. Clear stale dumps as the maplecast user.
4. **Operator: start a FRESH Ryu (PL00) match** — the load decode fires once at match start; capture window = opening frames. (If a selector is missed, `MAPLECAST_GFX1DUMP=40` widens the window.)
5. Pull `/dev/shm/PL00_part_*.ppm` + manifest → `python3 tools/rip_gfx2_assembly.py --gfx1 dasm_PLDAT/Output/PL00_DAT/PL00_DAT_GFX_DATA_00.BIN --gfx2 ...GFX_DATA_01.BIN --pal ...PALETTE_DATA.BIN --char PL00 --out web/test-atlas/chars --realparts <ppm_dir>` → `PL00_parts.png` + `PL00_asm.json`.
6. Load `webgpu-test.html`, `window._emitterPort=true`, A/B on **DIFF v7** vs the green TA truth. **GO = recognizable, color-matched Ryu** (parts at 8×8/16×32/16×64/64×128, not 32×32 portraits, not noise).
7. If GO: scp the atlas (ROM-derived → scp only, never git) + bump `?v`. Then scale: replay each char with the same capture flag.

---

## Key files / artifacts
- `tools/rip_gfx2_assembly.py` — offline assembly extractor (cracked GFX2[sid]+6 walk, cumulative pen) + `--realparts` PPM consumer. **Apply the mirror-bit fix here.**
- `core/network/maplecast_gamestate.cpp` — `partDump` (DM00 + GFX2 walk) + `gfx1Dump` (the new load-window capture, commit `3066fe455`).
- `core/network/maplecast_oracle_hook.cpp` — Oracle hooks; `mc_cellPartCount2` reads the GFX2 index correctly (with the sign-extension comment — the authoritative in-repo statement).
- `web/webgpu/sprite-client.mjs` — `buildEmitterDrawList` (geometry math VERIFIED — do not edit; consumes `c.asm[sid]`). Whole-sprite anchor = `window._objAnchor='own'` (own-origin, the proven default).
- `web/webgpu-test.html` — the differ **DIFF v7** (tint green=truth/red=ours/yellow=match; WebGPU mirror-canvas readback fix).
- `docs/PER-OBJECT-QUAD-SPEC.md`, `docs/MARVELOUS2-GFX-NOTES.md §3a` — **note: §3a's mirror-bit (0x10/0x20) and any 0x244 reference are WRONG per the cross-check; corrections above.**
- `.claude/agents/mvc2-sprite-render-expert.md` — new expert (bake/atlas/sprite-client/emitter).

## Roadmap
1. **Deploy GFX1DUMP → capture Ryu → render → DIFF v7 verdict** (the end-to-end proof). ← next
2. Apply cross-check corrections: mirror bits (#1), palette row (#2), blend flags (#4), z/draw-order from category (#3).
3. Live-validate body geometry (#5 — implement the count-segmentation on bodies).
4. Scale part-capture to the roster (replay each char with `MAPLECAST_GFX1DUMP`).
5. **Endgame:** route the emitter output into a TA → `pvr2-renderer` (flycast's rasterizer) so z/blend are flycast's, not ours — zero-guess raster.
