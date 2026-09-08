-- 151_live_tape_in_ram.surql -- the play loop builds the tape in RAM on the frame thread; recording is
-- always on. SUPERGUN ENGINE lane, 2026-09-04 (odd seeds).
-- Doc: supergun/docs/RECEIPT-RUNNER-PLAY.md addendum B
-- Idempotent and replayable; explicit edge ids; additive fields only on findings owned by earlier seeds.
USE NS re DB kb;

UPSERT source:tool_sg_live_tape SET kind='tool', strength='code',
  ref='supergun/supersoaker/runner/sg_runner/src/live_tape.rs and src/play.rs',
  note='Play mode ported to Rust so it can call the harvest library directly. live_tape.rs builds the tape row ON the frame thread in RAM and writes ONE tape at the end. GATE 2 INVARIANT HELD -- there is NO second implementation of game memory to tape row: sg_runner takes a path dependency on rr_agent and calls the same read_gs_row / harvest_objs / harvest_anodes / read_palrows / record_frame / build_gamestate_record the live agent and rr-tape use, through the same MemSource seam. Only the memory source differs and here it is simply this process; a stray pointer is bounds-checked against the two mapped windows and returns None exactly as a failed ReadProcessMemory does for the agent. The raw per-tick dump path is untouched behind --harvest-dump because it is the oracle comparison.';

UPSERT finding:play_tape_in_ram SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='Recording is always on: the play loop builds the tape in RAM and never touches disk per frame',
  statement='The per-tick raw dumps are a GATE tool -- blk and the dirty DC-RAM pages written out so a run can be compared byte-for-byte against the C++ and p-code oracle -- and they had simply stayed switched on for play. The arithmetic: play mode was copying 211 KB of blk plus dirty pages EVERY FRAME, when the thing actually wanted is the tape, measured at 369 bytes per frame (221,164 B for 600 frames), so a three-minute match is a couple of megabytes. Measured per-frame work in the play loop, 600 frames, same anchor, nobody at the controls: the C++ with raw dumps inline was p50 10.357, p99 34.099, max 58.054 ms and lost frames; the Rust build with the tape in RAM is p50 0.856, p99 1.568, max 3.024 ms with ZERO of 600 frames over the 16.667 ms budget. That is p99 22 times better and the worst frame 19 times better. The tape build itself is p50 0.161, p99 0.355, max 0.471 ms; the tick is p50 0.094, max 0.187 ms; sustained 61.1 fps. The residual 1.46 ms of copy-plus-disk-backpressure identified earlier is gone because both of its causes are gone.',
  consequence='Recording is no longer a mode that must be switched off to get clean timing. Every session records, at 60 Hz, by default.',
  gate='Gate 1 re-run after the change: 300 of 300 blk dumps byte-identical with gs_out, exe_dat_out and ggpo_out EQUAL. The in-RAM tape is well formed: emit_seq renders it at 60 frames, 35,404 draws, 351 distinct textures.';

UPSERT finding:play_guard_still_load_bearing SET status='confirmed', confidence='high', date='2026-09-04',
  title='The no-input guard survived the Rust port and still fires',
  statement='The dry run of the ported play loop printed NO INPUT WAS EVER PRESSED -- nothing was proven about a human in the loop, which is correct because nobody was at the controls. FIRST INPUT, FIRST DIRECTION and STEERING PROVEN remain three separate claims gated on PAD_DIRS (0xF0) rather than on any input. This matters MORE once there is a picture on screen, not less: the idle-animation bob looks exactly like a character responding to the player, which is the same failure shape as reading a px slide under an assist pad as locomotion.';

UPSERT finding:harvest_ring_off_frame_thread SET
  superseded_for_play_note='2026-09-04: for PLAY this is superseded by finding:play_tape_in_ram -- the play loop no longer uses the raw-dump ring at all, because the tape row is built in RAM and the raw dumps are only a gate tool. The ring remains the right shape for the GATE path (--harvest-dump), where raw memory must reach disk.';

RELATE finding:play_tape_in_ram->cites:p151_tool->source:tool_sg_live_tape;
RELATE finding:play_tape_in_ram->cites:p151_port->source:tool_sg_runner_rust;
RELATE finding:play_guard_still_load_bearing->cites:p151_guard->source:tool_sg_live_tape;
