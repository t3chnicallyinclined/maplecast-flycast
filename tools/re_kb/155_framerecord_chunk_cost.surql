-- 155_framerecord_chunk_cost.surql -- what the 289 KB per frame on the live wire actually is, measured
-- rather than reasoned about. SUPERGUN ENGINE lane, 2026-09-04 (odd seeds).
-- Doc: supergun/docs/RECEIPT-RUNNER-PLAY.md
-- Idempotent and replayable; explicit edge ids.
USE NS re DB kb;

UPSERT source:meas_framerecord_sizes SET kind='reproduction', strength='reproduction',
  ref='sg_view.py probe with a per-record size histogram, 120 frames, --chunk 6, against sg_runner --play --serve',
  note='Every record relayed by the live stream was measured individually rather than averaged, so that first-use cost and steady-state cost could be told apart.';

UPSERT finding:framerecord_chunk_boundary_cost SET status='confirmed', confidence='high', date='2026-09-04',
  title='The live wire cost is geometry re-sent per frame, NOT first-use tables re-sent per chunk',
  statement='Measured over 120 frames at chunk 6: first record 4,378,363 B; then min 271,436, p50 276,912. Split by position, chunk-BOUNDARY records average 277,212 B and non-boundary records average 278,099 B -- the boundary records are 0.3 percent SMALLER, not larger. The first-use tables are therefore NOT re-sent per chunk. The reason is the shape of the implementation: the runner opens ONE FrameFeed on the seed tape at startup and push_tape appends each chunk into that same open session, so tex_sent, blobs_sent and the state and constant-buffer id maps persist for the life of the session and every first-use table is emitted exactly once, in record 0. That one record is 4.4 MB and is 16x a steady-state frame; the steady-state 277 KB is per-frame GEOMETRY, dominated by the static deck being re-emitted every frame.',
  correction='This entry exists to correct a plausible and widely reasonable hypothesis -- that a fresh feed per chunk re-sends the first-use tables, so every Nth record spikes. That is what the code WOULD do if it opened a feed per chunk, and it is not what this code does. The measurement was cheap and the hypothesis was not testable by reading the wire format alone.',
  open_items='The 277 KB per frame of geometry is not optimised and does not need to be for a loopback viewer. If it ever matters, the target is the static deck, not the tables.';

RELATE finding:framerecord_chunk_boundary_cost->cites:p155_meas->source:meas_framerecord_sizes;
RELATE finding:framerecord_chunk_boundary_cost->cites:p155_feed->source:tool_rr_render_live_feed;
RELATE finding:framerecord_chunk_boundary_cost->cites:p155_stream->source:tool_sg_live_stream;
