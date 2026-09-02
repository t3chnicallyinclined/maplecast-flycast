-- ============================================================================
-- 90: the executor arc -- and a schema for verification verdicts
--
-- The graph's newest finding was dated 2026-07-18. Everything after it -- the
-- thin SH4 executor reaching a 0-diff whole-tick reproduction, the four bugs
-- that got it there, the object-pool spawn chain -- existed only in handoff
-- docs and auto-memory. Measured before this file: ZERO findings mentioned
-- `executor`, `gen_tick` or `realcore`.
--
-- WHY THIS NEEDS NEW SCHEMA, AND WHY IT IS NOT `routine.verified_byte_exact`
-- --------------------------------------------------------------------------
-- A boolean on the routine would have been TRUE for the pool-spawn chain on
-- 2026-07-21 and FALSE on 2026-07-22 -- same code, different seed. It encodes
-- a claim the evidence does not support.
--
-- "Byte-exact" is a property of (function-set, SEED, tick range), never of a
-- function. So the verdict lives on an EDGE to the RUN that produced it,
-- exactly as `tried_on` puts an outcome on the edge rather than on the
-- approach. That is the same lesson twice, and it is the one this project
-- keeps paying for.
--
-- Every artifact below was verified present before this file was written:
-- five commits (git log), six routine rows, and the 16MB seed / 16MB truth /
-- 21MB transpile on disk.
--
-- Apply: PYTHONIOENCODING=utf-8 python tools/re_kb/apply_seed.py \
--            tools/re_kb/90_executor_verification.surql
-- ============================================================================
USE NS re DB kb;

DEFINE TABLE IF NOT EXISTS oraclerun SCHEMALESS
  COMMENT 'ONE verification run -- the DOMAIN a verdict is true over. Fields: harness (test_tick|verify_leaf|realcore_leaf|tickhash), seed, entry_pc, ticks, truth (artifact), masks (addresses deliberately excluded), diff_bytes, commit, date. An ASMTRACE capture and a realcore --leaf run are the same kind of object: a reproducible evidence-producing run with a scope.';

DEFINE TABLE IF NOT EXISTS verified_by SCHEMALESS
  COMMENT 'routine -> oraclerun. THE VERDICT LIVES HERE, never on the routine. Carries verdict (byte_exact|diverges|not_exercised|benign_drop), scope (leaf|whole_tick_member|region), diff_bytes, date. A verdict is a property of (routine, run): the pool-spawn chain was byte_exact on one seed and diverged on another with identical code.';

-- ---- the run that anchors the whole arc ------------------------------------
UPSERT oraclerun:test_tick_f90_20260720 SET
  harness='test_tick',
  seed='tools/render-replica-poc/_ram_f90.bin (16MB RAM image)',
  entry_pc='0x8C0358BE',
  ticks=1,
  truth='tools/render-replica-poc/_tick_truth.bin (16MB, flycast ground truth)',
  masks='0x8C2D5748, 0x8C32DBAC, 0x8C268250 -- the three TCNT0-derived values, plus the 0x8CFExxxx scratch stack',
  diff_bytes=0,
  commit='4f93dc647',
  date='2026-07-20',
  note='Diff trajectory across the arc: 6,700,000 -> 25,565 -> 2,612 -> 113 -> 52 -> 0 bytes over 16MB. Each step is one of the findings below.';

-- ---- the commits, as code-grade sources ------------------------------------
UPSERT source:commit_4f93dc647 SET kind='code', strength='code',
  ref='4f93dc647 -- EXECUTOR TEST A COMPLETE: byte-exact vs flycast tick (0 bytes)',
  title='Test A: whole-tick byte-exact', date='2026-07-20';
UPSERT source:commit_7989bb1ef SET kind='code', strength='code',
  ref='7989bb1ef -- fix(executor): delay-slot latch for register-indirect branches',
  title='delay-slot latch', date='2026-07-20';
UPSERT source:commit_055f2c8fa SET kind='code', strength='code',
  ref='055f2c8fa -- feat(executor): whole tick transpiles (270/270) + runs',
  title='area-3 write gate / whole tick transpiles', date='2026-07-20';
UPSERT source:commit_56f905ef0 SET kind='code', strength='code',
  ref='56f905ef0 -- fix(executor): SZ-aware fmov (pair/XD bank) -> diff 113->52',
  title='SZ-aware fmov', date='2026-07-20';
UPSERT source:commit_49376e4dc SET kind='code', strength='code',
  ref='49376e4dc -- feat(exec): executor DRIVES the headless server (SH-4 game tick replaced in place)',
  title='executor drives the headless server', date='2026-07-23';

-- ---- 1. the capstone -------------------------------------------------------
UPSERT finding:executor_tick_byte_exact SET
  status='confirmed', date='2026-07-20',
  statement='The transpiled whole game-tick reproduces flycast BYTE-EXACTLY: 0 bytes different over the full 16MB RAM image, seeded from _ram_f90.bin at entry 0x8C0358BE, masking exactly three TCNT0-derived values (0x8C2D5748, 0x8C32DBAC, 0x8C268250) plus the 0x8CFExxxx scratch stack. Diff trajectory 6,700,000 -> 25,565 -> 2,612 -> 113 -> 52 -> 0.',
  note='The masked values are timer-derived and therefore not reproducible by construction -- masking them is a stated scope limit, not a fudge. Everything else matches byte for byte.';
RELATE finding:executor_tick_byte_exact->cites->source:commit_4f93dc647;
RELATE finding:executor_tick_byte_exact->about->routine:loc_8c0358be;

-- ---- 2-4. the three SH-4 semantics bugs that closed the diff ---------------
UPSERT finding:sh4_delay_slot_latch SET
  status='confirmed', date='2026-07-20',
  statement='For SH-4 register-indirect branches (jsr @rN, jmp @rN, braf, bsrf) the TARGET REGISTER MUST BE LATCHED BEFORE the delay-slot instruction executes, because the delay slot can clobber it. Closing this took the whole-tick diff from 2,612 bytes to 113.',
  note='This is general SH-4 semantics, not a MapleCast fact -- it would be wrong in any SH-4 recompiler that reads the target after the delay slot.';
RELATE finding:sh4_delay_slot_latch->cites->source:commit_7989bb1ef;

UPSERT finding:sh4_area3_write_gate SET
  status='confirmed', date='2026-07-20',
  statement='Only SH-4 AREA 3 is main RAM: mc_is_ram(a) = (a & 0x1C000000) == 0x0C000000. Writes outside it (VRAM, TA, MMIO) must be DROPPED, not aliased into the RAM array -- a texture-upload memcpy was corrupting game RAM through that aliasing. Closing this took the diff from 25,565 bytes to 2,612.',
  note='The failure mode is silent: an aliased write lands somewhere plausible and only shows up as a divergence thousands of bytes away.';
RELATE finding:sh4_area3_write_gate->cites->source:commit_055f2c8fa;

UPSERT finding:sh4_sz_aware_fmov_xd_bank SET
  status='confirmed', date='2026-07-20',
  statement='Under fschg SZ=1, SH-4 fmov moves 8-byte PAIRS into the xf[] back bank, not single registers. Closing this took the diff from 113 bytes to 52.',
  note='Verified against flycast core/hw/sh4/interpr/sh4_fpu.cpp. The residual 52 bytes were then the XMTRX back-bank loader loc_8c120220, reached only by a static bra with no #data reference -- which is why a boundary scan missed it.';
RELATE finding:sh4_sz_aware_fmov_xd_bank->cites->source:commit_56f905ef0;
RELATE finding:sh4_sz_aware_fmov_xd_bank->about->routine:loc_8c120220;

-- ---- 5. the object-pool spawn chain ---------------------------------------
UPSERT finding:object_pool_spawn_chain SET
  status='confirmed', date='2026-07-22',
  statement='MVC2 pool objects (effects, projectiles) are allocated by loc_8c044f12 -- a generic BUDGETED list-node allocator dispatched by r5 (list id) and r6 (list op), NOT a HUD-specific allocator -- reached via jsr @rN from loc_8c0e3098. The spawn chain is loc_8c044f12 (alloc) -> loc_8c129728 (memset) -> loc_8c044fa2 (list insert-at-head, writes +0x08 prev / +0x0C next) -> loc_8c129560/loc_8c129600 (copy) -> loc_8c034c38 (Obj_LoadAnim: +0x158/9 grp/anim, +0x154 cell-data). Budget global at 0x8C287AE8 (u16, cmp/pl gate -- the allocator returns 0 when <=0).',
  note='CORRECTS the graph\'s prior role for loc_8c044f12 ("TA poly-record ALLOCATOR, called by every HUD sub-builder"), which was the HUD CALLER\'S view and too narrow -- it reads as "this is a HUD thing", which is exactly the framing that stops someone looking here for a projectile-spawn bug. Effect: pool object 0x8C27D734 went 19-byte diff -> 0, and the whole tick f2->f3 went 109 -> 2.';
RELATE finding:object_pool_spawn_chain->cites->routine:loc_8c044f12;
RELATE finding:object_pool_spawn_chain->about->routine:loc_8c044fa2;
RELATE finding:object_pool_spawn_chain->about->routine:loc_8c034c38;

-- ---- the verdicts, scoped to the run that produced them --------------------
RELATE routine:loc_8c0358be->verified_by->oraclerun:test_tick_f90_20260720
  SET verdict='byte_exact', scope='whole_tick_member', diff_bytes=0, date='2026-07-20';
RELATE routine:loc_8c120220->verified_by->oraclerun:test_tick_f90_20260720
  SET verdict='byte_exact', scope='whole_tick_member', diff_bytes=0, date='2026-07-20',
      note='Was the entire 52-byte residual before it was reached at all.';
RELATE routine:loc_8c044f12->verified_by->oraclerun:test_tick_f90_20260720
  SET verdict='byte_exact', scope='whole_tick_member', diff_bytes=0, date='2026-07-20',
      note='SCOPE MATTERS HERE: byte_exact on THIS seed. The same chain diverged on f2->f3 with identical code -- see finding:executor_latent_divergence_class.';

-- ---- the two OPEN problems -- more consequential than any open render row --
UPSERT finding:executor_latent_divergence_class SET
  status='open', date='2026-07-22',
  statement='A transpiled function set proven byte-exact on ONE tick pair is NOT proven on another. The object-pool spawn chain was byte-exact across f90->f91 and diverged on f2->f3; after that fix a SECOND, different pool object still diverged 18 bytes at f6->f7. COROLLARY: "verified byte-exact" is a property of (function-set, seed, tick range) and never of a function.',
  note='This is why 90 introduces oraclerun/verified_by instead of routine.verified_byte_exact. What would close it: a verdict over a SET of seeds broad enough to exercise the spawn/despawn paths, not a single tick pair.';

UPSERT finding:executor_sh4_handback_open SET
  status='open', date='2026-07-23',
  statement='MAPLECAST_EXECUTOR drives mem_b in place of getSh4Executor()->Run() (core/emulator.cpp:952). On match-end the SH-4 RESUMES AT A PRE-HANDOVER STALE PC. This blocks continuous production use of the executor-driven headless server.',
  note='What would close it: snapshot/restore the SH-4 context at the handover boundary, or freeze at match-end. The executor itself is byte-exact -- this is the seam, not the engine.';
RELATE finding:executor_sh4_handback_open->cites->source:commit_49376e4dc;
