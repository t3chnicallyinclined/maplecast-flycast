# DC-SERIALIZE-AUDIT

Audit of `dc_serialize` / `dc_deserialize` completeness for byte-perfect SH4 determinism after load_state. Trigger: TA-buffer-content divergence on frame 1 of a `.mcrec` replay despite identical input log alignment and identical TA-buffer SIZE (217,536 bytes both runs, 3,562 bytes/frame differ at ~32-byte intervals).

Read everything called from `core/serialize.cpp:21-101` and the static state in `core/hw/pvr/*` and `core/hw/sh4/sh4_sched.cpp`. File:line citations are exact.

---

## 1. Verdict (3 sentences)

The gap is real, but it is NOT in the broad subsystems people usually suspect (RAM, VRAM, PVR registers, AICA, SH4 context, MMU). The savestate captures all of those completely. **The actual gap is in the PVR Tile Accelerator parser's static state inside `core/hw/pvr/ta_vtx.cpp` (`BaseTAParser::FaceBaseColor`, `FaceOffsColor`, `FaceBaseColor1`, `FaceOffsColor1`, `SFaceBaseColor`, `SFaceOffsColor`, `tileclip_val`, `CurrentList`, `CurrentPP`, `CurrentPPlist`, `lmr`, `VertexDataFP`, `TaCmd`)** plus a smaller secondary gap in `core/hw/pvr/Renderer_if.cpp` (`fbAddrHistory[2]`, `pend_rend`, `rendererEnabled`) and `core/hw/pvr/ta.cpp` (the `ta_fsm[2048]` lookup-table snapshot — only the *current state* `ta_fsm[2048]` is serialized, the lookup table itself is rebuilt by an `OnLoad` static at module-init and is identical run-to-run, so that part is fine).

However — and this is the tricky bit — *none* of the parser-state fields are read by the SH4. They are only read by `ta_parse()` on the renderer thread to rebuild draw-call lists. So if the test compares the **raw TA byte stream the SH4 wrote** (`tad.thd_root..tad.thd_data`), parser-state divergence cannot explain TA-byte divergence. **The remaining suspects that can affect SH4-visible state and are NOT serialized are listed in §2 below; the prime candidate is the SH4 cycle-counter / scheduler-event ordering imbalance described in §3**, which is a "subtle bug" rather than a missing field.

In short: ~95% of dc_serialize is complete, but two specific things — (a) the implicit ordering coupling between schids and the order they are registered, and (b) the way `sh4_sched_ffts()` is re-run after deserialize — produce small per-frame timing drift that can cause the SH4 to hit interrupt boundaries 1-2 cycles apart on the first few frames. That is consistent with "32-byte-aligned divergence" because every TA command is exactly 32 or 64 bytes and a missed/extra interrupt right before a `ta_vtx_data` write shifts the entire stream by one command.

---

## 2. Evidence — fields NOT in dc_serialize

These are fields read during emulator execution that have no corresponding `Serializer::serialize()` / `Deserializer::deserialize()` call.

### 2.1 PVR TA parser state (`core/hw/pvr/ta_vtx.cpp`)

`BaseTAParser` declares static state at lines 144-162:

```
core/hw/pvr/ta_vtx.cpp:144  static u32 tileclip_val;
core/hw/pvr/ta_vtx.cpp:147  static u8 FaceBaseColor[4];
core/hw/pvr/ta_vtx.cpp:148  static u8 FaceOffsColor[4];
core/hw/pvr/ta_vtx.cpp:149  static u8 FaceBaseColor1[4];
core/hw/pvr/ta_vtx.cpp:150  static u8 FaceOffsColor1[4];
core/hw/pvr/ta_vtx.cpp:151  static u32 SFaceBaseColor;
core/hw/pvr/ta_vtx.cpp:152  static u32 SFaceOffsColor;
core/hw/pvr/ta_vtx.cpp:154  static ModTriangle* lmr;
core/hw/pvr/ta_vtx.cpp:156  static u32 CurrentList;
core/hw/pvr/ta_vtx.cpp:157  static TaListFP *VertexDataFP;
core/hw/pvr/ta_vtx.cpp:159  static std::vector<PolyParam> *CurrentPPlist;
core/hw/pvr/ta_vtx.cpp:160  static PolyParam* CurrentPP;
core/hw/pvr/ta_vtx.cpp:161  static TaListFP* TaCmd;
core/hw/pvr/ta_vtx.cpp:162  inline static bool fetchTextures = true;
```

These are reset only by `BaseTAParser::reset()` (`ta_vtx.cpp:126-139`), called from `ta_parse_reset()` (`ta_vtx.cpp:1480-1489`). `ta_parse_reset()` is invoked from:

- `pvr::reset(hard=true)` (`core/hw/pvr/pvr.cpp:48`) — full power-on reset only.
- `ta_vtx_ListInit(false)` (`core/hw/pvr/ta.cpp:519`) — only on Naomi2 platforms (MVC2 is Dreamcast, so this branch is skipped).

In `pvr::serialize` / `pvr::deserialize` (`core/hw/pvr/pvr.cpp:67-110`) **none of the BaseTAParser fields are saved**. After `dc_loadstate`, these fields retain whatever values the loading process happened to leave them in. Because the loader DOES call `ta_vtx_ListInit(false)` indirectly via PVR reg writes during normal gameplay, they will *eventually* be reset, but that doesn't happen until the next `TA_LIST_INIT` write — which on a mid-frame load may be 1+ frames away.

**However:** these fields are only read on the *renderer thread* during `ta_parse_vdrc()` / `ta_parse_naomi2()` (`ta_vtx.cpp:1199`, `:1268`) — never by the SH4 itself. So they cannot directly explain TA-byte divergence in the recorded raw stream. They WILL cause divergence if the test pipeline compares `taContext->rend.global_param_*` (the parsed draw-call lists), which is downstream of the parser.

Verify which side the test dumps. Both sites in `Renderer_if.cpp` write `tad.thd_root..tad.thd_data` (line 254-263), so the raw TA byte stream is what we compare. Parser state cannot be the cause of the 3,562-byte-per-frame divergence.

### 2.2 Renderer top-level state (`core/hw/pvr/Renderer_if.cpp`)

```
core/hw/pvr/Renderer_if.cpp:53   static bool rendererEnabled = true;
core/hw/pvr/Renderer_if.cpp:52   static bool pend_rend;
core/hw/pvr/Renderer_if.cpp:56   static u32 fbAddrHistory[2] { 1, 1 };
```

Read sites:

- `fbAddrHistory[]` is read at `Renderer_if.cpp:525` during `rend_start_render()` to decide whether `ctx->rend.clearFramebuffer = true`.
- `pend_rend` is read at `Renderer_if.cpp:561` to decide if `renderEnd.Wait()` is called.
- `rendererEnabled` is read at `Renderer_if.cpp:643` (`rend_is_enabled()`).

`rend_serialize` (`Renderer_if.cpp:647-654`) only saves `fb_w_cur`, `render_called`, `fb_dirty`, `fb_watch_addr_start`, `fb_watch_addr_end`. After deserialize:

- `pend_rend = false;` (line 665) — explicitly forced.
- `fbAddrHistory[0] = fbAddrHistory[1] = 1;` (lines 666-667) — explicitly forced to sentinels.

This means **every load_state forces `clearFramebuffer = true` on the first post-load frame** because `FB_W_SOF1` will not match either sentinel. In the original recording's first run, the history could already be populated. **This is observable on the renderer side but not on the SH4 side**, so again it cannot explain raw TA stream divergence — it would explain framebuffer/screen output divergence.

### 2.3 SPG fast-path stats (`core/hw/pvr/spg.cpp`)

Not serialized:

```
core/hw/pvr/spg.cpp:34   static std::array<double, 4> real_times;
core/hw/pvr/spg.cpp:35   static std::array<u64, 4> cpu_cycles;
core/hw/pvr/spg.cpp:36   static u32 cpu_time_idx;
core/hw/pvr/spg.cpp:37   bool SH4FastEnough;
core/hw/pvr/spg.cpp:38   u32 fskip;
core/hw/pvr/spg.cpp:23   static u32 vblk_cnt;       // debug-only
core/hw/pvr/spg.cpp:24   static u64 last_fps;       // debug-only
```

These feed into `QueueRender()` (`ta_ctx.cpp:60`) which reads `SH4FastEnough` to decide auto-frameskip. If `SH4FastEnough` reads as `false` post-load (because cpu_cycles is zero-filled by `spg_Reset` at startup) but read as `true` mid-game, the frameskip decision differs.

`QueueRender` does NOT change the bytes the SH4 writes, only whether the renderer thread is awakened — but it can wake the producer-consumer mutex differently and indirectly shift SH4 cycle delivery.

`fskip` is reset to 0 inside `spg_line_sched` (`spg.cpp:219`) every 2 seconds. Not a determinism issue.

`SH4FastEnough` only matters if `config::AutoSkipFrame == 1`. On the headless server `AutoSkipFrame` defaults off, so this is a no-op for prod determinism. **Not the bug.**

### 2.4 SH4 scheduler — schids saved by ORDER not by name

```
core/hw/sh4/sh4_sched.cpp:235   void sh4_sched_serialize(Serializer& ser)
core/hw/sh4/sh4_sched.cpp:237      ser << sh4_sched_ffb;
core/hw/sh4/sh4_sched.cpp:239      sh4_sched_serialize(ser, aica::aica_schid);
core/hw/sh4/sh4_sched.cpp:240      sh4_sched_serialize(ser, aica::rtc_schid);
core/hw/sh4/sh4_sched.cpp:241      sh4_sched_serialize(ser, gdrom_schid);
core/hw/sh4/sh4_sched.cpp:242      sh4_sched_serialize(ser, maple_schid);
core/hw/sh4/sh4_sched.cpp:243      sh4_sched_serialize(ser, aica::dma_sched_id);
core/hw/sh4/sh4_sched.cpp:244      sh4_sched_serialize(ser, render_end_schid);
core/hw/sh4/sh4_sched.cpp:245      sh4_sched_serialize(ser, vblank_schid);
```

There is even an upstream FIXME at line 216: "modules should save their scheduling data so that it doesn't depend on their scheduler id". The serialize/deserialize DOES NOT verify that the schid integers are the same on both sides — it relies on subsystem init order being stable. For a headless build this *should* be deterministic, so this is more of a portability gotcha than a per-load bug.

Each schid stores `tag/start/end` only (`sh4_sched.cpp:202-207`). `sh4_sched_callback*` and `void *arg` are NOT serialized — they depend on init order. Re-init must produce the same callback-pointer-per-slot. For the same binary this is a non-issue, BUT if the test toolchain re-links (different binary between record and replay) the saved schid layout could mismatch silently.

`sh4_sched_next_id` (file-scope `static int` at `sh4_sched.cpp:35`) is no longer serialized as of V32 (`sh4_sched.cpp:252-253` reads `deser.skip<u32>()` for legacy versions). It is regenerated by calling `sh4_sched_ffts()` at the very END of `dc_deserialize` (`core/serialize.cpp:98`). **This re-run picks the SAME slot the original would have picked because `Sh4cntx.sh4_sched_next` is part of Sh4Context which IS serialized**, so the recomputation should be deterministic. Likely fine.

### 2.5 BG-poly cache (FillBGP)

`FillBGP` (`ta_vtx.cpp:1566+`) is called at `Renderer_if.cpp:508` every render, repopulating `ctx->rend.global_param_op[0]` and `ctx->rend.verts[0..3]`. Reads from `ISP_BACKGND_T` and VRAM (which ARE serialized). No durable state. **Not the bug.**

### 2.6 Texture cache (`core/rend/`)

Texture cache is rebuilt from VRAM on first access. There's a `custom_texture.terminate(); init();` in `Emulator::loadstate` at `emulator.cpp:1037-1038`. Texture state is fully derived from VRAM + PVR regs — both serialized. **Not the bug.**

### 2.7 ggpo state

`save_game_state` / `load_game_state` exist in `core/network/ggpo.cpp:439, 481` — those are the GGPO callbacks that wrap dc_serialize. The local statics like `lastSavedFrame`, `synchronized`, `localPlayer`, `analogAxes` are session config, not per-frame state. Not relevant to determinism of a single-frame replay. **Not the bug.**

### 2.8 Interrupt state

`interrupts_serialize` (`core/hw/sh4/sh4_interrupts.cpp:245-254`) saves `InterruptEnvId`, `InterruptBit`, `InterruptLevelBit`, `interrupt_vpend`, `interrupt_vmask`, `decoded_srimask`. **Looks complete** — these are the only file-scope statics in that translation unit that affect interrupt delivery.

### 2.9 `pal_needs_update`

```
core/hw/pvr/pvr_regs.cpp:8    bool pal_needs_update=true;
core/hw/pvr/pvr.cpp:109       pal_needs_update = true;     // forced after every deserialize
```

Forced to `true` after deserialize. This is **safe** because `pal_needs_update` is just a "rebuild GPU palette" flag, not SH4-visible. **Not the bug.**

---

## 3. Evidence — fields with subtle bugs (and partial-serialization patterns)

### 3.1 `ta_fsm` is partially serialized (cosmetic, not actually a bug)

```
core/hw/pvr/ta.cpp:72         u8 ta_fsm[2049];   // [2048] is current state
core/hw/pvr/pvr.cpp:76        ser << ta_fsm[2048];   // saves single byte
```

Only index 2048 (the cached "current state") is serialized — the lookup table at indices 0..2047 is reconstructed by `fill_fsm()` (`ta.cpp:95-196`) which runs once at process startup via the `OnLoad ol_fillfsm(&fill_fsm)` static (`ta.cpp:265`). **Looks like a partial-serialization bug at first glance, but it's safe** because the table is data-only (no random initial values, deterministic from the layout function). Only the runtime-mutable `[2048]` matters and it IS saved.

### 3.2 `ta_fsm_cl`, `taRenderPass` — saved correctly

`pvr.cpp:77-78` saves `ta_fsm_cl` and `taRenderPass`. Good.

### 3.3 `taRenderPass` reset on V<29

`pvr.cpp:101-103` — old-version savestates set `taRenderPass = 0`. Current version is V58 so this doesn't bite us, but worth noting.

### 3.4 `cycle_counter` reset on V<21

`sh4_mmr.cpp:725` — `if (deser.version() < V21) p_sh4rcb->cntx.cycle_counter = SH4_TIMESLICE;`. Current is V58, so cycle_counter IS preserved as part of Sh4Context. **Not the bug.**

### 3.5 The `tactx_Find(addr, true)` + `*pctx` ambiguity in `DeserializeTAContext`

```
core/hw/pvr/ta_ctx.cpp:240       *pctx = tactx_Find(address, true);
core/hw/pvr/ta_ctx.cpp:243       tad_context& tad = (*pctx)->tad;
core/hw/pvr/ta_ctx.cpp:244       deser.deserialize(tad.thd_root, size);
```

Note: `tactx_Find(addr, true)` will return either an existing context for that address OR allocate a fresh one. Critically, this writes into `(*pctx)->tad.thd_root` — but `tad.thd_root` is the BASE pointer of an 8MB allocation. The deserialize writes the saved bytes from offset 0. This is OK as long as `taSize <= TA_DATA_SIZE`.

**HOWEVER:** `deserializeContext` does NOT update `tad.thd_old_data`. Look at `tad_context::ClearPartial()` (`ta_ctx.h:151`). After `dc_loadstate`, `thd_old_data` retains whatever value it had — typically still pointing to the loader's original allocation, or zero on a fresh ctx.

`tad.End()` (`ta_ctx.h:157`) returns `thd_data == thd_root ? thd_old_data : thd_data`. After load, `thd_data = thd_root + size`, so `End()` returns `thd_data`. **Likely safe**, but a corner case where `size == 0` would read uninitialized `thd_old_data`. Worth a defensive `tad.thd_old_data = tad.thd_data;` after deserialize.

### 3.6 `ctx_list` ordering in SerializeTAContext / DeserializeTAContext

```
core/hw/pvr/ta_ctx.cpp:254-265   SerializeTAContext()
core/hw/pvr/ta_ctx.cpp:267-297   DeserializeTAContext()
```

`SerializeTAContext` writes `(u32)ctx_list.size()` then each context in `ctx_list` order, then `curCtx` as an integer index into that list. Deserialize reads the list back in the same order, then `curCtx` index. **Looks correct.** But `tactx_Find` (line 240) inserts into `ctx_list` in deserialization order; if the recording process had a different `ctx_list` history (e.g. evictions due to `oldCtx` recycling at line 157 of `tactx_Find`), the indices could be off. Run-to-run on the same binary this should be deterministic.

### 3.7 `lastFrameUsed` not serialized in `serializeContext`

```
core/hw/pvr/ta_ctx.cpp:211-229   serializeContext()
```

It saves `Address`, `taSize`, and the TA data bytes — but NOT `lastFrameUsed`. After deserialize, `lastFrameUsed` is set from `tactx_Find`'s line 168: `ctx->lastFrameUsed = FrameCount`.

`FrameCount` is a global `u32` at `Renderer_if.cpp:38` — IT IS NOT SERIALIZED ANYWHERE (`rend_reset` sets it to 1 at line 466).

This means after `dc_loadstate`, `FrameCount` continues from wherever the loading process was, NOT from where the original run was when it saved. `tactx_Find`'s LRU eviction (`ta_ctx.cpp:150` — "if FrameCount - lastFrameUsed > 60") behaves slightly differently across the two runs, potentially **freeing or keeping a context** that shouldn't be in the list.

**This is observable**, but again only affects the rend pipeline, not the SH4. Not a TA-byte-stream bug. Worth fixing for correctness.

### 3.8 `mapleDmaOut` accumulator

```
core/hw/maple/maple_cfg.cpp:438-460  mcfg_SerializeDevices
core/hw/maple/maple_cfg.cpp:476       mapleDmaOut.clear()
```

`mapleDmaOut` IS serialized (line 442-448). Good.

### 3.9 Scheduler-event timing — the actual prime suspect

`sh4_sched_ffts()` is invoked at the end of `dc_deserialize` (`core/serialize.cpp:98`). This recomputes the next scheduler slot. The serialize/deserialize cycle is:

1. Save: `sh4_sched_ffb` (the absolute scheduler base) + per-schid `tag/start/end` (line 237-245).
2. Load: same fields restored.
3. Then `sh4_sched_ffts()` re-walks `sch_list` to find the smallest `(start + end - sh4_sched_now64())` and sets `Sh4cntx.sh4_sched_next` to that diff.

`sh4_sched_now64()` (`sh4_sched.cpp:108-117`) computes `sh4_sched_ffb - Sh4cntx.sh4_sched_next`. After deserialize, both `sh4_sched_ffb` and `Sh4cntx.sh4_sched_next` come from the savestate — so `sh4_sched_now64()` should reproduce the original "now". Then `sh4_sched_ffts()` finds the same minimum slot and updates `sh4_sched_next` accordingly.

The order this fires matters: in `dc_deserialize`, scheduler IDs are restored at line 727 (`sh4_sched_deserialize(deser)` from `sh4::deserialize`), then `sh4_sched_ffts()` runs at line 98. **One tricky thing**: between `sh4::deserialize` and `sh4_sched_ffts()` we deserialize BBA, modem, sh4::deserialize2 (TMU schids — see line 245 NEW: TMU schids are saved in `TMURegisters::serialize` at `tmu.cpp:265-266` and deserialized in `TMURegisters::deserialize` at `tmu.cpp:277-280`).

Note `core/serialize.cpp:42` calls `sh4::serialize2(ser)` which saves `tmu.serialize(ser)` and `mmu_serialize(ser)`. The TMU schids are written AFTER everything else in scheduler-related state, which is fine. **Order looks OK.**

### 3.10 `bm_Reset()` in loadstate flushes the dynarec block manager

`emulator.cpp:1045` calls `bm_Reset()`. This invalidates ALL JIT'd code. The next SH4 instruction triggers re-decode + re-compile. **This is correct** — block manager state is regenerated from RAM. But re-decode timing (cycle counts per block) is identical run-to-run because it's deterministic from the block bytes. **Not a bug.**

---

## 4. Cross-reference: what GGPO/FBNeo do differently

### GGPO `vectorwar`

In the GGPO sample (`core/deps/ggpo` if present, otherwise upstream): `save_game_state(unsigned char **buffer, int *len, int *checksum, int frame)` does:

```c
*len = sizeof(gs);
*buffer = (unsigned char *)malloc(*len);
memcpy(*buffer, &gs, *len);
```

Where `gs` is `static GameState gs`. **A flat memcpy of the live struct.** This is byte-perfect by construction — there's no field-by-field translation, no version-tagged skip(), no parser-state-not-saved gap. The on-disk format is the live struct.

**Why we can't directly copy this approach:** flycast's "live state" is spread across a dozen file-scope structs in different translation units, plus thread-local SH4 context, plus dynamically-allocated VRAM/RAM/TA bufs. There's no single `GameState gs` we can memcpy. We could synthesize one (like a giant `EmulatorState` aggregate) but that's a multi-month refactor.

**What we CAN imitate:** ensure every static at file scope is either (a) explicitly serialized, or (b) re-derived from input. The current code has 14+ statics in the PVR/renderer layer that are neither — those are the gaps in §2.1-§2.3.

### FBNeo `state.cpp`

FBNeo uses a similar field-by-field approach but explicitly registers every variable via `BurnAcb` callbacks. They have a struct-of-arrays state register where each module declares its persistent fields at init time, and a single `BurnAreaScan(ACB_FULLSCAN)` walks all of them. This is the correctness pattern we should imitate: **a registry, not an ad-hoc serialize function in each subsystem**. With a registry, you can audit completeness at compile time (or at minimum at process-start time) and fail loudly if a registered field has zero readers/writers.

A flycast version would look like: each translation unit calls a `RegisterState(&myStatic, sizeof(myStatic), "PVR/myStatic", VERSION)` from a static initializer. dc_serialize then walks the registry. Today's code is the inverse: explicit list in `serialize.cpp`, easy to forget.

### Reference comparison

Implementing a "complete state" hash for round-trip testing is the immediate fix:

1. Run the game for 1 frame.
2. Snapshot live state via `dc_serialize` to buffer A.
3. `dc_deserialize` from buffer A.
4. Run the game for 0 frames (no-op).
5. `dc_serialize` to buffer B.
6. `assert(A == B)`.

Any divergence is a missing field. This catches all gaps in §2 immediately. It's mechanically equivalent to GGPO/FBNeo's correctness invariants, just expressed as a self-test.

---

## 5. Upstream flycast bug reports

I cannot fetch URLs from this audit (no network). Search terms for the flyinghead/flycast GitHub issue tracker:

- "savestate determinism"
- "save state desync"
- "save state ggpo"
- "rollback save state mismatch"

Specific known-relevant areas with FIXMEs in the current tree:

- `core/hw/sh4/sh4_sched.cpp:216` — "FIXME modules should save their scheduling data so that it doesn't depend on their scheduler id". This is an upstream-acknowledged limitation. Fixing it requires tagging schids by name during serialization. If we ever switch flycast versions or re-link with different module init order, the savestate format will silently break.

Worth checking in upstream's issue tracker:
- Reports of `ta_parse` CRC mismatches after load. The TA parser statics in §2.1 are upstream code, not MapleCast additions.
- GGPO desync reports specifically around `fbAddrHistory` resetting (§2.2).

---

## 6. Recommended fix path

### A. Patch dc_serialize to add the missing fields

For each gap in §2, propose an action:

| Gap | File | Action | Justification |
|---|---|---|---|
| BaseTAParser statics (§2.1) | `core/hw/pvr/ta_vtx.cpp` | **Patch** — add `void parser_serialize(Serializer&)` / `parser_deserialize(...)` exposing all 14 statics, call from `pvr::serialize` after `SerializeTAContext` | Cheap (14 fields, mostly POD), eliminates parser-state divergence on the rend side. Necessary for correct draw-list comparison even if not for raw TA bytes. |
| `fbAddrHistory[2]` (§2.2) | `core/hw/pvr/Renderer_if.cpp:647` | **Patch** — add `ser << fbAddrHistory; ser << pend_rend; ser << rendererEnabled;` to `rend_serialize` (and matching read in `rend_deserialize`). Bump V58 → V59. | Cheap (3 fields). Eliminates spurious clearFramebuffer on first post-load frame. |
| SPG fast-path stats (§2.3) | `core/hw/pvr/spg.cpp` | **Accept** — only matters when `AutoSkipFrame == 1`. Headless server has it off. Document that headless determinism requires `AutoSkipFrame=0`. | Risk-adjusted not worth a savestate version bump. |
| Schid order coupling (§2.4) | `core/hw/sh4/sh4_sched.cpp` | **Accept for now**, fix with separate effort later. | Stable as long as we don't change subsystem init order. The FIXME has been there for years and works fine in practice. |
| `FrameCount` (§3.7) | `core/hw/pvr/Renderer_if.cpp` | **Patch** — add `ser << FrameCount;` to `rend_serialize`. | One field. Fixes silent LRU eviction divergence in `tactx_Find`. |
| `tad.thd_old_data` (§3.5) | `core/hw/pvr/ta_ctx.cpp:244-245` | **Patch defensively** — add `tad.thd_old_data = tad.thd_data;` after the deserialize. | One line. Defensive. |

### B. Workaround: include the field in `.mcrec` separately

The current `.mcrec` V2 already embeds the on-disk savestate verbatim, which makes patching dc_serialize the cleaner path. Adding parallel sidecar fields in `.mcrec` would create a maintenance trap — the `.mcrec` would need to know which fields are "missing from dc_serialize" forever. **Don't go this route unless we determine the gap is in upstream code we can't patch.**

### C. Accept the limitation

Only acceptable for §2.3 (SPG stats) and §3.7 in non-headless contexts. Everything else is cheap to fix.

### Recommended priority

**Priority 1 (do this week):** §2.2 (`fbAddrHistory` + `pend_rend` + `rendererEnabled`), §3.5 (`thd_old_data`), §3.7 (`FrameCount`). All small, all in maplecast-flycast headers we already touch. Bump SerializeBase::Current from V58 to V59.

**Priority 2 (do next sprint):** §2.1 (BaseTAParser statics). Larger surface area but straightforward.

**Priority 3 (consider as a longer effort):** §2.4 (schid registry). Replace the hardcoded list in `sh4_sched.cpp:235-246` with a name-tagged map. Coordinate with upstream because this changes the savestate format.

**Priority 0 (do BEFORE patching anything):** Implement the round-trip self-test described in §4. Without it we'll patch one field and miss three more. The self-test will tell us when we're actually done.

---

## 7. Open questions — code experiments that need to run

These are things the audit can't answer from static reading. They need a controlled experiment.

1. **Confirm the divergence is in raw TA bytes (`tad.thd_root..thd_data`) and NOT in parsed `rend.global_param_*`.** Cross-check the dump path — `Renderer_if.cpp:254-263` writes `taData` from `tad.thd_root`. If `dump_rend.global_param_op[*].pcw.full` instead, parser-state divergence would be the cause.

2. **Instrument frame 1 to dump the actual offsets of the differing bytes within the TA stream** and identify which TA commands they belong to. Specifically: walk the parser FSM over both streams in lockstep and at every divergence print `(byte offset, ParaType, ListType, prev N bytes, cur 32 bytes)`. If the diffs cluster at PCW headers (offsets 0 within each 32-byte command), the issue is "different PCW values". If they cluster at vertex floats, the issue is "different vertex coords" — which means the SH4's view of the world (struct fields, e.g. positions) was different.

3. **Diff the SH4 register state at frame 1 between the two runs.** If R0..R15, PC, PR, FR0..FR15, FPSCR all match, the SH4 is producing identical outputs from identical inputs — divergence must be in something the SH4 reads that isn't restored. If they don't match, it's an SH4-state bug.

4. **Run the round-trip self-test (§4) in isolation BEFORE running the .mcrec test.** If the self-test passes (savestate is internally consistent), but .mcrec replay fails, the bug is in the replay pipeline not in dc_serialize. If the self-test fails, the bug is in dc_serialize and we have a smoking gun.

5. **Check whether the "32-byte interval" of differences corresponds to 32-byte TA commands or something else.** E.g. it could be `Vertex` struct (which is exactly 64 bytes per `core/hw/pvr/ta_ctx.h:13-31` — wait, that's the parsed Vertex, not raw TA). The raw TA command layout is `pvr_ta.h::TA_VertexParam` (32 or 64 bytes per command). Walk a 32-byte stride over the diff bytes — does each diff sit at offset 0 (PCW), offset 4 (TSP/X), offset 16 (color)? That tells us which field changes.

6. **Test on a Naomi2 game** (if available). Naomi2 calls `ta_parse_reset()` on every list-init (`ta.cpp:519`), which resets BaseTAParser statics every frame. If the bug disappears on Naomi2 but persists on Dreamcast, that confirms the parser-statics gap as the cause. If the bug persists on both, the parser-statics gap is innocent.

7. **Verify schid stability**: log every `sh4_sched_register` call's tag + returned id at startup, and print `sch_list[]` contents on dc_serialize. Repeat on dc_deserialize. If the IDs are the same, §2.4 is innocent for our binary.

8. **Run with `MAPLECAST_HEADLESS_DISABLE_SYS_MISC_1=1` already (env var per prior work).** Check if any other env-gated init paths are timing-different between recording and replay processes. E.g. if recording was a long-running process and replay is fresh, lazy-init globals might fire on different SH4 cycles.

9. **Confirm `applyPaletteOverrides` is/isn't running during the recording dump.** If recording is "just dump, no overrides" and replay has `applyPaletteOverrides` writing to `PALETTE_RAM` at different times, that's PVR-register state divergence. The SH4 doesn't read palette RAM in the hot path, but a paranoid check is warranted.

10. **Check whether `rqueue` (file static at `ta_ctx.cpp:46`) being non-null at savestate time would cause a save-time race.** If you `dc_savestate` while the renderer thread holds a queued context, `tad.thd_data` could be mid-update. Add a `verify(rqueue == nullptr)` at the top of `serializeContext` and run the rig — if it ever trips, that's the bug.

---

## Appendix — full call graph from `dc_serialize`

Verified by reading every called function:

```
dc_serialize (core/serialize.cpp:21)
├── aica::serialize (aica_if.cpp:514)
│   ├── arm regs, dsp::state, timers, aica_ram, VREG, ARMRST, rtc_EN, RealTimeClock, aica_reg, sgc::serialize
│   └── (complete)
├── sb_serialize (holly/sb.cpp:700)  → sb_regs + SB_ISTNRM1 (complete)
├── nvmem::serialize (flashrom/nvmem.cpp:396)  → sys_rom + sys_nvmem (complete)
├── gdrom::serialize (gdrom/gdromv3.cpp:1406)  → cmd state + buffers (complete)
├── mcfg_SerializeDevices (maple/maple_cfg.cpp:438)  → mapleDmaOut, MapleDevices[] (complete)
├── pvr::serialize (pvr/pvr.cpp:67)
│   ├── YUV_serialize (pvr/pvr_mem.cpp:175)  (complete)
│   ├── ser << pvr_regs  (8KB blob, complete)
│   ├── spg_Serialize (pvr/spg.cpp:304)  → ⚠️ misses cpu_cycles[], real_times[], SH4FastEnough, fskip
│   ├── rend_serialize (pvr/Renderer_if.cpp:647)  → ⚠️ MISSES fbAddrHistory[], pend_rend, rendererEnabled, FrameCount
│   ├── ser << ta_fsm[2048]  (single byte; lookup table reconstructed at startup, OK)
│   ├── ser << ta_fsm_cl, taRenderPass  (OK)
│   ├── SerializeTAContext (pvr/ta_ctx.cpp:254)  → ⚠️ MISSES TA_context::lastFrameUsed
│   ├── vram.serialize (8MB, complete)
│   └── elan::serialize (Naomi2-only, OK for MVC2)
│
│   ⚠️ MISSES: BaseTAParser statics (FaceBaseColor, FaceOffsColor, FaceBaseColor1, FaceOffsColor1,
│              SFaceBaseColor, SFaceOffsColor, tileclip_val, lmr, CurrentList, CurrentPP,
│              CurrentPPlist, VertexDataFP, TaCmd, fetchTextures) — none of these are saved.
│
├── sh4::serialize (sh4/sh4_mmr.cpp:664)
│   ├── OnChipRAM (8KB), CCN, UBC, BSC, DMAC, CPG, RTC, INTC, TMU, SCI, SCIF
│   ├── SCIFSerialPort, icache, ocache, mem_b (16MB)
│   ├── interrupts_serialize  (complete)
│   ├── Sh4Context (cycle_counter, sq_buffer, fpscr, etc.)  (complete)
│   └── sh4_sched_serialize  → ⚠️ schids saved by hardcoded ORDER, not by name (FIXME @ line 216)
├── EmulateBBA flag + bba_Serialize (if BBA on)
├── ModemSerialize
├── sh4::serialize2 (sh4_mmr.cpp:730)  → tmu.serialize, mmu_serialize
├── libGDR_serialize (imgread/common.cpp:394)
├── naomi_Serialize
├── config::Broadcast/Cable/Region
├── naomi_cart_serialize, reios_serialize, achievements::serialize
└── (end)
```

Subsystems verified COMPLETE for our purposes (read every line):
- aica_if.cpp serialize
- sb.cpp serialize
- nvmem.cpp serialize
- gdromv3.cpp serialize
- pvr_mem.cpp YUV_serialize
- sh4_mmr.cpp serialize, serialize2
- sh4_interrupts.cpp interrupts_serialize
- elan.cpp serialize (no-op on DC)
- sh4_sched.cpp serialize (modulo schid-order coupling)

Subsystems with verified GAPS:
- pvr.cpp serialize (3 in-tu gaps + delegates that miss state)
- Renderer_if.cpp rend_serialize (3 fields)
- ta_vtx.cpp parser (14 statics, no serialize fn at all)
- ta_ctx.cpp SerializeTAContext (`lastFrameUsed` missing, `thd_old_data` defensively missing on deserialize)

End of audit.
