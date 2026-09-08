-- 153_live_stream_framerecords.surql -- the presentation path: FrameRecords stream off the runner at
-- 60 Hz, in-process, no file on disk. SUPERGUN ENGINE lane, 2026-09-04 (odd seeds).
-- Doc: supergun/docs/RECEIPT-RUNNER-PLAY.md addendum C
-- Idempotent and replayable; explicit edge ids; additive fields only on findings owned by earlier seeds.
USE NS re DB kb;

UPSERT source:tool_sg_live_stream SET kind='tool', strength='code',
  ref='supergun/supersoaker/runner/sg_runner/src/stream.rs and supersoaker/runner/sg_view.py',
  note='sg_runner --play --serve PORT --seed-tape T --pack D turns each simulated frame into a FrameRecord v2 in-process via rr_render FrameFeed push_tape plus frame(i) and pushes it down a TCP socket. Wire format is deliberately trivial: SGLV u32 ver u32 info_len info_json, then per frame u32 len and len bytes of FrameRecord v2 beginning RRFR. sg_view.py is the dependency-free viewer half with two modes: probe connects and verifies every payload is a well-formed FrameRecord and reports rate and size (the gate, no browser needed), and bridge re-serves the same socket to a browser as WebSocket frames with an RFC6455 handshake by hand.';

UPSERT finding:live_framerecord_stream SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='The runner streams renderer-ready FrameRecords live at 60 Hz with no file on disk in the path',
  statement='Measured over 300 frames with a viewer attached: 300 FrameRecords received, 86,723,259 B (289,078 B per frame), 60.9 fps, ALL 300 payloads well-formed FrameRecord v2 beginning RRFR, first record frame_clock 1717, session info carrying stage 9, world true and the correct teams. The frame the runner just simulated reaches a viewer as renderer-ready draw data in the same process. Full loop with recording AND streaming both on: per-frame work p50 0.965, p99 3.641, max 4.803 ms, 0 of 300 frames over the 16.667 ms budget, 0 chunks dropped; tick p50 0.094, tape p50 0.268, stream p50 1.919 on the chunk boundary only (about 0.32 ms amortised). Gate 1 re-run after the change: 300 of 300 blk dumps byte-identical with gs_out, exe_dat_out and ggpo_out EQUAL.';

UPSERT finding:renderer_off_the_frame_thread SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='With the harvest already off the frame thread, the stream measurement isolated the RENDERER -- and it moved off too',
  statement='First measurement with the stream inline: chunk cost p50 9.330, p99 and max 35.371 ms, and 2 of 300 frames over budget, decomposing into push_tape p50 0.588 ms per chunk and frame() p50 1.020, p99 2.872, max 18.686 ms PER FRAME bunched six-deep onto one frame. Because step 3 had already taken the harvest off the frame thread, that reading was unambiguously the renderer rather than a pre-existing stall -- which was the stated reason for doing step 3 first. The emitter then moved to a worker as well: the worker owns the FrameFeed, which is NOT Send because it holds an Rc, so it is constructed on that thread and does push_tape plus frame() plus the socket write, while the frame thread only builds the small chunk tape and hands the bytes over. Result: stream work on the frame thread fell from p50 9.330 / max 35.371 ms to p50 1.919 / max 3.220 ms and frames over budget from 2 of 300 to 0 of 300.',
  design='The handoff uses try_send, not send: a viewer that cannot keep up must never stall the simulation. Dropped chunks are counted and reported rather than hidden; the measured run dropped none.';

UPSERT finding:play_batch_timing_line_suppressed SET status='confirmed', confidence='high', date='2026-09-04',
  title='A play run printed a zeroed TIMING line and a wrong tick count beside correct numbers',
  statement='A play session ended with TIMING per tick (ms): n=0 min 0.000 p50 0.000 and DONE: 20 ticks after running 600 frames, because the batch summary path is fed by J.ms which the play loop does not use -- it keeps its own timing and prints a fuller summary. The batch line is now suppressed in play mode and the tick count reports the real number (DONE: 600 play frames); batch mode is unchanged and still prints real per-tick timings. Recorded because the failure mode is social rather than technical: a visibly wrong line beside a correct one gets the correct one distrusted, or worse gets quoted later as if it were real.';

UPSERT finding:live_feed_inherits_whole_tape_properties SET
  carried_note='2026-09-04: carried into the streaming path. sg_runner --seed-tape must be a REAL recording of the same stage and roster, never empty, and stream.rs states this at the point of use as well as in the doc.';

RELATE finding:live_framerecord_stream->cites:p153_tool->source:tool_sg_live_stream;
RELATE finding:live_framerecord_stream->cites:p153_feed->source:tool_rr_render_live_feed;
RELATE finding:renderer_off_the_frame_thread->cites:p153_rend->source:tool_sg_live_stream;
RELATE finding:play_batch_timing_line_suppressed->cites:p153_fix->source:tool_sg_live_tape;
