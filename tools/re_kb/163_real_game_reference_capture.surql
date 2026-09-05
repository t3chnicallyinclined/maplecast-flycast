-- 163_real_game_reference_capture.surql -- the FIRST reference from the real game for the
-- tape-RECONSTRUCTION pipeline, and the repeatable recipe that produces it. 2026-09-05.
-- Doc: supergun/docs/RECEIPT-RUNNER-LIVE-DEBUG.md
-- Idempotent and replayable; explicit edge ids.
USE NS re DB kb;

-- ─────────────────────────────────────────────────────────────────────────────
-- SOURCES
-- ─────────────────────────────────────────────────────────────────────────────

UPSERT source:tool_d3dcap_collect SET kind='tool', strength='code',
  ref='mvc-live-skins-quarters/d3dcap/collect.ps1, launch_suspended.ps1, inject.ps1, dllmain.cpp',
  note='Path B capture. collect.ps1 does build -> launch SUSPENDED -> inject into a quiet process -> resume -> wait while a human plays -> detect the first COMPLETE gameplay frame -> analyse -> exit. A frame counts as COMPLETE only when the disjointness gate passes, the vertex snapshot decodes against the authoritative layout, AND every input layout / VS / PS that frame references was captured at creation time.';

UPSERT source:cap_real_game_20260905 SET kind='reproduction', strength='reproduction',
  ref='%TEMP%/rrcap, 1.4 GB, captured 2026-09-05 from a live training match via collect.ps1 -Seconds 5',
  note='REPRODUCIBLE by the recipe in finding:d3dcap_reference_recipe. ROM-derived: lives in TEMP, never in a repo.';

-- ─────────────────────────────────────────────────────────────────────────────
-- THE RECIPE -- this is the part that must survive
-- ─────────────────────────────────────────────────────────────────────────────

UPSERT finding:d3dcap_reference_recipe SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='How to capture a REAL-GAME reference: state + draws + pixels, frame-aligned',
  statement='powershell -NoProfile -ExecutionPolicy Bypass -File mvc-live-skins-quarters/d3dcap/collect.ps1 -Seconds N. -Seconds N records N*60 GAME frames -- the honest unit, since the capture records game frames at 60 fps however slowly the game is actually running while it records. Steam must already be running (SteamAppId satisfies the DRM on a direct launch) and the GAME must be CLOSED, because collect.ps1 launches it itself. Output lands in %TEMP%/rrcap.',
  why_suspended_launch='⚠ INJECTING INTO A RUNNING GAME IS NOT SUFFICIENT FOR A REFERENCE. The game creates its D3D11 device very early in startup, and shader / input-layout bytecode can ONLY be captured at CreateVertexShader / CreateInputLayout time. Inject late and the log says: "creation hook never fired -- injected AFTER device creation. Shader/layout coverage will be incomplete". Injecting the instant the process appears instead CRASHES it -- LoadLibraryW via CreateRemoteThread while the loader is initialising deadlocks the process. CREATE_SUSPENDED removes the race: the process exists but has executed no instruction, so it is injected quiet and only then resumed. A good capture prints "captured at creation: inputLayouts=29 VS=29 PS=108".',
  env_trap='⚠ collect.ps1 sets D3DCAP_BURST itself (process scope) and clears it. A USER-SCOPE D3DCAP_MANUAL=1 makes collect.ps1 HANG FOREVER: manual mode arms only on the ARM file at %TEMP%/rrcap/ARM, while collect.ps1 waits for the timer. Clear both user-scope variables before running it. Environment is read by the injected DLL from the TARGET process at init, so the variables must be in the environment the game was LAUNCHED with -- a Steam already running when they were set will not have them.',
  perf='A capture makes the game crawl; that is expected and harmless. The simulation is frame-based and deterministic, so every captured frame is genuine game output regardless of wall-clock speed. The 60 fps to 2 fps collapse noted in dllmain.cpp was the OLD synchronous writer, fixed by handing bytes to a background thread with a bounded queue (a stall shows up as a slow capture, which is visible; an unbounded queue shows up as an OOM crash twenty minutes in, which is not).';

UPSERT finding:cap_20260905_dataset SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='The 2026-09-05 reference dataset: 300 consecutive real frames with state, draws and pixels',
  statement='Captured from a live training match. 631 frames total; the usable burst is frames 3150..3449, 300 CONSECUTIVE. Contents per frame: frame_<n>.ndjson = the draw list the game ACTUALLY issued (~818 KB/frame) carrying per-draw input layout, VS, PS, vertex buffer + stride + offset, index buffer, blend/raster/sampler state, and "ret"/"retmod" = the RETURN ADDRESS of the game function that issued that draw. blk is reconstructable for every burst frame from full anchors at 2825, 3105 and 3150 (3150 is the burst start, so the burst needs no chain from outside itself) plus 576 deltas covering 2826..3449. Real pixels as scene_{2825,2975,3105,3150,3300,3449}_2048x1024_f87.bmp. Shader coverage complete: inputLayouts=29 VS=29 PS=108 captured at creation.',
  alignment_key='Every captured frame is stamped with the game own frame clock blk+0x3CC8 (dllmain.cpp:1654-1666 writes a per-frame sidecar with {frame, clock, size, runs, bytes, full, at, base}). That is the SAME clock an agent tape is keyed on, so a capture and a tape from one session align exactly, frame for frame, with no guessing.',
  significance='This is the FIRST reference from the real game for the tape-RECONSTRUCTION pipeline. Path B validated the CAPTURE pipeline (replaying captured draw calls) at 0.011-0.018 percent, which is a DIFFERENT pipeline. Reconstructing draws from game state had never been compared to real game output at all.',
  caveat='ROM-derived. Lives in %TEMP%/rrcap and must never enter a repo (BYOR).';

UPSERT finding:draw_list_diff_beats_pixel_diff SET status='confirmed', confidence='high', date='2026-09-05',
  title='Diff DRAW LISTS, not pixels -- the per-draw return address attributes a bad quad to the function that issued it',
  statement='A pixel diff answers "what percentage of the screen is wrong", which for a compounding defect is a number that means nothing without the curve. A draw-list diff answers "draw index M, issued from ret 0x1402B7649 in the game module, has vertices X in the capture and Y in our reconstruction, first diverging at frame N". The capture carries "ret"/"retmod" on EVERY draw precisely so a divergent quad can be attributed to the issuing game function rather than guessed at from coloured bands in a screenshot.',
  method='For each burst frame: reconstruct blk from the anchor + delta chain (verify by requiring the clock at blk+0x3CC8 to advance by exactly 1 per frame and G at blk+0x3CB8 to read [2,1,2,3,2] in-round), feed that same blk through the rr-render emitter, and compare its draw list against frame_<n>.ndjson.';

UPSERT finding:supergun_render_corruption_signature SET status='open', confidence='high', date='2026-09-05',
  title='OPEN: SUPERGUN native render corrupts specific quads -- correct texture, blown geometry',
  statement='Observed by Tris in a 3,600-frame sg_runner --play --window session (recording 2026-09-05 09:07, 81 GIF frames). Specific quads are drawn with the CORRECT TEXTURE but geometry stretched into flat coloured bands across the screen. The clearest instance is a HUD name plate reading MAGNETO stretched across the full screen width: right texture, blown vertices. Stage, characters and HUD render correctly most of the time; frame 28 of 81 is a burst where roughly a dozen quads fail at once, and frame 60 shows the same defect mildly as long flat diagonal bands.',
  candidates='(1) The live push_tape session-state divergence (finding:live_feed_session_history_diverges) compounding far past the 8 frames it was measured over. (2) The Gate 2 note that animated-prop vertices are stale in EVERY live tape. (3) Something new. These have NOT been separated.',
  gate_blindspot='The Stage B raster gate cannot see this: it compares the native D3D11 renderer against our own headless-Chrome WebGPU renderer on the SAME tape, so both draw the same wrong quad and the diff reads 0.0000 percent. A human eye beat the instrument. Do not build another gate shaped like that one.',
  next='Diff the reconstruction against cap_20260905_dataset per finding:draw_list_diff_beats_pixel_diff, and separate HARVEST (bad geometry already in the state/tape) from EMITTER (introduced when reading it).';

-- ─────────────────────────────────────────────────────────────────────────────
-- EDGES
-- ─────────────────────────────────────────────────────────────────────────────

RELATE finding:d3dcap_reference_recipe -> cites -> source:tool_d3dcap_collect SET id='e_recipe_tool';
RELATE finding:cap_20260905_dataset -> cites -> source:cap_real_game_20260905 SET id='e_dataset_src';
RELATE finding:cap_20260905_dataset -> cites -> source:tool_d3dcap_collect SET id='e_dataset_tool';
RELATE finding:draw_list_diff_beats_pixel_diff -> cites -> source:cap_real_game_20260905 SET id='e_drawdiff_src';
RELATE finding:supergun_render_corruption_signature -> cites -> source:cap_real_game_20260905 SET id='e_corrupt_src';
