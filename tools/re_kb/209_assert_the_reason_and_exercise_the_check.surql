-- 209_assert_the_reason_and_exercise_the_check.surql -- two method findings from the wasm lane on
-- 2026-09-07, arriving the same day as seed 208 and sharpening it. Forward slashes only; no
-- apostrophes.
USE NS re DB kb;

UPSERT source:wasm_lane_smoke_gate_20260907 SET kind='session', strength='reproduction',
  ref='SUPERGUN web supershell, wasm-eval. A page shipped that PARSED but threw on load -- a top-level const read by a function running during first paint, giving Cannot access SIM_HZ before initialization, frozen on the owner screen with every gate green. Fix verified BOTH ways: clean on the fix, and the bug reinstated to watch it reproduce the exact console line before restoring.',
  note='The same lane also ran and REJECTED a static scan for referenced-above-declaration, with numbers: 16 legitimate hits on the clean page against 17 with the bug. A function referencing a later const is legal; the distinguishing fact is not lexical but whether the code runs during load. The negative result is recorded in smoke_page.py docstring rather than the tree so nobody rediscovers it.';

UPSERT finding:a_check_that_does_not_exercise_is_not_a_check SET confidence='high', date='2026-09-07',
  contribution_candidate=true,
  title='node --check parses a module and a temporal dead zone is a runtime fault -- the check could not have caught it however carefully it was run',
  statement='The wasm lane shipped a page that parsed cleanly and threw on load, freezing the owner screen, with every gate green. node --check validates syntax; the fault was a top-level const read by a function that runs during first paint. No amount of care with that check would have caught it, because the check never exercised the thing it claimed to cover. This is the SAME defect as SUPERGUN pixel gate having never been run twice on the same input (finding:gate_must_agree_with_itself) -- in both cases a green signal was being read as evidence about behaviour the signal never touched. The fix was a smoke gate that LOADS the page headless and reads the page own error log out of the DOM. It does not check the page is correct, only that it is not dead, which is the failure that costs a session.',
  consequence='For every gate, state in one sentence what it EXERCISES, not what it checks. If the sentence does not include running the thing, the gate covers syntax or shape and must not be cited as evidence about behaviour. Verify a new gate BOTH ways -- clean on the fix and red with the bug reinstated -- before trusting it, the same discipline as running the control before trusting a comparison once. And when a cheaper static check is rejected, record the numbers that rejected it, so the next agent does not spend an afternoon rebuilding it.',
  ref='re_kb 208 finding:gate_must_agree_with_itself; SUPERGUN web supershell smoke_page.py',
  status='open';

UPSERT finding:assert_the_reason_not_the_decision SET confidence='high', date='2026-09-07',
  contribution_candidate=true,
  title='When a feature is held back for a MEASURED reason, assert the reason -- or the flag sits off forever because nobody rechecks',
  statement='Stated by the wasm lane after holding its 24-row extended training page back: 24 rows need 528 px at the minimum pitch against about 390 available, so it cannot fit. Rather than simply clearing the flag, it wrote an INVERTED assertion -- every page at the caps this build declares fits, AND the extended page does NOT. Make the page scroll and the check fails, and that failure is the message that the flag can now be set. SUPERGUN main carries several flags held off for measured reasons with the numbers written in prose and NOTHING asserting them: AFFINITY (interleaved A/B on the recorded round, off 1.24/1.28 p99 against on 1.25/1.41, and hot-spin the same story), gfx::VB_RING (p50 unchanged and the only arm whose draws p99 sat at 0.80-0.84 ms where every other arm at the same load sat at 0.42-0.47), and SPEC_PROBE. Each is well documented and none is rechecked, so if the machine, the scheduler or a later change makes one of them win, nothing will say so.',
  consequence='A decision recorded as prose decays into folklore; a decision recorded as an assertion tells you when it stops being true. BUT A PERF REASON NEEDS A TOLERANCE OR THE ASSERTION FLAPS, AND A FLAPPING GATE GETS MUTED -- which leaves you worse off than the prose, because it also teaches people to ignore the gates that do not flap. Three shapes, and pick by which one the reason actually is. (1) EXACT arithmetic that does not move between runs -- 24 rows needing 528 px against 390 available -- assert it directly, inverted, so the day it fits the gate fails and that failure is the message. (2) A LARGE MARGIN -- VB_RING at 0.80-0.84 ms against 0.42-0.47 at the same load is a factor of two -- assert the MARGIN with the tolerance you measured (the ring arm stays at least 1.5x the others), never the mere ordering. (3) A DIFFERENCE INSIDE THE NOISE -- AFFINITY off at 1.24/1.28 p99 against on at 1.25/1.41 -- do NOT assert that off still wins, because it will go red on a quiet afternoon and someone will delete it. Assert that the difference stays INSIDE THE BAND, so a real regression and a real win both trip it, and the gate fires when the SITUATION changes rather than when the noise does. Where even that is too expensive, record the numbers, the date and the machine, so a recheck is a comparison rather than a fresh investigation.',
  ref='SUPERGUN main.rs AFFINITY and SPEC_PROBE, gfx.rs VB_RING; the wasm lane TRAINING_EXTENDED gate. Tolerance refinement contributed by the wasm lane, 2026-09-07, after the first version of this finding would have produced a flapping affinity gate.',
  status='open';

RELATE finding:a_check_that_does_not_exercise_is_not_a_check->cites->source:wasm_lane_smoke_gate_20260907;
RELATE finding:assert_the_reason_not_the_decision->cites->source:wasm_lane_smoke_gate_20260907;

-- promoted after the RELATEs, per the schema constraint noted in seed 208
UPDATE finding:a_check_that_does_not_exercise_is_not_a_check SET status='confirmed';
UPDATE finding:assert_the_reason_not_the_decision SET status='confirmed';
