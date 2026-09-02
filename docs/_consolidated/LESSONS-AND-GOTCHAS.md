# LESSONS & GOTCHAS — the anti-repeat doc

**This is the single most-referenced doc for any future work on MapleCast.** Every agent dispatch should link it. It exists because this project has *repeatedly re-discovered the same traps* — the texel carve was re-solved ~7 times, rollback state-save was re-derived, the input latch was rebuilt, the dc_serialize gap was re-hunted, and the "prod is ahead of git" surprise has bitten multiple times.

Read this **before** touching build, tests, determinism, netcode, render, RE, or deploy.

Companion docs in this dir: **[INDEX.md](INDEX.md)** (map of all source docs), **[CURRENT-STATE.md](CURRENT-STATE.md)** (what's live/active right now). When a lesson references "the render ledger" that's `docs/RENDER-STATE.md` (authoritative render authority-ranking).

Each entry: **Trap** (what bites) → **Why** (root cause) → **Rule** (what to do instead).

---

## 0. The meta-lessons (read these even if you read nothing else)

- **Offline byte-gate PASS does NOT imply live correctness.** Phase 2a's TA byte gate closed offline (md5 == engine) yet the *first-ever live run rendered character bodies invisible*. The live composite-RAM byte gate has never actually run. Any "byte-exact vs engine" *live* log line is an assertion, not a measurement. → Never claim "done/correct" from an offline gate; require a live frozen-frame pixel A/B.
- **Single-frame / single-pose / eyeball "validation" self-confirms pose-dependent bugs.** The carve bug passed every "validation" on a lucky pose. → Gate discipline is ≥12 poses + frozen-frame pixel A/B with bounding boxes. Numbers, never impressions.
- **A hand-rolled validator can lie.** `_validate_all_multi.mjs` reported diff=0 because its reference assumed the same wrong storage model the decoder used (self-consistent under the wrong model). → Always validate against flycast's OWN output (its `twop`/`ConvertTwiddlePal4`, engine VRAM, engine-rendered pixels through pvr2 via `render_ta.mjs --mirror`), never a hand-authored reference.
- **MEASURE the ROM, never extrapolate from other games or intuition.** MVC2 input lag was *measured* at 1 frame vs an assumed 4–6. Whole-frame TA hashing was *assumed* to dedupe and measured 0% hit. SoA byte-plane transpose was *projected* to help and measured worse. → Every behavioral claim rides a measurement on MVC2 itself.
- **"Verify the verifier."** The expert cross-check that caught real geometry bugs also produced a false alarm (the `0x244` sign-extension error). → Ground RE facts in `re_kb`/marvelous2 read-sites; don't blindly trust a cross-checker either.
- **The project loses built knowledge.** 5+ parallel render implementations, design docs never tombstoned when superseded, gates that closed offline while the live path regressed. → Start every render session at `docs/RENDER-STATE.md`, not the design docs. When you supersede an implementation, update the ledger AND stamp the loser's doc with a 🪦 header.

---

## 1. Build & binaries

- **Trap: editing server C++ and rebuilding only `build/` (or only the Linux binary) — the stale server binary silently masks your fix.** This bit us 3+ times in one session.
  **Why:** server-side changes for local testing run through the Windows headless binary, a *separate* build tree. Rebuilding the wrong one ships old code that "proves" the fix didn't work.
  **Rule:** after any server-side C++ change, rebuild **`build-headless-win` via `_build_headless.bat`**, not `build`. When "the fix didn't take," clean-rebuild the *correct* binary before debugging anything else. (Same lesson on VPS: a STALE incremental build silently ships a binary missing the fix — clean-rebuild before debugging "didn't take".)

- **Trap: three mutually-exclusive CMake build modes share one source tree; a change lands in the wrong variant.**
  **Why:** `MAPLECAST_HEADLESS=ON` (VPS server, ~26 MB Linux/~9 MB Win, no GPU), `MAPLECAST_MIRROR_CLIENT=1` env / `MAPLECAST_CLIENT_ONLY=ON` (native mirror client), `packages/renderer` emsdk (WASM king.html), legacy EmulatorJS core — all from the same tree.
  **Rule:** always disambiguate which binary your change targets. Name it explicitly in the dispatch.

- **Trap: editing `core/network/maplecast/{client,server}/` or `maplecast_mirror_{client,server}.cpp` and nothing changes.**
  **Why:** those trees are DEAD CODE, not in the build (`README_DEAD_CODE.md`). Single-binary env-var mode-switching is the working path.
  **Rule:** don't touch the two-binary split. Work in the live single-binary path.

- **Trap: Windows headless build fails with `d3dx9shader.h` / `dl.lib` not found.**
  **Why:** you configured *without* `-DMAPLECAST_CLIENT_ONLY=ON`; the default build pulls DX9 (needs the deprecated June-2010 DirectX SDK) and a CUDA block (needs Linux-only `dl`).
  **Rule:** set the mode flag. For the native client: `-DMAPLECAST_CLIENT_ONLY=ON` + vcpkg `curl:x64-windows` + `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`.

- **Trap: native client runs but immediately fails — `libcurl.dll not found`, or no console output.**
  **Why:** CMake does not auto-copy vcpkg DLLs; `winmain.cpp` skips `AllocConsole()` unless `MAPLECAST=1`.
  **Rule:** manually copy `libcurl.dll` (and `zlib1.dll`) into `build/`; set `MAPLECAST=1` to attach the console. Run from a *regular* PowerShell, not a dev prompt (`vcvars64.bat` is invoked inline).

- **Trap (Windows headless): the emu thread never advances past frame 0 — fps 0, CPU ~0.5%.**
  **Why:** it stalls inside `InitAudio()` (`emulator.cpp` ~1563). This blocked live effect-capture validation for weeks.
  **Rule:** fix the emu stall (investigate the InitAudio hang) BEFORE attempting any live effect capture; a frozen frame-0 emu makes every live gate meaningless.

- **Trap: WASM king.html renders with broken transparency / wrong background after a change.**
  **Why:** Emscripten C-level `glEnable()` inside GLSM does NOT propagate to the WebGL2 context; and the server gets `FillBGP()` for free but the WASM client must call it manually before `Process()`.
  **Rule (Path A renderer):** JS must re-issue `gl.enable(BLEND|DEPTH_TEST|STENCIL_TEST|SCISSOR_TEST)` before every `_renderer_frame()`, and call `FillBGP()` before `renderer->Process()`.

- **Trap: EmulatorJS core serves a stale build during dev; CHD ROMs won't load; canvas stays black.**
  **Why:** EmulatorJS caches cores in IndexedDB for 5 days; `chd_stream.o` must stay in the archive; `_mirror_render_frame` renders to an FBO but `video_cb` won't present on its own.
  **Rule (Path B):** `EJS_cacheConfig={enabled:false}` during dev; keep `chd_stream.o`, strip `ZipArchive.cpp.o` (pulls unavailable libzip); `mirror_present_frame` must explicitly call `video_cb`; `emu.pause()` to stop the dual-render flicker.

- **Trap: `graphify .` at repo root melts down.**
  **Why:** this is a ~900K-LOC vendored Flycast fork (~9,600 C++, ~6,000 Python, 758 markdown). And it would duplicate the canonical `tools/re_kb/` SurrealDB RE graph.
  **Rule:** never run graphify at root; scope to `docs/`, `re-catalog/`, `core/network/`, `tools/`. Keep `re_kb` authoritative for RE facts; graphify is docs/prose-lane only, if at all.

---

## 2. Testing & gates

- **Trap: an IDLE test passes and you conclude the feature works — but input-path/determinism bugs hide in idle.**
  **Why:** with no changing input, the latch, the input-atomic race, and the rollback compare path never fire. The whole point of those systems is *transitions*.
  **Rule:** any input or determinism gate MUST drive **changing** input (button edges, presses+releases), never idle. (The dashing bug was invisible until a 50/50 press+release test ran.)

- **Trap: the determinism rig is run after every edit and burns time / gives false confidence.**
  **Why:** it's the FINAL gate, not a per-edit check. Each policy/path has its own baseline; intentional wire-byte differences (e.g. ConsistencyFirst) are expected.
  **Rule:** run `MAPLECAST_DUMP_TA` determinism rig ONCE at phase end after live verification. If it fails, backtrack. Baseline each policy separately.

- **Trap: claiming a live render is correct from a throttled log line.**
  **Why:** the live path computes no per-frame md5 and has no per-frame engine reference — the "byte-exact vs engine" text is carried over from the offline gate.
  **Rule:** the acceptance gate (G13) is savestate-freeze + framebuffer A/B: `savestate_save` over control-WS 7211 → RESTART headless with `MAPLECAST_HEADLESS_AUTOLOAD=1` (savestate_load over WS is hard-disabled; restart IS the freeze) → capture both legs with `MAPLECAST_GSTA_SHOT` → diff with `tools/render-replica-poc/diff_png.mjs`. Pass = char-pass-region diff pixels 0 vs mirror (full-frame 0 NOT expected: HUD is Phase 2b, stage separate); native must be ≥ transpile (native>transpile in any region = regression).

- **Trap: cross-leg shot pairing goes wrong when the scene animates.**
  **Why:** shot index `n` is the client-local render counter (`_grn`), NOT the vframe; pairing two legs by index is valid ONLY because the frozen savestate makes them the same moment.
  **Rule:** keep the frozen-state constraint. If the moment animates, index-pairing is invalid.

- **Trap: reproducible "done" claims fail because the tooling isn't in the repo.**
  **Why:** `gate_check.mjs` (the 189544592 standing gate) lived in an ephemeral scratchpad; `_run_client_shot.bat` is broken as committed (missing `MAPLECAST_MIRROR_CLIENT=1`, wrong port `7212` under `MAPLECAST_SERVER_PORT` instead of `MAPLECAST_GSTA_PORT`); there's no live per-frame TA md5 gate; `diff_png.mjs` is full-frame only (no `--rect`).
  **Rule:** treat these as known gaps — re-land `gate_check.mjs`, fix the bat, add a live md5 gate and a region mask before trusting a "gate passed" claim.

- **Trap (Windows): dump output silently vanishes.**
  **Why:** `MAPLECAST_DUMP_TA_DIR` (and Windows companion dumps hardcoded to `/dev/shm`, oracle_hook ~3545) default to POSIX paths that fail SILENTLY under MSVC.
  **Rule:** always pass a native Windows dir for dump paths.

- **Trap: stacking instrumentation crashes the Windows headless (`0xC0000005`).**
  **Why:** `MAPLECAST_READTRACE` + oracle-hook/CHARQ together (re_kb/52 fragility class).
  **Rule:** launch minimal instrumentation only, from the repo/build dir, log via redirected file. Also: the headless server CRASHES when its GSTA client disconnects — restart between client swaps.

---

## 3. Determinism & state

- **Trap: reintroducing any of the six race fixes (commit `466d72d54`) — the byte-perfect wire silently breaks.**
  **Why:** these are load-bearing and their symptoms are subtle (scene-transition garble, whole-stream misalignment).
  **Rule — NEVER re-add these regressions:** (1) `DecodedFrame::pages` must be `std::vector`, not a fixed array (transitions ship 100–200+ pages); (2) TA delta `runLen` MUST clamp to 65535 BEFORE gap-merge (uint16 wrap → ships runLen=7 but copies 65543 bytes); (3) diff loop snapshots live→shadow ONCE per dirty page (never re-read `reg.ptr` between memcmp and memcpy); (4) producer MERGES prev frame's unconsumed pages (don't `std::move`-overwrite `_decoded`); (5) PVR atomic 32KB snapshot at top of `serverPublish()` (spg.cpp races SPG_STATUS); (6) `_decodedMtx` mutex on producer/consumer.

- **Trap: re-adding a workaround that "fixes" garble.**
  **Why:** the workarounds that MASKED those races were deliberately removed — DMA dirty bitmap, scene-change `broadcastFreshSync` heuristic, periodic 10s safety SYNC, onOpen FSYN broadcast.
  **Rule:** if you reach for one of these, you have a determinism-rig regression instead. Fix the race, not the symptom.

- **Trap: `emu.loadstate()` for live resync — SIGSEGV after ~1000 frames.**
  **Why:** it corrupts scheduler/DMA/interrupt state; also boot/loadstate mprotect VRAM read-only and silently drop memcpy patches.
  **Rule:** use direct memcpy of RAM/VRAM/ARAM for live sync; always `memwatch::unprotect()` after any state sync.

- **Trap: the dc_serialize round-trip poisons replay — ~3900-byte frame-1 TA divergence with the SAME binary/savestate/no inputs.**
  **Why:** runtime `dc_serialize → dc_deserialize` misses ~14 file-scope statics (Renderer_if `fbAddrHistory`/`pend_rend`/`rendererEnabled`, `FrameCount`, `thd_old_data`, BaseTAParser statics) and drifts scheduler-event timing (`schids` saved by hardcoded ORDER not name). Step C worked only because both runs loaded `mvc2.state` from DISK (no runtime round-trip).
  **Rule:** DON'T round-trip at runtime — use the on-disk `mvc2.state` via the existing `dc_loadstate(SavestateSlot)` autoload path (or power-on boot). In-memory rollback state copies may sidestep the gap; file-replay does not. If you must patch dc_serialize, build the `serialize→deserialize→serialize` round-trip self-test FIRST (it catches all gaps mechanically). Never call `dc_deserialize` raw — mirror `dc_loadstate`'s wrappers (RAM-watch reset, `EventManager::event(LoadState)`).

- **Trap: assuming savestates/.mcrec are portable across flycast commits.**
  **Why:** "crash B" was old-binary bytes read into new struct shapes — we had NO version check anywhere.
  **Rule:** embed a `build_id`/commit hash; refuse mismatches (FBNeo's `nMinimumMovieVersion` pattern), fall back to no-prediction mirror mode. Predictor and server MUST run the same commit. Cross-version and emscripten/wasm determinism are NOT validated — re-validate any new toolchain (same savestate + `.mcrec`, `MAPLECAST_DUMP_TA`, byte-compare).

- **Trap: headless determinism silently depends on config.**
  **Why:** `AutoSkipFrame==1` engages the unsaved SPG fast-path stats; the headless `Dreamcast.AutoLoadState=yes` cfg is SILENTLY IGNORED.
  **Rule:** headless determinism requires `AutoSkipFrame=0`. For autoload use the env var `MAPLECAST_HEADLESS_AUTOLOAD=1` (the cfg is ignored).

- **Trap: MVC2 attract-mode crashes flycast at ~75s (SIGSEGV in `bm_GetCode` addr `0xA0000000`).**
  **Why:** SH4 soft-reset the dynarec can't handle.
  **Rule:** drop a savestate + `MAPLECAST_HEADLESS_AUTOLOAD=1` so the game boots to a stable screen and never reaches the reset path.

- **Trap: `atexit` doesn't fire on `systemctl stop` → footerless `.mcrec` files.**
  **Why:** signal-terminated processes skip atexit.
  **Rule:** per-frame `fflush` + SIGTERM/SIGINT handler that calls `stop()`; write a tentative footer after every flush. Fix the writer — don't build reader robustness to tolerate corrupt files (inverted priority).

---

## 4. Netcode / rollback / input

- **Trap: a separate PUSH thread injecting recorded inputs into the live input atomic → use-after-free / wild JIT jump (the `0x5e6bb82b5f80` SIGSEGV, 100% reproducible).**
  **Why:** the SH4 thread also reads that atomic; two threads racing a system the SH4 reads is the fault.
  **Rule:** rollback = replay = spectator must be ONE code path with a **PULL-model** input read — the SH4's own input-read function (`ggpo::getLocalInput`, single-threaded by construction) returns recorded data indexed by frame. NEVER use a playback thread.

- **Trap: state-restore or record-start fired too early → crash referencing nonexistent JIT addresses.**
  **Why:** firing during `input_server::init` (before the dynarec/JIT cache exists) loads a PC into non-code.
  **Rule:** fire state-restore AND `replay_writer::start` at the `emulator.cpp` autoload point where `dc_loadstate(SavestateSlot)` already runs and the JIT is fully initialized. On Windows, `os_InstallFaultHandler()` is mandatory or the first vmem fault silently kills the process.

- **Trap: replay falls through to live SDL globals at frame zero → non-deterministic start.**
  **Why:** the input log is interleaved per-packet; before the first entry is seen `getInputAtFrame` returns FALSE and the SH4 grabs whatever SDL had.
  **Rule:** if replay is active and no entry seen yet, return NEUTRAL (0xFFFF/0/0) and TRUE. Replay must NEVER fall through to live globals.

- **Trap: per-slot pace counters drift.**
  **Why:** `_replayPaceFrame[2]` advancing independently per slot only works because both slots are read once per SH4 frame; a 1P-only mode desyncs them. And `maplecast_mirror::currentFrame()` only advances in mirror mode.
  **Rule:** key replay off the SH4's ONE authoritative frame counter, the way `ggpo_synchronize_input` does.

- **Trap: the recorder logs the input atomic at END-of-frame but the SH4 reads it at START-of-frame (~14ms apart).**
  **Why:** between those two points the input thread can change the atomic; the recorded value is "atomic at end-of-frame", not "what the SH4 read".
  **Rule:** fine for watch-back replays; NOT acceptable for rollback (every frame must be byte-exact). Hook `ggpo::getInput`'s tail and log what was actually returned to the SH4.

- **Trap: using the full savestate for rollback state-restore (slow), or a `rollback=false` hybrid (wrong).**
  **Why:** the full 27 MB savestate is far too heavy per-frame; the non-rollback path doesn't do block-invalidation on write so the restored state is stale/wrong.
  **Rule:** rollback state-restore uses flycast's **`rollback=true` page-delta + memwatch** (block-invalidation on write). Not the full savestate, not the `rollback=false` hybrid.

- **Trap: the client guessing which server frame its input lands on.**
  **Why:** in an authoritative-server topology the client can't know the server's landing frame; guessing desyncs the rollback comparator.
  **Rule:** the SERVER assigns the input landing frame (arrival + fixed delay) and echoes it back, so the client doesn't guess.

- **Trap: adding a third buffer layer or moving the latch hook to "fix" input timing.**
  **Why:** the two buffers (`_slotInputAtomic[2]` packed 64-bit live word, `mapleInputState[]` frame-stable snapshot) are correct; the accumulator IS the third layer but lives next to the live word, not between live and latched. The latch fires once per frame at the top of `maple_DoDma()` (vblank boundary) where CMD9 reads microseconds later.
  **Rule:** don't add buffers, don't move the hook, don't widen the guard window past ~1–2ms (default 500µs; 5ms feels like lag), don't reintroduce input prediction ("hold previous" → doubled/reordered inputs), don't double-debounce NOBD's firmware 5ms window. The packed atomic is `[buttons:16][lt:8][rt:8][seq:32]`, single release/acquire (verified 19.89B reads, 0 torn).

- **Trap: an unauthenticated `set_latch_policy` from a spectator's dev console changes another slot's policy.**
  **Why:** the UI hide is not the security boundary.
  **Rule:** the `getSlotForConn(hdl)` gate is the ONLY thing stopping it — any new WS handler calling `setLatchPolicy()` must gate the same way. Don't default `MAPLECAST_LATCH_POLICY=consistency` globally (current default is `latency`; ConsistencyFirst is a *different tradeoff*, +1 frame jitter on near-boundary/blip inputs, not "better").

- **Trap: the hub in the gameplay hot path.**
  **Why:** the hub does discovery + matchmaking ONLY; putting game data through it adds latency and a single point of failure.
  **Rule:** once a match is assigned, browsers/clients connect DIRECTLY to the input server. Never proxy game data through the hub. (The hub's in-memory store is volatile — the 404-reregister fallback in `relay/src/hub_client.rs` recovers; don't remove it.)

- **Trap: the relay forwarding join/leave control messages upstream to flycast → slot conflicts.**
  **Why:** the relay manages its own roster; forwarding makes relay and flycast disagree on who's connected.
  **Rule:** relay forwards ONLY gamepad input + queue commands upstream (`relay/src/fanout.rs` ~553). Join/leave go via the browser's direct `/play` WS.

- **Trap: MVC2 input lag treated as 4–6 frames.**
  **Why:** extrapolated from other games / assumption.
  **Rule:** it's MEASURED at 1 frame. The P1 raw controller byte is `0x8C200BA8` (active-low, maple-latched; gameplay acts +1 frame after the latch). Use the measured value.

---

## 5. Render

- **Trap: hand-rolling or re-serializing flycast's renderer / re-parsing geometry server-side → garble (stretched tris, HUD textures painted on characters, smeared strips).**
  **Why:** `PVR2Renderer` is validated against exactly ONE producer shape — a degenerate-linked triangle STRIP, 28-byte/vertex buffer (col/spc as R,G,B,A bytes), `PolyParam{first,count,isp,tsp,tcw,pcw,tileclip}` spanning that strip, raw PVR words, `paraType==4`. Any different layout re-triangulated on the server breaks the contract. The surrogate→texture mismap that painted HUD on characters existed ONLY in a layer added to work around a problem we created (the server re-serialize).
  **Rule:** the server's ONLY geometry job is to de-index `rc.idx` → contiguous verts and ship the strip VERBATIM; ALL triangulation/winding/modulate happens in the client's `PVR2Renderer`. Prefer the path that touches neither the renderer NOR the VRAM decoder.

- **Trap: TA geometry rendered by iterating raw `rc.verts` or Canvas2D blits → long stage strips and tagged-in character strips smear across the frame.**
  **Why:** `ta_parse`'s `makeIndex` rewrites `PolyParam.first/.count` to index `rc.idx` (a strip with degenerate links + alternating winding). Iterating `rc.verts[pp.first+i]` only works for isolated 4-vert quads. Canvas2D can only do axis-aligned image blits.
  **Rule:** use the index buffer with alternating winding (tri1 `[2,1,3]` reversed, tri3 `[4,3,5]` reversed) and degenerate-link skipping; rasterize textured triangles on the GPU.

- **Trap: caching/identifying textures by VRAM address → stale/garbled textures, broken skins.**
  **Why:** VRAM addresses are REUSED (same TCW addr holds different content across frames as the game DMAs new sprites); textures are twiddled/VQ-walked, not linear.
  **Rule:** identity is a 64-bit content hash — `texHash64` FNV-1a over `fmt|w|h|vq` + raw texel bytes + (for paletted fmt 5/6) the live palette window. Address EXCLUDED from the key. Palette MUST be in the key for paletted formats or skins/team-color swaps render stale.

- **Trap: classifying effect/HUD quads by additive-blend or screen-strip POSITION (the `MAPLECAST_HUDF` heuristic) → the garble-flash-on-supers bug.**
  **Why:** position strips miss non-additive HUD bars; the additive heuristic grabs additive STAGE grid geometry. There is NO effect `sprite_id` namespace.
  **Rule:** identify effects by the draw object's GFX source pointer — is `node+0x15c`/`+0x160` in the Effect-Poly region `[0x0CED0000,0x0CF00000)`? Route by GFX base pointer, NEVER by sprite_id/blend/position.

- **Trap: the Effect-Poly bank `0x0CED0000` parsed as a GFX1 count+relative-offset table → pink-streak/blue-block garble.**
  **Why:** it's an absolute-pointer DIRECTORY (head `0x0CED0010`, base `*(0x0CED0008)`, 0x10-byte entries), NOT a GFX1 table; its texel source is 13.6 MB system RAM DMA'd to a dynamically-allocated VRAM slot.
  **Rule:** SKIP effect-bank gfx1 in body decode (`fc7072a69`, proven) — the texture is already resident in shipped VRAM via the resident-template TCW.

- **Trap: the texel carve — re-solved ~7 times, the single most re-hit bug family.**
  **Why:** the engine's chunk order is 2-ROW BANDS (`by=row&~1; k=by*Tw+col*bh+(row-by)`), which equals Y-first for any grid with a dim ≤2 — so every earlier single-pose "validation" self-confirmed. Carve-per-tile pitch is `m=W/cols=H/rows` ∈{8,16,32}, not hardcoded 32. Native-chunk twiddle carve is correct ONLY for SQUARE grids (`Tw==Th`); non-square falls back to linear-slice.
  **Rule:** FINAL answer = 2-row-band chunk order (`body_decoder.mjs:478-487` + `gstaDecodeBodies` lockstep, commit `189544592`); verify ≥12 poses. And validate offline decode against DISC GFX1 or the in-RAM `node+0x15C` base, NEVER disc==RAM offsets (in-RAM PLDAT is relocated/re-packed at load).

- **Trap: editing `gstaDecodeBodies` (mirror.cpp) OR `ensureBodyTextures` (body_decoder.mjs) alone.**
  **Why:** they implement the SAME carve in LOCKSTEP (stated in-code at mirror.cpp:4092/4130/4181, body_decoder.mjs:478-487). Editing one desyncs the decode.
  **Rule:** the LOCKSTEP INVARIANT — always edit both together.

- **Trap: overriding palette-bank TCW PalSelect with the static formula `16*(char_pair+1)+8*player_side` → "Cable-all-blue".**
  **Why:** the engine allocates banks dynamically and uses ODD siblings (17,25); the static even-only formula is wrong.
  **Rule:** PRESERVE the resident rectab TCW PalSelect (`f2e81a82f`). Also write the sprite BASE COLOR at +16 (zero face color × MODULATE = black canvas). And "purple Cable" is engine-faithful (bank 24 IS purple) — identify chars by `char_id`, NEVER by color; a palette that looks wrong can be exactly what the engine renders.

- **Trap: flattening all HUD/object quads to z=1.0 in one flat TR list → the all-red-bars bug (para4 red backing covers para5 team-color fill).**
  **Why:** z-order is by list-type then submission order (OP→PT→TR), not depth. `buildHudTA` flattening loses the layering.
  **Rule:** emit each object in its real PCW ListType with EOL between lists. STAGE=OPAQUE (ListType0/ParaType4/PCW 0x80); BODIES=TRANSLUCENT (ListType2/ParaType5, PCW|=0x02000000). Don't enable Z-sort with depth write (flickers on overlapping translucent geometry) — submission order works because MVC2 emits translucent roughly back-to-front.

- **Trap: whole-triangle MARGIN culling drops the giant stage floor quad → black floor/deck.**
  **Why:** the blue lower-deck floor (mesh3 tcw 0xa0000) spans X-6822; rejecting a whole triangle if ANY vertex leaves `[-800, dim+800]` dropped it. The engine submits the full quad and lets the PowerVR guard-band clip.
  **Rule (re_kb/47):** drop the whole-tri MARGIN reject; reject a vertex ONLY if non-finite or `|screen|>1e6` (the 1/w=10 sentinel); keep a triangle iff its screen bbox overlaps the visible frame ±64px. (Same MARGIN cull lives in `stage-client.mjs _buildFromTA`.)

- **Trap: super/projectile over-tiling (~29–34× explosion).**
  **Why:** THREE root causes (re_kb/50): (1) bit15 = SCALE-walker dispatch (`loc_8c0348c8`, one scaled sprite per record), NOT tiling and NOT flip — engine routes effect nodes via `& 0x8000` to the scale walker; (2) the per-frame effect TEMPLATE `efxtmpl` (node+0x180) must ship per-frame; (3) idxtab effect-range needs client-side remap (`effect_block_start + (alloc_index - idx_base)`). During super freeze `node+0xDC` reads 0 for all nodes (engine defers the pass during hitstop), so a stale snapshot gives 8 tiles × ~28 nodes.
  **Rule:** don't ship a blind fix — acceptance gate is GSTA per-frame quad count == mirror quad count on a fresh live A/B.

- **Trap: reasoning from the emitter model / chasing pixel symptoms before checking facing & render-model.**
  **Why:** the "detached forearm"/wrong-default-facing/half-body-swap was ONE facing bug (texU mirror locked to `!facing` instead of raw `facing XOR 0x4000`), not missing pixels. Atlas coverage is 100% — render bugs are RULE bugs, not missing assets.
  **Rule:** check facing/render-model first. Settled geometry facts (do NOT re-derive): `facing=1` faces RIGHT (setter `loc_8c0d97ee`); texU mirror = raw `facing XOR 0x4000`; reflect the pen ORIGIN only (`tlx=2A−tlx`) never the rect; tileScale=1.0 with full anisotropic CPS (Sx=1.6667/Sy=2.1428, NOT the 1.75 lock); atlas parts stored bottom-up → V-flip in sampling only; full-span (sw·8×sh·8) not logical-crop; intra-assembly z record 0 = FRONT.

- **Trap: dx/dy treated as absolute per-record offsets → parts pile at origin (158px scatter vs correct ~94px body span).**
  **Why:** they're a CUMULATIVE running pen (facing-neutral), superseded the older "absolute" model (MARVELOUS2-GFX-NOTES §6 → §3a).
  **Rule:** X-acc ±dx gated by facing, Y-acc −=dy; final screen = `node+0xE0/E4 + (acc+tile)·scale` where scale = `node+0xEC/F0` (resolved screen-space, NOT world +0x50/54).

- **Trap: packing full storage tiles → 2× oversized / upside-down parts.**
  **Why:** GFX1 header `[lw][lh][sw][sh]` = LOGICAL vs STORAGE dims; the engine draws ONLY the `lw·8 × lh·8` logical region (real pixels bottom-left, parts bottom-up).
  **Rule:** crop to logical bottom-left before packing (`extract_gfx1_atlas.py decode_part`). NOTE: an earlier "logical-crop" note was itself the UPSIDE-DOWN regression — the correct answer is full-span for the atlas sampling but logical for the crop; when in doubt, byte-compare against flycast.

- **Trap: the browser render default is still EMITTER — the source of body/HUD/anchor/white-wash garble.**
  **Why:** the EMITTER path reimplements raster + GUESSES render props and did NOT inherit the hardened fixes; `replay.html:1811` still defaults to `emitter`.
  **Rule:** the single biggest browser win is switching the default to `render_frame` (`replay.html?bodymode=render_frame`) — it + `body_decoder.mjs` inherit every measured fix for free. Carry as OPEN. (Cockpit knobs `_asmCfg/_objAnchor/_emitFaceInv` etc. belong to the RETIRED emitter path — don't "re-calibrate" them.)

- **Trap: super/aura complex geometry treated as a reconstruct-from-state gap to close.**
  **Why:** input/RNG-driven multi-strip additive geometry CANNOT be rebuilt from a few state bytes — the deliberate architecture STREAMS these as real quads.
  **Rule:** this is a boundary, not a gap. Reconstruct bodies + simple typed effects; stream the chaotic tail.

- **Trap: shipping the wire's COPY/duplicate field offsets instead of the authoritative ones → recurring facing/palette/flash bugs.**
  **Why:** the shipped wire reads copies: facing from `0x110` (copy) not authoritative `0x1D2`; palette from `0x52D` (dup) not the live-tint `0x25`; hit-flash from `0x40` (PALF, empirically 0 on normal hits) not the real selector `0x12e`. The renderer actually reads `0x12d/0x12e/0x25/0x151/0x1a4`.
  **Rule:** ship the authoritative fields. NEVER ship the engine-owned pointer cluster `0x154–0x184` (the injection crash source). Wire bumps must be a TRAILING block + update all four parsers together.

- **Trap: whole-frame / game-state hashing to dedupe (0% hit rate three separate times).**
  **Why:** timer + HUD + animation ticks make every full TA buffer unique; a 60-frame move = 60 unique keys, entries ~200 KB each (5.7 GB from 10 min).
  **Rule:** dedup granularity is per-draw-call TA parameters keyed by `(TCW,TSP)` — the WebGPU texture-manager already does this with 0 steady-state misses across 24,000 frames. Never key on surrounding state. Also: headless skips `TexCache::Update()`, so `captureTexture()` never fires on the VPS — don't try to bundle decoded textures server-side.

- **Trap: WebGPU texture/color bugs.**
  **Why/Rule:** VQ codebook `p[1]=(1,0)` top-right NOT bottom-left; bit-replication `(v<<3)|(v>>2)` NOT `v*8`; do NOT multiply vertex colors by Z; match flycast's front-face cull convention. The green flash during supers is the game legitimately changing FillBGP for 1–2 frames — NOT a bug; masking it hides legit BG changes. Never read the presented WebGPU canvas for diffing — read the 2D mirror canvases fed at each present site.

- **Trap: citing the wrong bandwidth number in a decision.**
  **Why:** three figures circulate — 4.1 Mbps (the ONLY conditions-stated anchor, Apr 2026), 1.7 MB/s (unlabeled uncompressed), 36–88 Mbps (conditions never stated, drove the "pixel-shipping DEAD" verdict). The spread was decision-load-bearing.
  **Rule:** cite only the anchor. Fresh 2026-07-08 measurement = **6.875 Mbps** in-match (grown from DMA force-dirty page re-ships — a workaround the deterministic wire had removed; investigate WHY it ships). A measured compression stack reaches **0.788 Mbps at 0.23ms/frame, zero loss**. The single biggest quick win: hash-gate the DMA force-dirty ships (server-only, kills 56.9% of page ships). SoA 32-B transpose measured WORSE (rejected with numbers) — don't revisit.

- **Trap: resuming a dead render campaign.**
  **Rule — do NOT resume (per RENDER-STATE.md):** client 3D-machine re-impl (the 3D machine is INSIDE the Phase 2a native TA by construction); P3D capture/OP-prefix injection as a render path (keep only as a measurement tool); render_frame hand-assembly patch stack; HUDQ-as-renderer (survives only as the Phase-2b byte-gate oracle); HUD leaf-transpile (bars are coordinate-less — `loc_8c0f06ec` writes no x/y/uv); effects-atlas binding heuristics; per-prop matrix-stack capture; live-SH4 state injection; TX64/STAF/VCACHE pixel-shipping; whole-frame TA dedup.

---

## 6. RE (reverse-engineering MVC2)

- **Trap: guessing a format/mapping byte and "coincidentally" matching a subset of cases.**
  **Why:** repeated pattern — the `e4` format byte was read three wrong ways before the TCW-chain (`*(0x8C2DAD4C)+idx*0x20, +0x0C`); the `sprite_id→assembly` re-key was read three wrong ways before the live cell (`read16(cell+4)` where `cell=read32(player+0x154)`); `part_idx→dir-base` by dimension-matching was structurally wrong (the `0x0CE80000` directory is a LIVE working set, not a GFX1-order window); PalMod was mistaken for the MVC2 codec twice.
  **Rule:** read the ACTUAL SH4 read-site / follow the descriptor chain; verify against a ground-truth oracle (community rip `PLxx.png`, live VRAM `effects-capture/efx_*.png` of `0x0CED0000`) before trusting.

- **Trap: the object-pool anchor offsets are +0x80 off.**
  **Why:** the empirical scan keyed on the OWNER value and treated that field's address (`+0x80`) as the record base; the disassembly gives offsets from the absolute base `0x8C26AA54 + slot*0x1D0`.
  **Rule:** re-verify `readObjects()` against the absolute base with one live ptrdump BEFORE shipping the rewrite. (`0x8C26AC24` in work.asm is node #1, NOT the array start.) There is NO numeric z field — draw order = active linked-list position; infer layer from `category@+0x3` + sprite_id range.

- **Trap: reading `facing` from `0x110` (a copy) or palette from `0x52D` (a dup).**
  **Why:** offset drift between the reader and the ground-truth disassembly caused the original facing/palette bugs.
  **Rule:** authoritative `xflip = 0x1D2`; live tint = `0x25` (pl_palid_match). A symbol-importer / drift-guard parsing marvelous2 `#symbol` lines would prevent recurrence.

- **Trap: mislabeling anotak's frame-data columns.**
  **Why:** disasm proves `attack+0x08=KDDuration, +0x09=JuggleX, +0x0A` never read as juggle (anotak's "JuggleY" is derived/mislabeled); cell `+0x0c` is the real effect-spawner trigger (anotak calls it "SoundFX", labels `+0x01` "EffectTrigger").
  **Rule:** treat disasm offsets as authoritative; anotak column names as hints. Attack record = 0x1C bytes, anim cell = 0x14 bytes (confirmed by next-record boundary).

- **Trap: reading effect/HUD constants from the wrong routine.**
  **Why/facts:** life-bar main fill = `currentHP/maxHP` where maxHP is PER-CHARACTER (u16 via ptr at element+0x6A) — the existing `_pointHealth/144` is WRONG on both counts (max isn't 144, and /144 is only right for the SUPER meter, `meter_fill/144.0`). Red layer = `red_health/32.0`. Bar color is PER-TEAM-SLOT (C1 pink→yellow, C2 green→yellow, C3 cyan→yellow), NOT a health gradient — the "white bar" bug is dropping the per-vertex col + ShadInstr modulate. HUD is a self-contained `bank0f` object pool (`loc_8C0F0160`), NOT the character slot-table.
  **Rule:** use the confirmed constants; don't fabricate a HUD draw address (no life/gauge symbol exists in work.asm — it's the bank0f pool).

- **Trap: don't fabricate `loc_8c...` addresses.**
  **Why:** honest negative results (no HUD symbol in work.asm; combo `0x8C289670` not read anywhere in the disasm) were later resolved correctly by finding the real routine.
  **Rule:** an honest "not located" beats a fabricated address. Tag every claim CONFIRMED (verified vs code/disasm/disc bytes) vs INFERRED (with the named verification step).

- **Trap: reading decoded parts from VRAM.**
  **Why:** by then they're twiddled/format-encoded per-format, packed at upload granularity, and VRAM churns every frame.
  **Rule:** the `0x0CE80000` (DM00 Poly, in main RAM not VRAM) copy-out is the canonical format-clean per-part source at load. The sprite codec is flag-bit LZSS over 16-bit LE words (decoder `loc_8c03552a`), NOT bespoke 4bpp RLE.

- **Trap: reconcile-conflicts with marvelous2 asserted over rather than verified.**
  **Why:** they call `0x8C289621` "Frameskip Counter", we call it match_sub_state; `STG_ID 0x8C26A95C` vs our stage_id `0x8C289638`.
  **Rule:** verify the live one, don't assert. Also fix stale ledger conflicts (e.g. win count = `char+0x540` is already shippable, superseding the "win-stars need a new wire field" claim).

- **Trap: the "offline decode is impossible" and "assembly-driven is infeasible" conclusions.**
  **Why:** ASSEMBLY-DRIVEN-DESIGN's central blocker ("~86% of parts need a live scratch buffer, can't build the atlas offline") was DISPROVEN — offline LZSS + de-twiddle closed byte-exact and the full roster was baked offline from disc. The "offline LZSS is a dead end" claim was a decoder-polarity bug (re_kb/08).
  **Rule:** treat those docs' feasibility conclusions as HISTORICAL; offline faithful decode is byte-exact.

---

## 7. Deploy

- **Trap: raw `scp` to production, or assuming git matches prod.**
  **Why:** the 2026-04-10 incident — an AI scp'd git's `king.html` over a prod version that had features not in git; the site broke for users. Prod is often AHEAD of git with independent WIP.
  **Rule:** ALWAYS deploy via `deploy/scripts/deploy-web.sh` / `deploy-headless.sh` (they make timestamped backups + print the rollback command); verify md5 before/after. If prod was edited directly, sync prod→git BEFORE any local change. Edit-local → commit → deploy — never edit prod directly. On the prod box: never pull/commit (prod-overwrite risk) — transplant fixed files + build only.

- **Trap: using the wrong / decommissioned prod host.**
  **Why:** topology drifted TWICE (66.55 -> 149.28 -> rise3). Docs keep restating box facts.
  **Rule:** current prod = **rise3 `15.204.141.58`** (DNS nobd.net / play.nobd.net, cut over
  2026-09-01), login `ubuntu@` with key `~/.ssh/ovh_maplecast`. rise3 is ALSO the build box:
  it holds `~/src/maplecast-flycast` + `~/roms/mvc2.gdi`. `66.55.128.93` is DECOMMISSIONED;
  `149.28.44.118` is no longer the nobd origin (still powered on). dev0ps `65.109.77.178`
  still has a checkout + a ROM copy and can build, but runs NO maplecast services — it is the
  frozen forgily rollback standby. Do not restate any of this in another doc: the SSOT is
  forgily-creations `plans/rise3_handover.md` section 0 (copy `~/HANDOVER.md` on rise3).

- **Trap: `deploy-web.sh` is itself partly wrong — wrong host, wholesale, interactive.**
  **Why:** it used to default to the decommissioned 66.55 host (that default is now removed —
  the host argument is required) and it still overwrites wholesale.
  **Rule:** deploy web SURGICALLY — file backup + scp ONE file + in-place `?v=` sed bump + md5 verify. Bump `?v=` on EVERY web module change or stale browser cache silently ships regressions (real regressions came from stale caching). `relay/deploy.sh` overwrites the ENTIRE relay systemd unit — prefer manual scp+install+restart.

- **Trap: scp'ing the headless binary strips +x → 203/EXEC; or installing over a running binary → "Text file busy".**
  **Rule:** `systemctl stop <svc>` BEFORE the binary swap; re-chmod +x; verify md5 (plain scp can also truncate over the hop).

- **Trap: systemd capability stripping blocks writes even as root.**
  **Why:** `CapabilityBoundingSet=CAP_NET_BIND_SERVICE` strips `CAP_DAC_OVERRIDE`; `ProtectSystem=strict` / `ReadWritePaths` gaps block replay/node-id writes.
  **Rule:** `chown -R maplecast:maplecast` the data dirs; add explicit `ReadWritePaths=` drop-ins for `/var/lib/maplecast`. `MAPLECAST_ROM` must be in the RELAY drop-in too (relay computes the ROM SHA-256 for the verify badge).

- **Trap: nginx / infra footguns.**
  **Rule:** nginx loads EVERY file in `sites-enabled` — move `.bak` files OUT (duplicate default server). `client_max_body_size 64M` on `/hub/api/` (`.mcrec` uploads are 7–10 MB → else 413). `tcp_nodelay` is per-socket, NOT a sysctl. Nodes behind nginx TLS MUST set `MAPLECAST_PUBLIC_*_URL` or the hub stores mixed-content-blocked `ws://host:7201` (browsers on https can't reach ws://; native clients have no such restriction). DNS must cut over BEFORE certbot HTTP-01.

- **Trap: Docker headless SIGBUS / OOM.**
  **Why:** `/dev/shm` defaults to 64 MB but flycast wants ~168 MB; hugepage over-reservation OOM-killed flycast during savestate load on the 1 GB plan.
  **Rule:** `--shm-size=256m`; tune `vm.nr_hugepages` down on small plans (256→64). Use native systemd until the Docker SIGBUS is debugged. CPU governor is a NO-OP on Vultr KVM (hypervisor-controlled) — skip that tuning line.

- **Trap: committing ROM-derived assets.**
  **Why:** ROMs/savestates/sprite rips/atlases/decoded pixels/`marvelous2`/`MVC2 Dev Files` are copyrighted; committing one is a DMCA event that pollutes git history permanently.
  **Rule:** all ROM-derived outputs go to `/dev/shm` or `/tmp` ONLY, gitignored, NEVER committed — scp out-of-band. `git status` + eyeball before any `git add -A`. Never commit savestates/VMU/NVRAM or symlinks into `savestates/`/`roms/` (bakes an absolute host path into history). Only probe code, `tools/*.py`, and docs are committable.

- **Trap: the known-good ROM hash / provenance.**
  **Rule:** MVC2 US v1.001 SHA-256 = `396548fe53f9b3641896398be563795ff190f9b0d7cc61c331901bc68f4e5392`. marvelous2 is disassembled COPYRIGHTED code — read for facts, reimplement clean-room, never vendor/commit; contribute back addresses/semantics ONLY, never MapleCast prod internals (topology/ports/relay/hub/SurrealDB).

---

## Cross-references

- **[INDEX.md](INDEX.md)** — the map of all ~56 source docs with status (CURRENT/SUPERSEDED/HISTORICAL/REFERENCE).
- **[CURRENT-STATE.md](CURRENT-STATE.md)** — what is live/active right now (lockstep client, Phase 2a, render_frame default, prod topology).
- **`docs/RENDER-STATE.md`** — the authoritative render authority-ranking + gate catalog + dead-campaign list. Start render sessions here.
- **`tools/re_kb/`** — the canonical curated RE knowledge graph (SurrealDB); authoritative over any prose doc for addresses/formulas.
