# RENDER-STATE appendix 02 — Native GSTA client render path + Phase 2a live audit

> Produced 2026-07-08 by the flycast-internals expert during the RENDER-STATE ledger sweep.
> CONFIRMED = line read in tree; INFERRED = reasoned, with the confirming test named.

## 0. STATUS HEADLINE

- **Phase 2a is in-tree, flag-gated, byte-gate-closed offline, NEVER pixel-validated live.** CONFIRMED commit 483511fef ("NEXT (Phase 2a-live): … then the frozen live pixel gate vs the 7200 mirror").
- **The follow-up commit bc16af338 (same day, 17:47) declares it SHELVED:** "Also in-tree (flag-gated, prod-inert): the shelved render-reconstruction WIP (gsta_charpass native char-pass render, byte-exact; **superseded by lockstep**)." That commit also landed the Phase 2a-live pixel-side wiring (embedded entry ctx, `inPlace` run, `gstaNativeTAFixup`, decode-follows-native) *inside* the lockstep commit — the live wiring exists but was abandoned mid-arc. Record BOTH: (a) lockstep-mirror (`core/network/maplecast_lockstep.{h,cpp}`, MAPLECAST_LOCKSTEP) is the declared successor, live-proven 9121/9121 checksums; (b) Phase 2a is the fallback render-reconstruction frontier if lockstep is rejected (e.g. thin clients).

## 1. Current native GSTA client frame path
(all in `core/network/maplecast_mirror.cpp` unless noted; the GSTA section is inside `#ifdef MAPLECAST_GSTA_CLIENT_BUILD` L3562–5643)

**WS thread** (`gstaClientRun` L5395–5443; spawned by `initGstaClient` L5445–5469, armed by `MAPLECAST_GSTA_CLIENT=1` at L566–574):
1. WS recv + zstd → prefix `gstaApplyPrefix` (L3649, seeds 16MB `_gstaRam` L3603 + vram + pvr_regs) → per-frame `gstaApplyFrame(d,n)` L4542.
2. `gstaApplyFrame` order (L4542–5392):
   - dyn-region splat into `_gstaRam` L4562–4571; GFX tail L4577–4592; **palette tail memcpy'd straight into global `pvr_regs`** L4594–4601 (not staged — gap G6); HUDQ tail L4604–4617; BTCW tail L4619–4634; PL3D tail L4636–4652.
   - `render_frame(&_gstaCtx)` (transpiled geometry) L4656–4660.
   - palette private-bank bake (Dat_Pal @char+0x164 → banks 0–5, writes `pvr_regs+0x1000`) L4662–4726.
   - body parity-pin (arena==400 → texaddr word −0x6000, scope [0x88000,0x8C000)) L4728–4771.
   - stage OP-list emit (`gstaStageEmitTA`) L4779–4806.
   - **PHASE 2a fork** L4812–4888 (§2).
   - `gstaDecodeBodies(nQuad, fr.tiles)` L4910–4911 (texture tiles STAGED, not applied).
   - HUD append L4996–5003; PL3D injection into the stage OP prefix L5005–5171.
   - stage into `_gstaFrame` under `_gstaMtx` + `_gstaReady` L5341–5349 (frame DROPPED if render thread hasn't drained, L5343–5346).

**Render thread** (`core/ui/mainui.cpp`): `gstaModeActive()` → drain `while (clientReceiveGsta(mirrorCtx, vramDirty))` mainui.cpp:166–170; `renderer->Render()` L184; MAPLECAST_GSTA_SHOT PNG via `renderer->GetLastFrame` L188–213. Inside `clientReceiveGsta` (L5474–5634): apply staged tiles `VramLockedWriteOffset` + memcpy L5514–5520; copy TA into `_decodeTaCtx` L5522–5530; seed PVR regs/tile-clip L5545–5565; `vramDirty → renderer->resetTextureCache = true` **L5568** (full TexCache kill per frame); palDirty → `palette_update()` L5569–5572; `renderer->Process(&ctx)` L5575.

## 2. Phase 2a module anatomy (483511fef + live wiring from bc16af338)

Files: `core/network/gsta_charpass.{h,cpp}`, hook in `core/hw/sh4/interpr/sh4_interpreter.cpp` Run() (`if (unlikely(gsta_charpass_active)) gsta_charpass_onpc(ctx->pc)`), selftest call in `core/nullDC.cpp`, `core/network/CMakeLists.txt:4–5` (unconditional target_sources).

- **Mechanism** (gsta_charpass.cpp): swaps global `ReadMem*/WriteMem*/IReadMem16` to working-buffer handlers L129–133; area-3 serviced from `g_wram` L45–67; SQ stores land in `Sh4cntx.sq_buffer` L57–62; `doSqWrite = h_sqCapture` appends every 32B SQ flush to `outTa` L79–82 (**unconditionally, regardless of flush dest** — proven benign offline; INFERRED benign live only if the driver never SQ-copies to RAM on live images). Runs interpreter ENTRY_PC 0x8C030858 → RET_PC 0x8C039648, stop/watchdog via `gsta_charpass_onpc` (watchdog 400M instrs). Saves/restores `Sh4cntx` + handlers. Allocates `p_sh4rcb` if the SH4-off client never did.
- **Entry context**: EMBEDDED 512B `kEntryCtx` L169–202, claimed frame-invariant ("frame-150 RAM + frame-90 ctx reproduces frame-150 engine TA byte-for-byte" — evidence is 2 frames offline). `MAPLECAST_GSTA_CHARPASS_SEED` overrides. QACR0/1 = 0x0C.
- **`run_live`** L234–241: **`inPlace=true` — runs the driver DIRECTLY on `_gstaRam`, mutating it** (safety argument: driver writes only re-shipped render scratch; drop-scratch gate).
- **Injection** (mirror.cpp L4812–4888): under the flag, `run_live` on the WS thread; then:
  - `gstaNativeTAFixup(_nta, _gstaRam)` L4436–4483: walks the parcel stream (32/64B chunking via `p3dPcwHdr64/p3dPcwVtx64`), rewrites each textured global's TCW: base-bank palsel {16,24,32,40,48,56}→private banks {0..5}, + parity pin −0x6000 on arena==400.
  - **decode-follows-native** L4834–4869: extracts sprite TexAddrs from render_frame's TA and the native TA (`gstaExtractSpriteAddrs` L4488–4505), rewrites SceneQuad TCWs so `gstaDecodeBodies` WRITES tiles to the native (authoritative) addresses the native TA SAMPLES. Root cause documented at L4838–4846 ("MEASURED f150: 7/89 sprites differ, incl a duplicate addr → collision → gray/washed body on alternating frames"). Relies on 1:1 same-order sprite correspondence — silent-skip on mismatch (L4858), only a 1-in-120 log. INFERRED risk: live order divergence → decode≠sample garble with weak observability.
  - native TA spliced AFTER the stage OP list + own EOL L4871–4873; transpile fallback if run fails L4887–4888.

**Replaces vs keeps under the flag:**

| Replaced | Kept running |
|---|---|
| `gstaEmitSpriteTA_append` body/effect sprite TA (transpile emit only) | `render_frame()` itself (scene still drives palette bake + decode), `gstaDecodeBodies` (textures), stage OP list, HUDQ append, **PL3D injection — NOT gated on `nativeDone`** |

## 3. Open wiring gaps / live-vs-offline deltas (each = ledger item + falsifiable test)

- **G1 — P3D double-draw (BLOCKER).** Commit message says native "replaces … P3D injection" but the code does not gate the PL3D block on `nativeDone` (`nativeDone` used only at L4826/4874/4887; PL3D block L5017–5019 checks only tail-nonempty + `MAPLECAST_GSTA_POLY3D`). Server ships PL3D by default when replica-live is armed. Native TA contains the effects natively. → Live flag-on draws the cls-0x10 3D-machine parcels TWICE. Workaround: `MAPLECAST_GSTA_POLY3D=0` on the client; proper fix: `if (!nativeDone)` around L5017.
- **G2 — `_gstaRam` is a COMPOSITE, not a driver-entry snapshot.** The byte gate ran on RTSEED02 = coherent RAM at driver entry. The live image = serverPublish-time dyn regions + char-pass-STARTRENDER side-snapshots (tiledesc/idxtab-body0/GFX2/slots+objpool — `maplecast_replica_live.h` L54–104) stitched together. No measurement exists that the live composite equals driver-entry state. Test: flag on, `MAPLECAST_DUMP_GSTA_TA_DIR` per-vframe native TA (L5314–5339) vs the 7200 mirror engine TA at the same frozen vframe. **This is THE live byte gate and it has never run.**
- **G3 — `inPlace` mutation drift.** The driver writes scratch into `_gstaRam` mid-frame. Claim: write-set ⊆ wire-re-shipped regions (drop-scratch) — proven only on recorded frames. If any written byte lies outside the per-frame splat, `_gstaRam` drifts cumulatively. Test: A/B `inPlace=false` over N live frames; diff `_gstaRam` before/after `run_live` vs the union of dyn-region extents.
- **G4 — splice/list-order in flycast's TA FSM.** Offline gate compared BYTES, never flycast's parse of [stage OP + EOL + native segment + EOL + HUD]. The P3D block documents how mid-list globals inherit the open list and EOLs get swallowed mid-strip (L5022–5041, citing ta_vtx.cpp:381–388,476). The native segment through `Process()` is unvalidated; the HUD append strips the native segment's trailing EOL (L4998–4999) and appends paraType-5 quads into whatever list is open. 483511fef itself lists "splice order" as unfinished. Test: `[GSTA] parsed: verts/op/pt/tr` L5590–5596 + `MAPLECAST_DUMP_TR_EXTENTS` L5604–5632, flag on vs off on a held frame.
- **G5 — TexCache.** Live mechanism = FULL cache reset every frame with decoded tiles (L5289 + L5568), so staleness is suppressed by brute force; residual hole: a frame with `written==0` but native TCWs moved (no reset) → stale entry. Test: log frames with `written==0 && nativeDone` live.
- **G6 — palette timing.** Wire palette tail memcpy's global `pvr_regs` on the WS thread (L4598) and the private-bank bake writes `pvr_regs+0x1000` (L4677) while the render thread may be mid-`Render()` of the PREVIOUS frame — palette is NOT frame-staged like tiles. One-frame palette tear possible; pre-existing for the transpile path. Test: consecutive-frame live shots (`MAPLECAST_GSTA_SHOT_EVERY=1`) during a palette-multiplex moment (Cable super).
- **G7 — timing.** "~5.8ms/frame" is the OFFLINE selftest number. Live `run_live` drops the 16MB copy (inPlace) but adds serially on the WS thread on top of splat+render_frame+stage+decode. Budget check: `MAPLECAST_GSTA_PROF=1` → `[GPROF] PRODUCE_FPS` + `AVG_VFRAME_PER_PRODUCE` (L5373–5389) + `[charpass] … ms` L4876–4879. (Live measurement 2026-07-08: 0.95–4.1 ms/frame in-match.)
- **G8 — thread-model caveat.** `run()` swaps PROCESS-GLOBAL memory handlers + `Sh4cntx` on the WS thread. Safe only while SH4 is OFF (GSTA client). **Mutually exclusive with MAPLECAST_LOCKSTEP (SH4 ON)** — must never be armed together. No assert exists.
- **G9 — stale warn string.** L4883–4884 tells the operator to set `MAPLECAST_GSTA_CHARPASS_SEED`, but `run_live` no longer returns false for a missing seed (embedded ctx default) — a false return now means watchdog/fault, a different diagnosis.

## 4. Windows build reality

- `gsta_charpass.cpp/.h` compiled **unconditionally** into every variant (`core/network/CMakeLists.txt:4–5`); SH4 interpreter unconditional (`core/hw/sh4/CMakeLists.txt:16–22`).
- The CALL SITE exists only under `MAPLECAST_GSTA_CLIENT_BUILD`, defined for `MAPLECAST_CLIENT_ONLY=ON` builds (`core/network/CMakeLists.txt:87–105`). Build per `docs/WINDOWS-CLIENT-BUILD.md`.
- `_run_client_shot.bat` arms the flag + SHOT_EVERY=1 frames 200–230 — but is BROKEN as committed (missing `MAPLECAST_MIRROR_CLIENT=1`, wrong port var; see appendix 05 Missing #4).

## 5. "Run Phase 2a live" checklist

1. **Server** (re_kb/52 minimal-stable): `build-headless-win\flycast.exe` with `MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1 MAPLECAST_REPLICA_LIVE=1 MAPLECAST_HEADLESS_AUTOLOAD=1`, oracle/CHARQ/VERIFY_DC UNSET. Serves 7200 (ground truth) + 7212 (GSTA).
2. **No wire change needed**: entry ctx embedded; charpass inputs (arena/tiledesc/idxtab/rectab) already ship.
3. **Client**: native flag + **`MAPLECAST_GSTA_POLY3D=0`** (G1) + `MAPLECAST_GSTA_PROF=1` (G7). Expect `[charpass] NATIVE TA …` every 120 frames; the transpile-fallback warn means watchdog/fault → dump needed.
4. **Live byte gate first** (isolates G2/G3 before pixels): client `MAPLECAST_DUMP_GSTA_TA_DIR=<dir>` + hold a frame + engine TA from the 7200 mirror at the same vframe; byte-diff the char-pass segment. PASS → pixels; FAIL → G2 is the lead.
5. **Live pixel gate** (acceptance): appendix 05 §2 protocol (frozen frame, consecutive shots, diff_png, bboxes).
6. **Parse sanity**: `[GSTA] parsed: verts/op/pt/tr` flag-on vs off; `MAPLECAST_DUMP_TR_EXTENTS` for G4.
7. **Never run with MAPLECAST_LOCKSTEP set** (G8).

**Blockers:** G1 (double-draw — env workaround / one-line gate), G2 (live composite-RAM byte gate never run — the decisive unknown), G4 (splice order explicitly unfinished).

**Superseded-paths note:** transpile `render_frame` body emit = current default (flag off); Phase 2a native char-pass = shelved frontier (byte-exact offline, live-unvalidated); lockstep-mirror (bc16af338) = declared endgame, live-proven, moots the reconstruction stack when the client can run SH4+ROM.

---

## LIVE A/B RESULT 2026-07-08 (first-ever live run of the flag — added by the ledger session)

Local rig (headless `_run_srv_gsta.bat` + client, in-match autoload savestate):
- Flag ON engaged natively every frame: `[charpass] NATIVE TA 135–4540 parcels 0.95–4.13ms` — in budget, embedded ctx, no seed file. One early-frame transpile fallback, then native throughout.
- **Flag ON pixels: character bodies INVISIBLE** (stage + HUD + an electric effect render; no Storm/Cable sprites). `decode-follows-native` mostly `0/N redirected` (once `57/85`).
- **Flag OFF control: bodies render fully** (Storm complete with cape/hair, correct colors).
- Verdict: the live pixel-side (G2/G4/G5/G6 + decode-follows-native correspondence) is the real gap, exactly as the commit's NEXT section declared. The G1 double-draw was also live in the first run (P3D injected AND native effects).
- Rig note: the headless server **crashes when its GSTA client disconnects** (reproduced twice; matches re_kb/52 fragility class) — restart between client swaps.
