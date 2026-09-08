-- 211_cross_host_determinism_and_bankload_unblock.surql
-- From the 2026-09-08 netcode-lane consolidation. AUTHORED, NOT APPLIED.
--
-- Two things, both of which the graph is currently wrong or silent about:
--
-- (a) CROSS-HOST DETERMINISM. The graph has NOTHING on it, and DETERMINISM-CONTRACT.md:150-152
--     still reads "CONFIRMED on this CPU (FMA3 present: the live flag is 1)" as its strongest
--     claim. The netcode lane measured it across two machines. This is a bigger result than the
--     library choice and it closes the second-ranked open item in
--     supergun/docs/HANDOVER-2026-09-07-netcode-library.md section 2(b).
--
--     ⚠ IT IS SEEDED WITH ITS SCOPE ATTACHED, AT THE MEASURING LANE'S OWN INSISTENCE. The tape is
--     effectively ONE-SIDED: seat 0 has input on 1926 of 4436 frames, seat 1 on THREE. Every anchor
--     on disk has non-actionable fighters -- px/py never move across 600 ticks on all five, though
--     inputs do reach the fighter structs. So the claim is "identical across hosts over the code
--     paths a one-sided tape reaches", NOT "the simulation is deterministic across hosts". Recording
--     the unscoped version would be exactly the overclaim M3 and M4 exist to prevent, and the lane
--     that produced the measurement asked for the caveat rather than being talked into it.
--
-- (b) A STALE blocked_on. finding:bank_loader_reachable_from_frame_function says it is blocked on
--     "A capture: no existing receipt spans a match-flow transition." That was true when written and
--     is now false: finding:supergun_ko_freeze_menu (confirmed, 2026-09-06) means the shell reaches
--     KO, win screen and rematch, so the sequence is producible in-runner. The hazard is the one
--     open finding that can still move the 277,288 B save set and therefore W1's published hash
--     coverage, so a stale blocker on it is expensive.
--     M2 applies: the UPDATE below touches ONE field and quotes what it replaces, so a later
--     refinement is not silently overwritten.
--
-- Paths use forward slashes only: a backslash in a single-quoted SurrealQL string is an invalid
-- escape, the request returns HTTP 400, and rekb.sh used to report that as success.

USE NS re DB kb;

-- ---------------------------------------------------------------------------
-- (a) cross-host determinism, scoped
-- ---------------------------------------------------------------------------

UPSERT source:xhost_20260908_two_machine SET kind='dynamic', strength='reproduction',
  ref='600-tick run on receipt-20260903-stage9-anchor (battle frame), two-seat tape, run on two machines and byte-compared; plus a two-machine netplay session over real UDP',
  note='Host A: Ryzen 9 5900XT, Zen 3, Windows 11. Host B: Ryzen 7 8745HS, Zen 4, Proton Wine 11.0, Bazzite 44. 608 artifacts byte-identical: 600 per-tick blk dumps plus blk_out, ctx_out (4 MB), dcram_out (32 MB), gs_out, ggpo_out and the exe mutable pages. The live netplay session agreed on CHECKSUM AT CONFIRMED FRAME 240 = 25ada41e1b48db8eaa6b0e2f444eb7d5, which is also the value the loopback run produced. Measured by the netcode lane, branch netcode, handover supergun/docs/HANDOVER-2026-09-08-netcode.md (95d7afa).';

UPSERT finding:cross_host_determinism_one_sided_tape SET status='confirmed', confidence='high', date='2026-09-08',
  statement='CROSS-HOST DETERMINISM, MEASURED AND SCOPED. The frame function produced BYTE-IDENTICAL state on two different machines, two different microarchitectures and two different operating systems: Ryzen 9 5900XT / Zen 3 / Windows 11 versus Ryzen 7 8745HS / Zen 4 / Proton Wine 11.0 / Bazzite 44. 600 ticks from the stage-9 battle anchor, 608 artifacts compared, all identical -- 600 per-tick blk dumps plus blk_out, ctx_out, dcram_out, gs_out, ggpo_out and the exe mutable pages. A separate two-machine netplay session over real UDP agreed on the confirmed-frame-240 checksum 25ada41e1b48db8eaa6b0e2f444eb7d5, matching loopback. This closes the second-ranked open item in the netcode library handover, and it supersedes the scope of DETERMINISM-CONTRACT.md:150-152, which proved only that the FMA3 and SSE2 paths agree ON THIS CPU. Zen 4 has AVX-512 and Zen 3 does not, so the DAT_142eefbd8 dispatch in section 3 of that contract was exercised on two ISA surfaces rather than one. ⚠ THE COVERAGE LIMIT IS PART OF THIS CLAIM, NOT A FOOTNOTE TO IT: the run is close to BLIND on the one floating-point result the determinism contract says can move blk. Section 3 records that the only CRT result reaching blk is the exact SQRTSS, where a +1 ulp nudge moves 9 blk bytes -- the walker screen x/y of the drawn fighters, via the eye-target normalisation. On all five anchors the fighters are non-actionable (px/py never move across 600 ticks), so the camera is static, so that sqrtf path is computing a CONSTANT every frame. The two hosts therefore agreed over a code path on which the value in question never changes. The 608 artifacts are real and the gate is sound; what it is sound ABOUT is narrower than "the simulation is deterministic across hosts". This is the difference between incomplete coverage and coverage that misses the target.',
  note='⚠ SCOPE, AND IT IS LOAD-BEARING -- DO NOT QUOTE THIS AS "the simulation is deterministic across hosts". The tape is effectively ONE-SIDED: seat 0 carries input on 1926 of 4436 frames, seat 1 on THREE. Every anchor currently on disk has non-actionable fighters: px/py never move across 600 ticks on any of the five, although inputs do reach the fighter structs. So what is established is that the two hosts agree OVER THE CODE PATHS A ONE-SIDED TAPE REACHES. Combat interaction -- hit/block resolution, pushbox, throws, hitstun, super freeze, DHC -- is NOT covered, and that is exactly the region where a 1-ulp SQRTSS difference would show. Sharpened by the measuring lane after first seeding: the fighters being non-actionable is not merely a weak arm, it holds the CAMERA STATIC, so the sqrtf whose result reaches blk computes a constant on every one of the 600 ticks. Inputs demonstrably DO reach the fighter structs -- they diverge from idle at tick 1 at +0x669/+0x66d/+0x67d/+0x681/+0x68e in slots 0/2/4 -- so the tape is not inert; it is specifically inert on the one value the contract says can move blk. A genuinely two-sided round (--start-match with --p2-keys) is the open item that would widen this. Also still UNKNOWN: an INTEL host. Both AMD parts share a vendor, a compiler-visible feature baseline and a memory model; Intel is the untested axis and is the one a beta population will actually contain.';

RELATE finding:cross_host_determinism_one_sided_tape->cites:e211_xhost_repro->source:xhost_20260908_two_machine;

-- ---------------------------------------------------------------------------
-- (b) the stale blocker on the bank-load hazard -- one field, quoted
-- ---------------------------------------------------------------------------

UPDATE finding:bank_loader_reachable_from_frame_function SET
  blocked_on = 'NO LONGER BLOCKED ON A CAPTURE (corrected 2026-09-08). The previous value read: "A capture: no existing receipt spans a match-flow transition. Everything above is derivable and derived without one." That was true when written. finding:supergun_ko_freeze_menu (confirmed 2026-09-06) means our own shell now reaches the KO, the win screen and a same-team rematch, so a transition-spanning tick sequence is PRODUCIBLE IN-RUNNER from an anchor we already hold, with no live Steam capture. What remains is to run it: gate_n1c.py at depth 60 with --rb-verify all on a window that CONTAINS a KO, and the comparison must ALSO cover the 17 bank-pointer globals 0x142EDF588..0x142EDF620, which the current gate does not compare. Both arms required or it is vacuous (M4): a depth-0 control that must PASS, and a deliberately narrowed arm that must FAIL. Two closures need no capture at all -- resolve statically which outer dispatch state reaches each of the 16 call sites (is any reachable with G+3 == 4 other than the tick that leaves battle?), and trap entry to FUN_14060C370 counting hits per tick over the existing 300-tick receipts, where the expected answer is 0 and a 0 retroactively scopes finding:runner_rollback_dcram_not_needed.';
