-- 161_native_rasterizer.surql -- Stage B: MvC2's own draw list, rasterized natively in the runner's own
-- process, gated numerically against the WebGPU reference. Plus the withdrawal of a measurement.
-- SUPERGUN ENGINE lane, 2026-09-05 (odd seeds). Doc: supergun/docs/RECEIPT-RUNNER-WINDOW.md
-- Idempotent and replayable; explicit edge ids.
USE NS re DB kb;

UPSERT source:tool_sg_native_rasterizer SET kind='tool', strength='code',
  ref='supergun/supersoaker/runner/sg_runner/src/scene.rs (the rasterizer and the HLSL), src/win.rs (the D3D11 seam), src/play.rs (the live path)',
  note='A port of the four reference files that make up the browser render path: replay.mjs (the draw loop), resources.mjs (resources and the per-draw uniform block), state.mjs (state translation) and sprite.wgsl (the game shaders, disassembled from captured bytecode). sg_runner --raster-gate renders a tape offline with no game and no window; sg_runner --play --window --raster-dump does the same on the LIVE path.';

UPSERT source:meas_raster_gate SET kind='reproduction', strength='reproduction',
  ref='supersoaker/runner/raster_ref.mjs (headless Chrome, real GPU, the same player.html a human reviews gates with) and raster_diff.py (per-pixel, inside the game viewport)',
  note='Both sides are raw BGRA of the SAME 2048x1024 scene render target at scale 1, so the comparison has no resampling in it.';

UPSERT finding:native_rasterizer_pixel_exact SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='The native rasterizer is pixel-exact against the WebGPU reference on identical input',
  statement='Two independent runs against headless Chrome rendering the same tape: 6 frames of the seed tape and 8 frames of a freshly played tape. Both PASS with worst 0.0000 percent of viewport pixels differing beyond a tolerance of 2, and the single largest disagreement anywhere was ONE pixel with a channel delta of 1. Draw coverage matches too: 497 of 499 draws submitted, the 2 skipped being the 28-byte POSITION+NORMAL input layout that cannot feed the shader -- which is exactly the set the browser skips through its own REQUIRED_LOCATIONS check.',
  method='The port was made safer than the original by the shape of the data rather than by care: every state field in a captured draw is ALREADY a raw D3D11 value -- blend.src is a D3D11_BLEND, depth.func a D3D11_COMPARISON_FUNC, raster.cull a D3D11_CULL_MODE, samp.filter a D3D11_FILTER bit layout, tex.fmt a DXGI_FORMAT. The browser has to translate all of it and state.mjs is explicit that a silent mistranslation there is its most expensive class of bug. Going back to D3D11 collapses every one of those tables to identity, and with them that entire class of bug. Only three things genuinely had to be re-derived: the shaders, the uniform block layout and the draw loop.';

UPSERT finding:native_path_needs_no_framerecord SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='In-process rendering takes the draw list directly; the wire format is not on the native path at all',
  statement='FrameFeed frame(i) serialises a FrameRecord AFTER calling em.emit_row(i), purely so the bytes can cross a process or a socket. A renderer in the same process crosses nothing, so it calls emit and renders the returned draws directly -- skipping serialisation, the 4.4 MB first record and the 277 KB per frame the socket path pays. Three strictly additive accessors were added to rr-render for this (emit, emitter, template) plus input_layouts_merged. Measured on the live path: DRAW (emit plus rasterize) p50 4.227, p99 5.580 ms on the frame thread, with the whole loop at 60.1 fps and 1 of 300 frames over the 16.667 ms budget.',
  design='input_layouts_merged exists because getting the merge wrong is SILENT. The world template carries its own input layouts and info_json merges them for the browser; passing only the sprite template layouts made 442 of 499 draws fail to build an input layout, and the renderer quietly drew the characters into an empty stage. One implementation now serves both paths.';

UPSERT finding:dxgi_frame_statistics_unavailable SET status='confirmed', confidence='high', date='2026-09-05',
  title='WITHDRAWN: the INPUT to SCAN-OUT numbers reported on 2026-09-04 cannot be reproduced',
  statement='GetFrameStatistics on this machine returns S_OK with EVERY FIELD ZERO -- PresentCount 0, SyncQPCTime 0 -- for a windowed flip-model swapchain, on every present. The first call after creation returns DXGI_ERROR_FRAME_STATISTICS_DISJOINT (0x887A000B) and every call after it succeeds while reporting nothing. Present itself returns S_OK and never DXGI_STATUS_OCCLUDED, so the window is composited and visible; the statistics are simply not collected. Sync interval 1 behaves identically to sync interval 0. The earlier figures (self-test p50 3.304 ms n=238 of 241; play p50 2.402 ms n=297 of 300) are therefore NOT REPRODUCIBLE and are withdrawn.',
  method='The attribution was established rather than assumed. The exact committed build that produced the original numbers (98c8994) was checked out into a scratch worktree, rebuilt and re-run: it reports no frame statistics either. So the Stage B changes did not break the instrument -- the instrument is conditional on something environmental that held when the number was taken and does not hold now.',
  correction='The instrument now invalidates its reading on failure instead of leaving the previous one standing, counts the failures, and reports the sample yield broken down by cause (no-statistics, count-did-not-advance, out-of-range) beside the percentiles, flagging the total as UNACCOUNTED if the causes do not sum to the frame count. A measurement that keeps its last good reading when the sensor fails is worse than one that reports nothing, because the stale value looks fresh and nothing downstream can tell.',
  open_items='A replacement instrument is needed for input-to-photon. DwmGetCompositionTimingInfo is the standard route for a windowed application and is the candidate; the latency lane owns that budget and should arbitrate rather than have this lane build a second one. Until then the reproducible number is INPUT to PRESENT RETURNED: p50 4.766, p99 6.134 ms with the game drawn, and p50 0.539 ms with the overlay alone.';

UPSERT finding:live_feed_session_history_diverges SET status='open', confidence='high', date='2026-09-05',
  title='The live feed path diverges slightly from a fresh open of the same rows, and it is the emitter, not the renderer',
  statement='Rendering the same 8 rows two ways against the same browser reference: opening the feed FRESH on those rows gives 0.0000 percent differing pixels, while the LIVE path -- a session opened on the 600-row seed tape with the rows pushed in one at a time via push_tape -- differs by up to 0.2072 percent of viewport pixels, with the difference growing monotonically from frame 0 (exact) to frame 7 and channel deltas up to 124. Because the fresh-open arm is exact against the same reference on the same rows, the rasterizer is excluded: the difference is emitter SESSION STATE. A session opened on the seed tape has resolved palrow_slot, unknown_slots and world_state from 600 rows of history, and a fresh open of 8 rows has not.',
  open_items='UNSETTLED: which of the two is more correct. The seeded session has strictly MORE information, and inheriting whole-tape properties from a real recording is the stated reason --seed-tape must never be empty, so the live output is plausibly the better one and the fresh open is the impoverished arm. Settling it needs a reference rendered with the same history, which the browser cannot currently be given. NOT introduced by the native window: the browser live path uses the same push_tape into a seed-opened session, so this property is pre-existing and applies to the WATCHING product too.';

UPSERT finding:veh_killed_on_host_cpp_throw SET status='confirmed', confidence='high', date='2026-09-05',
  title='The runner VEH treated a benign host C++ throw as fatal, which the rasterizer immediately triggered',
  statement='The vectored handler called ExitProcess(3) for every exception code except three debug codes, so the first C++ exception (0xE06D7363) raised anywhere in the process killed the run at tick 0 as soon as the graphics stack was exercised from the play loop. A vectored handler sees exceptions FIRST CHANCE, before the thrower own handler is offered them, and host graphics libraries throw and catch internally as a matter of course. The recompiled game is SH4 code and never throws a C++ exception, so such a code is always a host library and is now passed to its own handler, counted, and logged once. Gate 1 re-run after the change: 300/300, 60/60 and 20/20 blk dumps byte-exact, clock equal on every tick.';

RELATE finding:native_rasterizer_pixel_exact->cites:p161_tool->source:tool_sg_native_rasterizer;
RELATE finding:native_rasterizer_pixel_exact->cites:p161_meas->source:meas_raster_gate;
RELATE finding:native_path_needs_no_framerecord->cites:p161_feed->source:tool_rr_render_live_feed;
RELATE finding:native_path_needs_no_framerecord->cites:p161_code->source:tool_sg_native_rasterizer;
RELATE finding:dxgi_frame_statistics_unavailable->cites:p161_win->source:tool_sg_native_window;
RELATE finding:live_feed_session_history_diverges->cites:p161_gate->source:meas_raster_gate;
RELATE finding:veh_killed_on_host_cpp_throw->cites:p161_veh->source:tool_sg_native_rasterizer;
