-- 147_harvest_off_frame_thread.surql -- step 3: the GATE 2 harvest moved off the frame thread, gated on
-- byte-identical output. SUPERGUN ENGINE lane, 2026-09-04 (odd seeds).
-- Doc: supergun/docs/RECEIPT-RUNNER-RUST-PORT.md addendum A/B
-- Idempotent and replayable; explicit edge ids; additive fields only on findings owned by earlier seeds.
USE NS re DB kb;

UPSERT source:tool_sg_harvest_ring SET kind='tool', strength='code',
  ref='supergun/supersoaker/runner/sg_runner/src/harvest.rs + the harvest_capture path in src/main.rs',
  note='Two things were on the frame thread and both are removed. FINDING the dirty pages was a 32 MB memcmp against a shadow every tick to discover the <=9 pages that changed; it is now GetWriteWatch, which is O(dirty), for which the arena is reserved with MEM_WRITE_WATCH (OS bookkeeping only, no change to contents or protection). WRITING the files is now a drain thread fed by a bounded 64-slot channel; the frame thread copies blk plus the game_state page plus the exe page plus the written DC-RAM pages and sends. Write-watch reports pages WRITTEN, a superset of pages CHANGED, so the drain thread still memcmp-s the CANDIDATE pages against the shadow -- tens of KB rather than 32 MB -- which is what keeps the emitted .dlt byte-identical.';

UPSERT finding:harvest_ring_off_frame_thread SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='The harvest is off the frame thread, with byte-identical output and 3.4x lower frame-thread cost',
  statement='Measured on the frame thread itself, 300 ticks, same anchor and binary, timing ONLY the harvest own on-frame-thread cost. INLINE (32 MB compare plus writes): p50 5.038, p99 7.422, max 7.843 ms, 1,544.8 ms total. RING (GetWriteWatch plus copy): p50 1.460, p99 2.202, max 2.944 ms, 374.0 ms total. That is p50 3.4 times better, max 2.7 times better, and 1,171 ms of frame-thread time returned over 300 ticks. The residual 1.46 ms is NOT the compare: it is the copy plus backpressure from the disk, because with --dump-every 1 the drain writes a 211 KB blk file every tick and the frame thread waits when the 64-slot channel fills. The ring absorbs 64 frames of jitter and sustained disk throughput is the floor, so how aggressively to record is now a recording choice rather than a property of the simulation. --harvest-mode inline keeps the old path for A/B measurement.',
  gate='Gates re-run AFTER the step, not just at the end. Gate 1 with the arena now MEM_WRITE_WATCH: 300 of 300 blk dumps byte-identical, gs_out / exe_dat_out / ggpo_out EQUAL. Ring output versus inline output: 300 of 300 dcram .dlt files byte-identical, 903 of 903 gs / exe / blk per-tick dumps byte-identical, same 952 pages and 3,899,392 B total. Gate 2 with the ring harvest: PASS with P 14, A 1281, F 1 and zero unexplained, identical to the inline run.';

UPSERT finding:live_render_blocked_on_scope SET status='open', confidence='high', date='2026-09-04',
  title='Live rendering (step 2): every piece exists, none of them inside supergun, and FrameFeed has no append',
  statement='Investigated, not built. rr_render::feed::FrameFeed is open(tape_bytes, AssetPack, EmitOpts) then frame(i) -> Vec<u8>, emitting a binary FrameRecord v2 (RRFR) that replay.mjs and resources.mjs already consume, with textures, constant buffers and pipeline states sent once on first use -- exactly the per-frame streaming primitive step 2 needs, and already what the player wasm path uses. rr_agent::harvest with runner::RunnerView is a MemSource built to read runner memory and produce tape rows. The browser WebGPU player turns FrameRecords into pixels. THE BLOCKER IS SCOPE, NOT DIFFICULTY: FrameFeed::open takes a WHOLE tape and builds an emitter over tape.frames, with no append, so live streaming needs either (a) a one-row tape rebuilt per frame with open() called per frame -- the asset pack is passed by reference so only the tiny envelope is re-parsed -- or (b) a small push_row / frame_live addition to rr-render, which lives in RetroReceipts-agent, a tree this lane was told to leave alone.',
  next_step='Measure option (a) first because it touches nothing outside supergun, and only request option (b) if a per-frame open() cannot hold 60 Hz. Either way sg_runner gains path dependencies on rr_render and rr_agent, which is the point of the port and is not a modification to those crates. Not started: the socket/present path and the browser-side change to consume a live FrameRecord stream rather than a tape URL.';

UPSERT finding:rust_runner_port_identical_state SET
  step3_note='2026-09-04: the harvest was then moved off the frame thread (finding:harvest_ring_off_frame_thread) and Gates 1 and 2 were re-run and still pass, so the arena MEM_WRITE_WATCH flag and the ring changed no state.';

RELATE finding:harvest_ring_off_frame_thread->cites:p147_tool->source:tool_sg_harvest_ring;
RELATE finding:harvest_ring_off_frame_thread->cites:p147_port->source:tool_sg_runner_rust;
RELATE finding:live_render_blocked_on_scope->cites:p147_open->source:tool_sg_runner_rust;
