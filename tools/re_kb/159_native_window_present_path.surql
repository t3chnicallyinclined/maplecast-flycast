-- 159_native_window_present_path.surql -- SUPERGUN presents its own frames. The window, the low-latency
-- present path, the pacing, and the first input-to-photon numbers this project has ever had.
-- SUPERGUN ENGINE lane, 2026-09-04 (odd seeds). Doc: supergun/docs/RECEIPT-RUNNER-WINDOW.md
-- Idempotent and replayable; explicit edge ids.
USE NS re DB kb;

UPSERT source:tool_sg_native_window SET kind='tool', strength='code',
  ref='supergun/supersoaker/runner/sg_runner/src/gfx.rs and src/win.rs (the Win32, GDI and D3D11/DXGI seam) and src/play.rs',
  note='sg_runner --window opens a window and presents from the simulating thread. sg_runner --window-test N exercises the present path alone, with no image, no anchor and no tick. Every Win32 and COM declaration lives in win.rs by hand, no crate, because the seam has to be re-implemented for Bazzite.';

UPSERT source:meas_window_present SET kind='reproduction', strength='reproduction',
  ref='sg_runner --window-test 4 and sg_runner --play --play-frames 300 --window on the stage 9 anchor, 240 Hz display',
  note='Present-path self-test and a full play session, both on the target machine.';

UPSERT finding:native_window_present_path SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='SUPERGUN presents its own frames: no socket, no bridge, no browser between the simulation and the glass',
  statement='The runner now opens a window and presents from the SAME thread that simulates, so nothing sits between the pad and the present. The present path is the low-latency configuration rather than the default: DXGI_SWAP_EFFECT_FLIP_DISCARD, a swapchain created with DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT, SetMaximumFrameLatency(1), and a wait on the frame-latency waitable object BEFORE the pad is sampled -- so the frame begins as late as it can and still makes the scan-out. Measured on a 300-frame play session: 60.2 fps, 0 of 300 frames over the 16.667 ms budget, tick p50 0.059 ms, tape p50 0.206 ms, INPUT to PRESENT RETURNED p50 0.539 p99 2.160 max 3.892 ms, and INPUT to SCAN-OUT p50 2.402 p99 6.796 max 11.821 ms over 297 samples.',
  method='INPUT to SCAN-OUT is not a measurement of our own code. Each frame stores the QPC taken immediately after the pad read, indexed by its present ordinal; DXGI GetFrameStatistics then reports PresentCount and SyncQPCTime, the timestamp of the vertical blank a given present actually went out on, and the two are joined by that ordinal. It is the first number in this project taken past the tick.',
  invariant='Gate 1 re-run after the window landed: 300/300, 60/60 and 20/20 blk dumps byte-exact against the p-code oracle, clock equal on every tick. The window is invisible to the simulation, which is the only thing that mattered.';

UPSERT finding:window_pacing_spin_to_deadline SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='Pacing must NOT be locked to the display, and spin-to-deadline holds 60 Hz far tighter than Sleep',
  statement='A waitable swapchain paces the caller to the DISPLAY. On the target machine the display is 240 Hz, so vsync-locking the loop would have run the SIMULATION at 240 Hz and made the match itself wrong. The runner therefore reads the refresh rate (GetDeviceCaps VREFRESH) and only uses the waitable object plus sync interval 1 when the display is within 58 to 62 Hz; otherwise it paces the simulation itself against a fixed epoch and presents unsynced. Measured with spin-to-deadline pacing on the 240 Hz display: frame-to-frame p50 16.667, p99 16.786, max 17.039 ms over 241 presented frames -- within about 0.12 ms at p99 of the 60 Hz period, against the roughly 0.6 ms overshoot the netcode lane measured for Sleep with no primitive on this machine doing better. This is P7b, and it costs a burned core: the last stretch to the deadline is spun. For a fighting game that is the correct trade and it is stated rather than hidden.',
  design='Pacing happens at the TOP of the loop, before the pad is read, so the input is taken at the last possible moment and lands in the frame presented next. The Sleep it replaces was at the BOTTOM, which read the pad and then made it wait.';

UPSERT finding:window_says_what_it_is_showing SET status='confirmed', confidence='high', date='2026-09-04',
  title='The window names the battle it resumed, because a picture with no context reads as a load screen',
  statement='A player opened the live path and asked how to select characters. There is no character select by design -- the runner resumes a captured battle at an anchor, mid-match, with a fixed stage and roster -- and nothing on screen said so, so it read as a load screen that had not finished. The window now shows the stage and both teams, and states in words that the battle was captured mid-match and resumed. Those three fields are read from the SEED TAPE via rr_render tape Tape decode (stage_id, p1_team, p2_team), the same source the browser session info uses, and NOT from live memory: no roster offset is CONFIRMED in this graph, and a confidently wrong banner is worse than a plain one. The three guard claims are drawn beside it and stay separate: input reached the sim, a DIRECTION was pressed, and STEERING PROVEN. They matter more with a picture in front of the player, not less, because a character moving to its idle bob looks exactly like one moving because of you.';

UPSERT finding:dxgi_alpha_mode_ignore_is_3 SET status='confirmed', confidence='high', date='2026-09-04',
  title='DXGI_ALPHA_MODE_IGNORE is 3; writing 1 asks for premultiplied alpha and an HWND swapchain refuses',
  statement='CreateSwapChainForHwnd returned DXGI_ERROR_INVALID_CALL (0x887A0001) with an otherwise valid DXGI_SWAP_CHAIN_DESC1. The cause was the AlphaMode constant: the enum is UNSPECIFIED 0, PREMULTIPLIED 1, STRAIGHT 2, IGNORE 3, so the value 1 requests a premultiplied-alpha swapchain, which an HWND cannot have. The runner now passes UNSPECIFIED, the only value documented for CreateSwapChainForHwnd. Recorded because the failure is indistinguishable by inspection from a wrong vtable slot, which is the other thing a hand-declared COM surface gets wrong, and it cost a debugging pass to tell them apart. The related trap in the same struct: DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT is 0x40, while 0x800 is ALLOW_TEARING -- using 0x800 yields a swapchain with no waitable object and a null handle to wait on.';

UPSERT finding:native_window_draw_list_next SET status='inferred', confidence='high', date='2026-09-04',
  title='The native window can consume the emitter draw list in process, with no FrameRecord in the path at all',
  statement='INFERRED from reading rr-render: FrameFeed frame(i) calls em.emit_row(i), which returns a frame carrying draws, verts and idxs, and the emitter also owns textures and blobs. Serialisation to a FrameRecord happens AFTER that, purely so the bytes can cross a process or a socket. A renderer inside sg_runner does not need to cross anything, so the native path can take the draw list directly and skip serialisation, the 4.4 MB first record and the 277 KB per frame entirely. What remains to build is the translation of that draw list to D3D11 plus an HLSL port of replay/sprite.wgsl, which is bounded: 246 lines, two vertex entry points (vs_world, vs_flat) and five fragment entry points, with exactly five valid pairings enumerated in replay.mjs FS_ENTRY. CONFIRMING this needs the renderer built and gated per-part against the browser output on the same tape.',
  gate='The acceptance gate is a numeric per-frame diff of the native output against the WebGPU tape path on the SAME tape, offline and with no game running -- not a look at the screen.';

RELATE finding:native_window_present_path->cites:p159_tool->source:tool_sg_native_window;
RELATE finding:native_window_present_path->cites:p159_meas->source:meas_window_present;
RELATE finding:window_pacing_spin_to_deadline->cites:p159_pace->source:meas_window_present;
RELATE finding:window_pacing_spin_to_deadline->cites:p159_pacecode->source:tool_sg_native_window;
RELATE finding:window_says_what_it_is_showing->cites:p159_banner->source:tool_sg_native_window;
RELATE finding:dxgi_alpha_mode_ignore_is_3->cites:p159_alpha->source:tool_sg_native_window;
RELATE finding:native_window_draw_list_next->cites:p159_feed->source:tool_rr_render_live_feed;
