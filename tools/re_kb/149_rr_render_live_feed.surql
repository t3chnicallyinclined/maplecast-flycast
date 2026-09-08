-- 149_rr_render_live_feed.surql -- step 2 part 1: additive live entry points in rr-render, gated on the
-- production replay gates. SUPERGUN ENGINE lane, 2026-09-04 (odd seeds).
-- Doc: supergun/docs/RECEIPT-RUNNER-RUST-PORT.md addendum C
-- Idempotent and replayable; explicit edge ids; additive fields only on findings owned by earlier seeds.
USE NS re DB kb;

UPSERT source:tool_rr_render_live_feed SET kind='tool', strength='code',
  ref='RetroReceipts-agent/rr-render/src/sprites.rs Emitter::push_live_row and src/feed.rs FrameFeed::push_tape',
  note='Strictly additive: two new methods, zero change to any existing path, no existing caller reaches them. push_live_row appends one decoded row and its per-clock side tables, repeating exactly the bookkeeping new() does for them -- bank_slot fed from the row nodes, node_counts given the PRISTINE count so the torn guard verdict stays a pure function of what was pushed, and tape.nodes kept in step with the emitter own nodes copy. push_tape decodes a tape (typically the one-row tape the runner harvest emits for the frame just ticked) and appends its rows, returning indices the caller hands straight to the existing frame(i). The point is that the genuine per-tape setup -- the palette-block resolver, node_counts, world-state init, atlas selection -- is paid ONCE at open rather than sixty times a second, which is why a per-frame open() on a one-row tape was rejected as the wrong shape even where it might be fast enough.';

UPSERT finding:rr_render_live_feed_additive SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='rr-render gained live entry points with the production replay gates unchanged',
  statement='gate_l1.sh MODE=full: 6 of 6 PASS with draws exact at 24574, 17969, 21061, 24481, 2588 and 22167, matching the stated baseline exactly, and the camera closed-form sub-gate unchanged (max abs err 0.000e0 on list6 and hud, 7.629e-6 on list7, value-differing 0 for every P). gate_l3.mjs: 60 of 60 PASS on window 1500 and 60 of 60 on window 2190, the torn-guard window. feed.rs is +28 / -0, entirely additive.',
  caveat='WHAT L1 PROVES AND L3 DOES NOT. gate_l1 rebuilds emit_seq from source so it exercises the crate AS CHANGED, and the sprites.rs edit is inside Emitter, the type L1 drives hardest -- that is the real evidence the existing path is untouched. gate_l3 runs the WASM ARTEFACT, dated 16:53 while the edit is 19:12, so the wasm PREDATES the change and L3 did not exercise it. L3 passing confirms the shipped path is unaffected, which is trivially true while the artefact is stale, and is NOT evidence that a rebuilt wasm passes. A wasm rebuild and an L3 re-run are required before this ships; the wasm build and ENGINE_BUILD belong to the PWA lane and were not touched.';

UPSERT finding:live_feed_inherits_whole_tape_properties SET status='confirmed', confidence='high', date='2026-09-04',
  title='A live FrameFeed session inherits whole-tape properties from its seed tape and cannot derive them incrementally',
  statement='push_live_row deliberately does NOT recompute unknown_slots, palrow_slot / palrow_note, or world_state. Those are whole-tape properties that Emitter::new derives once from the complete tape -- the palette-block resolver inspects the whole tape to decide whether staged palette blocks are in fighter-slot order, and unknown_slots is the complement of the slots any node in the tape ever bound. A live feed has no way to improve on them incrementally without changing existing behaviour. Consequence for how step 2 must be wired: the live session is opened on a REPRESENTATIVE SEED TAPE rather than an empty one, and its palette-slot resolution and unknown-slot set are those of the seed. Recorded now rather than discovered as a rendering artefact later.';

UPSERT finding:shared_checkout_crlf_hazard SET status='confirmed', confidence='high', date='2026-09-04',
  title='sed -i silently converted a shared-checkout source file from CRLF to LF, which would have looked like a whole-file rewrite',
  statement='RetroReceipts-agent is a SHARED checkout and had uncommitted work from another lane before this lane arrived: README.md, src/bin/emit_seq.rs, src/web.rs, tools/gate_l1.sh and part of src/sprites.rs were already modified. An sed -i run against src/sprites.rs rewrote it with LF endings while every other file in the crate is CRLF, so a diff showed all 602 lines changed and the other lane would have received a whole-file rewrite on top of their work. Detected by byte-counting CRLF versus bare LF against untouched files in the same crate, and restored to 628 CRLF / 0 bare LF before anything was staged. Of the resulting sprites.rs diff (+191 / -7) only about 25 added lines are this lane; feed.rs (+28 / -0) is entirely this lane. Nothing was staged or committed.',
  rule='On a shared checkout, prefer a Python rewrite that preserves the file existing line endings over sed -i, and check line endings against a sibling file before staging.';

UPSERT finding:live_render_blocked_on_scope SET
  resolved_note='2026-09-04: RESOLVED -- option (b) was approved and the additive entry points are in (finding:rr_render_live_feed_additive). What remains of step 2 is the presentation path: the local socket carrying FrameRecords, the browser-side change to consume a live stream rather than a tape URL, and the sg_runner path dependencies on rr_render and rr_agent.';

RELATE finding:rr_render_live_feed_additive->cites:p149_tool->source:tool_rr_render_live_feed;
RELATE finding:live_feed_inherits_whole_tape_properties->cites:p149_seed->source:tool_rr_render_live_feed;
RELATE finding:shared_checkout_crlf_hazard->cites:p149_crlf->source:tool_rr_render_live_feed;
