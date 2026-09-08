-- 208_gate_discipline_method.surql -- the METHOD lessons of 2026-09-07, as opposed to the facts.
-- Seeds 203/205/207 hold what we learned ABOUT the game. This holds what we learned about HOW WE WERE
-- WRONG, so the next agent does not repeat a day of it. Forward slashes only in ref/note; no
-- apostrophes.
USE NS re DB kb;

UPSERT source:supergun_session_20260907 SET kind='session', strength='reproduction',
  ref='SUPERGUN main 196f519 and the runs under C:/Users/trist/AppData/Local/Temp/rr_play/ -- gate_main, gate_main2, refA, refB, fixA, fixB, g9a, g9b, landed. Each pair is same build, same flags, same 4436-tick script rec4/words.txt.',
  note='Four published claims were WRONG on this day and had to be corrected in the repo: the anchor capture called a WinDbg TTD session when it is 333 lines of ReadProcessMemory; the anchor called 1.7 GB when the loaded set is 108 MB; round time attributed to a config field that never reaches it; and a 304-frame pixel window blamed on host residue when it was our own loading card. Every one was caught by someone else reading, or by a control that had never been run.';

UPSERT finding:gate_must_agree_with_itself SET confidence='high', date='2026-09-07',
  contribution_candidate=true,
  title='A gate is not a gate until it has been run twice on the same input and agreed with itself',
  statement='The SUPERGUN pixel gate is the primary correctness instrument and every render-path change of 2026-09-07 was signed off with it. The control -- same build, same flags, same script, twice -- had never been run in that form. When finally run it FAILED: 304 of 4435 frames differed. Three sub-failures were exposed with it. (1) The reference hash.txt could not be compared against anything because its runner.log did not record its own argv, so a disagreeing run could not be distinguished from a differently invoked one. (2) The G9 flag --blk-hash called fs::File::create on a path inside --out before --out was created, failed, left BLK_HASH None, and the gate then ran to completion asserting NOTHING -- a pass with no evidence. (3) No work counter was quoted beside any verdict.',
  consequence='Before a comparison is trusted ONCE, run the control. Every gate run logs its argv and its binary. A gate flag that cannot create its output is FATAL, never a printed line. Quote the work counter -- flips performed, hashes written, ticks compared -- beside the verdict. This is the sibling of finding:gate_reporting_zero_work_is_not_a_pass: that rule catches a gate that did nothing, this one catches a gate that did something unrepeatable.',
  ref='supergun/docs/PIXEL-GATE-NONDETERMINISM.md; supergun main 52bffdc argv logging, 6b7175d the fix, 7c5fa88 the blk-hash fix',
  status='open';

UPSERT finding:the_shell_is_in_the_picture SET confidence='high', date='2026-09-07',
  contribution_candidate=true,
  title='Suspect our own overlays before the game, the host or memory residue',
  statement='A presented frame is the game output PLUS everything the shell draws over it: the loading card (which covers the whole window, card_set bleed=true), the match-end card, the diagnostics overlay and the menu. The 304-frame pixel-gate window was the loading card rasterising a wall clock from the stats lane. The investigation instead spent a day on 0x1400488B5, a host texture-slot read three ticks into the window, which is structurally incapable of reaching the draw list -- its value is written nowhere, returned nowhere and compared to nothing but zero. The tick correlation was real and NOT causal: G+2 becoming 2 fires both the texture teardown and the shell loading-card predicate. One cause, two effects, and the more interesting effect was followed.',
  consequence='Account for our own surfaces FIRST and prefer the boring explanation. Absolute corollary: nothing time-, network-, allocation- or iteration-order-derived may reach a rasterised string; keep timestamps in the log where nothing hashes them. A determinism gate must also refuse a network lane rather than trust the operator to pass --stats-off. Do NOT fix this class by excluding the window -- the SUPERGUN window spanned the battle entry chain, which holds the VS card, the portrait pages and the whole texture teardown, and is the last place a gate should be blind.',
  ref='supergun/docs/PIXEL-GATE-NONDETERMINISM.md; src/stats.rs:73; src/menu.rs:293; src/main.rs:3390,4188,4194',
  status='open';

UPSERT finding:ghidra_xref_list_is_not_closed SET confidence='high', date='2026-09-07',
  contribution_candidate=true,
  title='get_xrefs_to misses code in regions auto-analysis never defined, so an empty xref list is not proof',
  statement='The only site that writes 3 to G+0x4d, making training time infinite, is 0x14060BE7B, and it does NOT appear in the Ghidra xref list for the G pointer global DAT_142EDF580 because its containing body sits in an undefined region. The exhaustive site list was recovered only by scanning every 4-byte RIP-relative displacement in mvc_dump.bin for the target address and disassembling a window at each hit with capstone.',
  consequence='When an xref list looks suspiciously empty, or a writer is unaccounted for, SCAN the image rather than concluding the writer does not exist. Never report an absence of xrefs as an absence of code. Related and separately earned the same day: a p-code op-coverage claim derived by grepping an emitter source was wrong twice; enumerate the emitter branches and diff against real data instead.',
  ref='re_kb seed 205; GhidraMCP on mvc_dump.bin',
  status='open';

UPSERT finding:host_side_field_is_inert_in_the_shell SET confidence='high', date='2026-09-07',
  contribution_candidate=true,
  title='A setting whose only readers are host functions changes nothing in the runner, and a menu row that writes it is a dead switch',
  statement='The runner never calls the Collection front-end, only the sim. Two cases in one day. (1) config+0x8d0, the training-mode input delay: all six xrefs are host front-end functions -- reads at 0x14009FD6E, 0x1400A01FD, 0x140039760 and writes at 0x14009F318, 0x14003794B, 0x14009F6BE -- and NONE is in the sim range, so the field is structurally inert here. That is why a player capturing their own anchor cannot carry hidden input lag into SUPERGUN, and why an input-delay row must be implemented in our own input path rather than by writing that byte. (2) G+0x4d, round time: the sim DOES read it, every clock tick at 0x1406270F0, so the value applies immediately -- but the two HUD digit objects are built once per round entry by FUN_140653EE0, and in infinite mode it builds ONE object whose update handler is a bare RET, so the screen cannot show a live change.',
  consequence='Before believing a setting works, check that its xrefs land in the sim range (FUN_140607B50 carve, FUN_140607D60 tick), not the host band. Then check separately whether anything REDRAWS on a change: a field the sim honours and the HUD ignores looks identical to a field nothing honours. Both failure modes present as the player pressing a button and nothing happening.',
  ref='re_kb seeds 205 and 207; supergun/docs/BETA-READINESS.md section 1',
  status='open';

UPSERT finding:two_sources_of_truth_for_one_count_will_drift SET confidence='high', date='2026-09-07',
  contribution_candidate=true,
  title='The menu row COUNT and the menu row BUILDER lived in different files and drifted; and a count check does not catch a layout collision',
  statement='menu.rs items_on bounds the cursor and main.rs build_shell_menu builds the rows. Adding a refresh-rate row to the DISPLAY page without changing the other file left the last row drawn but unreachable, and its LEFT/RIGHT handler dead -- a shipped feature that could not be used. A drift check comparing built count against declared count was added, and it did NOT catch the next bug: when a tenth PAUSE row arrived, both numbers agreed at 10 while the row was painted UNDER the readout strip, invisible and still selectable, so selecting what looked like the readout quit the game. The rows laid out top-down at a fixed pitch and the strip laid out bottom-up, with nothing checking they did not meet. The TRAINING page had been overflowing the card entirely since before that.',
  consequence='Derive a count from the thing it counts, never maintain it beside it. And note the invariant that actually matters is not count against count but EVERY SELECTABLE ROW IS VISIBLE -- a different assertion needing a different check. Both bugs were found by a human on the first live run of pages that had never been watched, which is the argument for live-verifying every page before restructuring any of it.',
  ref='supergun main 5bebad3 and merge b9ff4f3; supergun/docs/BETA-READINESS.md section 1',
  status='open';

UPSERT finding:never_quote_latency_from_a_scripted_run SET confidence='high', date='2026-09-07',
  contribution_candidate=true,
  title='A scripted gate run locks the pad out of the simulation but not out of the instrument',
  statement='One run of a control pair logged 63 chords and 227 real key presses; its twin logged zero. A human was at the keyboard. It could not reach the simulation -- main.rs:4149 assigns inputs.0 = SCRIPT[SCRIPT_POS] unconditionally and main.rs:4162 forces raw_both = 0 whenever a script is loaded -- and frames 1019..4434 being byte-identical between the two runs proves it independently. But it does contaminate that run INPUT STATS, PRESS->SCANOUT, LATENCY BY MODE and FRAME COST lines. The two runs also differed in machine load, 24.8 percent against 20.6 percent of 32 cores.',
  consequence='Never quote a latency number out of a scripted run; gate and time in separate runs. Ideally the gate should not start the input worker at all. Read MACHINE LOAD before believing any timing, and remember --pixel-hash itself stalls the frame and inflates FRAME COST to about 4.2 ms.',
  ref='re_kb seed 207 finding:pixel_gate_live_input_pollutes_the_log; supergun/docs/TEST-CHANGELOG.md',
  status='open';

UPSERT finding:captured_host_object_graph_cannot_be_restored SET confidence='high', date='2026-09-07',
  contribution_candidate=true,
  title='--restore-regions is not merely unnecessary, it turns a benign tick into a fatal one',
  statement='Running the pixel gate with --restore-regions on the vs-01 anchor restored 893 private regions, FAILED on 416, and then crashed at tick 718 with an access violation reading 0x10 at 0x1402703B3, inside the nDraw texture teardown FUN_1402702C0. Without the restore, the host texture slot is a lazily committed zero page, the NULL test at 0x1400488BF is taken and the release is correctly skipped. With a partial restore the slot holds a live-looking pointer FROM THE CAPTURE SESSION, the release runs, and it dereferences a D3D11 child object at this+0x78 whose device does not exist in this process.',
  consequence='A captured host-object graph can never be meaningfully restored at ANY base, because the objects behind the pointers are driver-owned and do not exist here. So a fixed host allocation is not available as a way to make a gate reproducible -- not merely empirically, but in principle. Treat --restore-regions as diagnostic only; the playing path stands in for it with --heaps and --host-gfx.',
  ref='re_kb seed 207; supergun/docs/PIXEL-GATE-NONDETERMINISM.md',
  status='open';

RELATE finding:gate_must_agree_with_itself->cites->source:supergun_session_20260907;
RELATE finding:the_shell_is_in_the_picture->cites->source:supergun_session_20260907;
RELATE finding:ghidra_xref_list_is_not_closed->cites->source:supergun_session_20260907;
RELATE finding:host_side_field_is_inert_in_the_shell->cites->source:supergun_session_20260907;
RELATE finding:two_sources_of_truth_for_one_count_will_drift->cites->source:supergun_session_20260907;
RELATE finding:never_quote_latency_from_a_scripted_run->cites->source:supergun_session_20260907;
RELATE finding:captured_host_object_graph_cannot_be_restored->cites->source:supergun_session_20260907;

-- The schema refuses status='confirmed' unless a cites->source edge of strength
-- reproduction or code already exists, so the status is promoted AFTER the RELATEs above.
-- Written this way so the file applies cleanly in ONE pass; the first version needed two and
-- reported ERR on every finding, which looks exactly like a broken seed.
UPDATE finding:gate_must_agree_with_itself SET status='confirmed';
UPDATE finding:the_shell_is_in_the_picture SET status='confirmed';
UPDATE finding:ghidra_xref_list_is_not_closed SET status='confirmed';
UPDATE finding:host_side_field_is_inert_in_the_shell SET status='confirmed';
UPDATE finding:two_sources_of_truth_for_one_count_will_drift SET status='confirmed';
UPDATE finding:never_quote_latency_from_a_scripted_run SET status='confirmed';
UPDATE finding:captured_host_object_graph_cannot_be_restored SET status='confirmed';
