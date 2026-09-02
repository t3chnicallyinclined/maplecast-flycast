# HANDOFF — Satellite read-set + ground-truth diff pipeline (2026-07-09, evening)

> **STALE PROD BOX - historical record.** Written when prod was `149.28.44.118`
> (Vultr, hostname `flycast-inputserver-nyc`). Since 2026-09-01 `nobd.net` /
> `play.nobd.net` are served by **rise3** (`15.204.141.58`, `ubuntu@`, key
> `~/.ssh/ovh_maplecast`, passwordless sudo), unit `maplecast-flycast.service`.
> Current server architecture lives in ONE place: forgily-creations
> `plans/rise3_handover.md` section 0 (copy `~/HANDOVER.md` on rise3).
> Read the host facts below as history, never as a deploy target.

> Continues `docs/HANDOFF-CHARFLIP-2026-07-09.md`. This session fixed the live wire
> drop-storm, the cape depth, and the big one — the **satellite/effect node pool was
> never in the per-frame read-set** (stale capes/projectiles). Built a full **ASMTRACE
> ground-truth diff pipeline** and used it to ground the remaining residual on
> **re_kb/51 (2-part transposition)**. Everything below is on `feat/render-replica-live`
> and deployed to prod `149.28.44.118` unless marked otherwise.
>
> **Read `docs/RENDER-STATE.md` + `tools/re_kb/README.md` first** — they are the authority.
> This doc is the delta.

## TL;DR — what shipped and what's left

**LIVE on prod, verified:**
1. **Wire drop-storm fix** (`e7bfb6a56`) — the relay's 16-slot broadcast silently dropped
   ZCS2 messages mid-epoch whenever a multi-MB SYNC crossed a slow socket → `soaInverse`
   garbage → desync loop ("perfect until the SYNC hits, then background gone"). Fixed with a
   per-client lossless `SendQueue` (never drop ZCS2/state/audio; drop-oldest legacy over an
   8MB budget; a SYNC supersedes queued legacy + older SYNCs) + **ZCS2 flags bit7 = u16
   per-epoch seq** so a mid-epoch drop is detected deterministically, not via garbage.
2. **Cape per-part depth** (`ee3e60065`, deployed `73db3791a`, wasm `?v=capez1`) — render_frame
   emitted ONE constant per-object z for every body tile; missing the engine's `W += 0.001`
   per-tile increment (re_kb/38). Now `q->z = 1/(ow + 0.001·(k+1))`. First-submitted tile
   frontmost, matching the engine's Greater/GEqual depth test.
3. **objpool satellite read-set fix** (prod `maplecast_replica_live.cpp`, binary md5
   `a1eb501b`) — **the session's main fix.** See §2.

**In source, NOT deployed:**
- `render_frame_cull_85xxx` **removed** from `render_frame.c` (verified no-op on idle; it was
  masking re_kb/51). Deploy it bundled into the next wasm rebuild.

**The remaining residual (grounded):**
- **re_kb/51 — 2-part transposition.** Two cleanly-rendered Storm body parts (same-Ax,
  storage-columns reversed) swap positions = the stray "dark band" the user saw. Documented
  OPEN; was blocked on "a frame-exact capture of the specific pose" — which the new pipeline
  now provides. NOT reproduced yet (idle capture had no clean 2-swap; the pose is specific).

## 1. The wire fix (relay + seq) — details

- `relay/src/fanout.rs`: per-client `SendQueue` (std::Mutex + Notify), a dedicated sender task
  drains it so the select! loop **never awaits the socket**. `protocol.rs` `classify()` →
  `FrameClass::{Critical, LegacyDelta, Sync}`. Broadcast channel 16→1024. Zero-copy `Bytes`
  end to end. Targeted SYNC delivery (clients that speak `request_sync` get only snapshots
  they asked for). Auto-`request_sync` on forced drops. Unit tests: `send_queue_tests`,
  `classify_tests` (10/10 pass).
- ZCS2 bit7 seq: `maplecast_mirror.cpp` emits `_zSeq` (u16, resets 0 at stream-start) after
  the cam/vf/ord payloads. Parsers updated: `webgpu-test.html` (main + worker), `zcs2-test.html`.
  Gate: `tools/render-replica-poc/zcs2_seq_gate.mjs` (extracts the SHIPPED worker decoder,
  16/16 synthetic scenarios pass). Wire table row added to the char-flip handoff.

## 2. THE MAIN FIX — satellite object pool in the read-set

**Root cause (confirmed with ASMTRACE):** the replica-live per-frame read-set shipped the 6
character body slots (`char_str` @ 0x8C268340) but **NOT** the satellite/effect object pool.
Capes, projectiles, drones, extra-limbs are out-of-char-struct BODY/satellite nodes at
`0x8C26AA54 + N·0x1D0` (256 nodes; base/stride/count = `bank04.asm loc_8c044dce`). They rode
only the once-shipped static 16MB snapshot → **frozen stale**. Proof: idle Storm cape node
`0x8C272EA4` read `gfx1 = 0` while ASMTRACE showed it actively emitting 11 parts. Trace had
**190 stale satellite nodes vs 2 fresh body nodes**. This is **re_kb/29** (satellite GFX
residency — "never triggered"), and the user intuited it ("separate object/spark data, could
we not be shipping it?").

**The fix already existed in the local branch but was never deployed to prod.**
`core/network/maplecast_replica_live.cpp` local line ~450: `D(0x8C26AA54, 0x1D000, "objpool")`.
Prod's `prod-zcs2-shadow` was an older read-set missing it (and `gstate`/`battle`/`efxtmpl`).
Ported objpool to prod, rebuilt, deployed. Whole-pool ship (per-node windows rejected as
fragile — the walker reads scattered fields +0x24..+0x184). **Bandwidth cost: ZERO** — the
wire stayed 0.367 Mbps because zstd crushes the mostly-static pool ("zstd-trivial when
unchanged"). The client applies all dynamic regions generically → no client change needed.

**Verified:** every Storm satellite now reads `gfx1 = 0xC420040` (fresh); user confirmed
**moving forward + supers render correctly**. The cape now flows in front/behind Storm
faithfully (render_frame reads the real per-frame `W` at node+0xE8).

## 3. The ground-truth diff pipeline (built this session, `tools/render-replica-poc/`)

The method: **ASMTRACE (the engine's real per-part recipe) vs render_frame output**, per-node,
frame-exact. This is what re_kb/51 was blocked on.

- **ASMTRACE** — `MAPLECAST_ASMTRACE=1` (enabled on prod, read-only, PC `0x8C034864`) →
  `/dev/shm/mc_assembly.log`. Columns: `frame sid slot cid sel dx dy accX accY screenX screenY
  pal row flip flags r11 r13 node`. The `frame` col == the .mcrr `vframe` (alignment key). The
  `flags` col = PCW mirror bits (0x10=X, 0x20=Y); `flip` = the applied X flip.
- **`capture_break.mjs`** — dependency-free (Node 22 built-in WebSocket) one-command capture of
  `wss://nobd.net/replica-live` → scrubbable `.mcrr`. Stamps offset-8 = post-dyn tail length so
  the on-change GFX tail doesn't break offline frame-boundary recovery. `--out X.mcrr --frames N`.
- **`_extract_ram.mjs`** — seed a 16MB RAM image from one `.mcrr` frame → `ram.bin`.
- **`_z_probe.c`** — native (gcc) render_frame build; dumps per-body z-bands (found the cape z
  ordering; proved render_frame reads the real W faithfully).
- **`_diff_vs_asm.mjs`** — single-frame per-node diff. Auto-discovers nodes, pulls each node's
  gfx1 from RAM (node+0x15C), reports position divergence + X/Y mirror flags. Fixed to the
  capture_break `.mcrr` format.
- **`_diff_sweep.mjs`** — ALL-frame sweep, per-node matching via the new `render_frame_obj_*`
  exports, flags mis-positioned parts + over-emission + 2-part transposition. **Node key gotcha:
  rf node is guest `0x8C..`, asm node is area-3 `0x0C..` → mask `& 0x0FFFFFFF` + padStart(8).**
- **`_diag_scan.mjs`** — per-body sid / effect-routing / X-Y mirror flag census.
- **New wasm exports** (`wasm_entry_frame.c`): `render_frame_obj_count/obj_node/obj_ntiles` — let
  the diff partition quads by owning node (satellites of one char share a gfx1, so gfx1 alone
  can't key them). Rebuild with `build_wasm_frame.sh` (emsdk installed at `~/emsdk`).

## 4. Findings ledger (this session)

- **render_frame is FAITHFUL** on geometry: reads real node+0xE8 W → z=1/W; part positions match
  ASMTRACE (the cape flow front/behind is correct, driven by the engine's own per-frame W).
- **`render_extra` (anim cell +0x11) was a DETOUR.** It correlates with cape/wing characters
  (Storm idle = 1/2/3, jump = 0) — surfaced via the `mvc2-skin-studio` repo's animation catalog
  (`web/anim/PL2A.json`; cell = sprite_id@+0x4, duration@+0x2, ender@+0x3, render_extra@+0x11,
  hitbox@+0x12). But render-state doesn't treat it as a render mechanism, and the skin studio
  composites with the SAME engine-z model (re_kb/38) we already have. Not the residual.
- **The residual = re_kb/51 2-part transposition** (per the render-state ledger). Two same-Ax
  Storm parts, storage-columns reversed → swapped. The 0x85xxx cull (removed this session) was
  the fix for re_kb/51's *motion-blocks* half; the *transposition* half stays open.
- Hardcode audit (shipping render path) = clean: no capture-pinned maps, no per-character
  branches in the client; the one band-aid was `render_frame_cull_85xxx` (now removed).

## 5. Docs / context in use

- `docs/RENDER-STATE.md` + `docs/render-state/01-09` — **the render authority.** re_kb/51 is the
  documented body residual; re_kb/29 is the satellite-gfx-residency we just fixed.
- `tools/re_kb/*.surql` — the RE knowledge graph (query before re-deriving). Key: **38** capes
  z=1/W, **51** transposition, **29** satellite gfx residency, **32** scramble (GFX2 self-modify),
  **22** flip flags (r11+4 0x4000/0x8000), **50** super effect over-tile.
- `marvelous2/build/bank*.asm` — the labeled MVC2 SH4 disasm (gitignored; never commit).
- `mvc2-skin-studio` (github t3chnicallyinclined) — animation catalog + the engine-z compositing
  reference; confirms the cell field layout.
- ASMTRACE live trace + the memories (`reference-re-kb-knowledge-graph`, `maplecast-prod-topology`,
  `feedback-derive-from-marvelous2`, `feedback-insane-overkill-engineering`).

## 6. What to do next + methods

### A. Close re_kb/51 (the transposition) — the last body residual
1. **Capture the exact pose** where the stray part/dark band appears (reproduce the move):
   `node capture_break.mjs --out band.mcrr --frames 300`.
2. **Pull + align the trace**: `scp root@149.28.44.118:/dev/shm/mc_assembly.log .` ; get band.mcrr's
   vframe range; `awk 'NR==1||($1>=LO&&$1<=HI)' mc_assembly.log > asm_band.txt`.
3. **Pin the swap**: `node _diff_sweep.mjs band.mcrr asm_band.txt 15` — the transposition detector
   finds the pair `rf[i]≈asm[j] && rf[j]≈asm[i]` (same-Ax, storage-col reversed). Get the two sels.
4. **Fix**: trace those sels to the walker's storage-column assignment for same-Ax parts (re_kb/21
   `wide_part_tile_storage_order`, re_kb/46 `carve_nonsquare_yfirst_twiddle`) in `render_frame.c` /
   `gen_walker*.c`. Faithful transpile fix.
5. **Verify**: re-run `_diff_sweep` → 0 transpositions; then rebuild `render_frame.wasm` (bundle the
   cull removal), deploy (`scp` to `/var/www/maplecast/render-replica/`, bump `?v=` in
   `webgpu-test.html`), re-capture, confirm clean.

### B. Housekeeping (do once §A capture is in hand)
- Deploy `efxtmpl` region (super/scale-walker, re_kb/50) — same porting move as objpool
  (it's in the local read-set, missing on prod). Supers already look good, so low priority.
- **Disable ASMTRACE on prod** (`MAPLECAST_ASMTRACE=1` off in `/etc/maplecast/headless.env`,
  restart) once we're done tracing — it adds per-frame log I/O.
- Backups on prod: `flycast.bak-objpool-*`, `flycast.bak-seq-*`, `headless.env.bak-asmtrace-*`,
  web `*.bak-capez-*` / `*.bak-lru-*` / `*.bak-seq-*`.

### Deploy patterns (unchanged from prior handoff)
- **Binary**: git-apply the local diff onto `prod-zcs2-shadow` (NEVER checkout), rebuild in
  `/opt/maplecast/src/build-headless` (`cmake --build . --target flycast -j2`), backup +
  `install -m755` to `/usr/local/bin/flycast`, `systemctl restart maplecast-headless`.
- **Web/wasm**: build wasm via emsdk (`~/emsdk/emsdk_env.sh` + the `build_wasm_frame.sh` emcc
  command), backup + `scp` + md5-verify both ends. `.mjs/.wasm` are gitignored build artifacts.
- **Relay**: native glibc `cargo build --release` (our binary needs ≤GLIBC_2.34; prod has 2.39),
  swap `/opt/maplecast/maplecast-relay`, `systemctl restart maplecast-relay`.

## 7. Commit trail (this session, newest first)

`73db3791a` capez deploy · `ee3e60065` cape per-part depth · `9e93ec68c` bodytex LRU + capture tool ·
`004976284` zcs2-test strip-aware · `d433e6946` preserve prod sprite-client hotfix ·
`e7bfb6a56` wire drop-storm fix (relay + seq) · (+ this doc + the diagnostic tools).
Prod `prod-zcs2-shadow`: `8d56255` seq + the objpool git-apply (binary `a1eb501b`).
