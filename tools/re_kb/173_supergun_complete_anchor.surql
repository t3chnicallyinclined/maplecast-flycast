-- 173_supergun_complete_anchor.surql -- what a COMPLETE (pre-match) SUPERGUN anchor must contain.
-- Derived 2026-09-05 from the graph + a static read of mvc_dump.bin. No new live probe.
USE NS re DB kb;

UPSERT steam_routine:FUN_140607b50 SET addr='0x140607b50', name='FUN_140607b50',
  role='ARENA CARVE. Runs on mode entry (MvC2 selector). Reads arena = *(*(0x140ACD3A0)) = *(0x140AC6D40) = game_state+0, then lays every region out at a FIXED offset inside the single 256 MiB arena and stores the derived pointers.';

UPSERT finding:arena_carve_map SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='The whole SUPERGUN region set is ONE 256 MiB arena carved at fixed offsets by FUN_140607b50',
  statement='Disassembled straight out of mvc_dump.bin at file offset 0x607b50 (VA 0x140607B50). rbx = arena = *(0x140AC6D40). 0x140607B68 ctx = arena + 0x8000000, stored to 0x142EF0AB0; memset(ctx, 0, 0x1F8600). 0x140607B94 ctx[0x00] = arena (the resident AFS arc image). 0x140607B82/B9E ctx[0x08] = arena + 0x8400000 = DC-RAM, memset 0xCD over 0x2000000 (32 MiB). 0x140607BAE/BB5 ctx[0x18] = arena + 0x8200000. 0x140607BB9/BC7 game_state+0x208 (staging) = arena + 0xA400000. 0x140607BCE call 0x14003AD20 (shell RNG); AND EAX,0x3F; LEA ECX,[RAX+0xC0]; SHL ECX,0x14; ADD RCX,RBX; 0x140607BEB ctx[0x10] = blk = arena + ((rng AND 0x3F) + 0xC0) * 0x100000.',
  consequence='CAPTURE THE ARENA, NOT SIX REGIONS. blk, blk2, ctx, dcram, the staging buffer and the arc image are all carves of one contiguous 256 MiB allocation whose base is one pointer read. A six-region anchor can silently omit a carve; an arena snapshot cannot. Only the exe image and the C++ heap lie outside it.';

UPSERT finding:blk_movement_is_the_arena_rng SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='blk moving within a boot is the arena RNG draw, NOT a general-purpose allocator',
  statement='finding:live_blk_is_allocator_assigned recorded blk at 0x13FC1000, 0x147C1000, 0x14CC1000 in one session and concluded an allocator was handing out successive blocks. Against the carve at 0x140607BD3..0x140607BEB and the live arena base ctx[0] = 0x07FC1000 (finding:live_pointer_set) the three values are arena + 0xC000000 / 0xC800000 / 0xCD00000 = arena + 192 / 200 / 205 MiB, i.e. exactly ((rng AND 0x3F) + 0xC0) MiB for draws 0, 8 and 13. Cross-check on the same session: arena + 0x8400000 = 0x103C1000 = the measured DC-RAM base, and arena + 0x8000000 = 0x0FFC1000 = the measured ctx. Every observation lands on a MiB boundary in [0xC0, 0xFF].',
  supersedes='The MECHANISM clause of finding:live_blk_is_allocator_assigned. Its OPERATIONAL rule (never cache blk, always deref 0x142edf560) stands and is reinforced: FUN_140607b50 re-runs per mode entry and re-draws.',
  consequence='blk is ALWAYS inside the arena, in the top 64 MiB window. An arena capture subsumes blk by construction and no separate blk region or blk-specific delta is needed if the arena is restored at its own base.';

UPSERT finding:default_allocator_is_in_image SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='The 64-entry allocator registry mostly points at a STATIC in-image Default allocator -- 0x142E12EF0',
  statement='Read out of mvc_dump.bin at VA 0x142E12CD0 (64 qwords, all non-null): 51 of 64 entries = 0x142E12EF0, which is INSIDE the exe image and therefore already carried by exe_image.bin. That object is a MT Framework MtObject: +0x00 vtable = 0x1408FF408, +0x20 = 0x01, +0x21 = ASCII Default, and the qword immediately after the 14 vtable slots reads 4D 74 4F 62 6A 65 63 74 = MtObject, the [vtable][class name] layout predicted in supergun/docs/WORKSTREAM-SUPERGUN-CORE.md section 2.3. Vtable slot 7 (+0x38) = 0x14003C8C0 -- exactly the slot the fault at 0x1402704C8 (JMP [R10+0x38]) calls. The 13 remaining entries resolve to only 10 distinct heap objects in TWO bands: 0x6797EB0, 0x67B8E90, 0x67B9520 (entries 8, 10, 14), 0x67BD140, 0x67BD7D0, 0x67BDE60, 0x67BE4F0 (band span 0x6797EB0..0x67BE4F0 = 0x26640 = 157 KB) and 0x74BEC0, 0x74C550 (entries 11, 12, 16..19, 21) -- a SECOND band near 7.6 MB that the earlier nine-object survey missed.',
  consequence='Two consequences for the anchor. (1) A heap capture must cover BOTH bands, not just the 128 KB one. (2) There is a cheap fallback short of capturing any heap: point every registry slot whose target is not carried at 0x142E12EF0, the in-image Default allocator, whose allocate virtual is fully present in exe_image.bin. That converts the null-vtable fault at 0x1402704C8 into a real allocation from the runner own heap.',
  caveat='INFERRED for the fallback: rewriting a slot changes WHICH heap an object comes from, so any allocated address that later lands in blk perturbs blk and would break a byte gate against a live reference. Sound as an unblock-and-observe step, NOT as the determinism story.';

UPSERT finding:load_path_readset_unknown SET status='open', confidence='high', date='2026-09-05',
  title='GAP: no read-set inventory exists for the LOAD path -- only for the tick',
  statement='finding:determinism_hidden_state_inventory covers exactly one thing: every byte outside blk that the TICK FUN_140118950 reads before it writes, classified by first access from a 313,754-record trace (scratchpad/hidden_state.py, supergun/docs/DETERMINISM-CONTRACT.md:30). Seeds 105/106 (ttd/frame readset) have the same one-battle-frame scope. NOTHING in the graph or the docs enumerates what the select-to-battle transition reads: FUN_14062BB30 commit, FUN_14060F450 order commit, the VS intro FUN_140628570, the load machine 0x14060F330, FUN_14060C370 cases 1/4/5/6/9/10 and FUN_14060DCF0. Likewise finding:determinism_external_calls bounds the TICK at 24 external calls into the UCRT only; the LOAD path external-call set is unmeasured.',
  next='Same harness, new scope: d3dcap/replay/emu_gate.py frame over a CHARACTER-SELECT image with G+2 written to 2, tracing until G+4 reaches 2 (in round), then hidden_state.py over that trace. No debugger, no live probe -- the load machine runs inside the same frame function the harness already emulates whole.';

UPSERT finding:anchor_phase_validity SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='Anchor regions have a PHASE of validity -- a select-phase capture makes battle-phase derived tables stale by construction',
  statement='The staleness the SUPERGUN runner saw (ctx texture slot table reporting 32x32 for 8x8 tiles) is the general case, not a capture bug. Measured instances already in the graph: blk+0x6D04 stage is written at LOAD and reads 6 through all of character select (finding:live_stage_written_at_load); all six costume bytes normalise to 1 at load; POL banks in DC-RAM are rebased IN PLACE at load and differ from the pristine AFS entry by 0.9 to 2.5 KB (finding:bank_load_rebase_measured); rec+0x8 model patches are applied at NODE SPAWN and persist (finding:dcram_spawn_time_model_patches); G+0xAD is only meaningful when FUN_14060C370 case 10 last ran, so the results bank must be placed demand-driven (finding:results_bank_loader_rule); the 123 texture-slot flags at ctx+0x1E2624+n*0x18 are created by the load path and all-0xFFFF hangs the slot search (finding:determinism_hidden_state_inventory); ctx+0x107864..0x108EF4 page-table records are run-dependent host residue excluded from every byte gate (finding:runner_ctx_pagetable_nondeterminism).',
  consequence='A character-select anchor is CORRECT and its derived tables are correct FOR CHARACTER SELECT. The battle-phase versions must be produced by RUNNING the load machine, not by grafting a battle-phase region onto a select-phase anchor. Mixing phases across regions is the error class to design against.';

UPSERT finding:spectator_bound_on_anchor SET status='inferred', confidence='high', date='2026-09-05',
  title='GGPO spectate bounds the anchor UPWARD, not downward -- inputs-only presupposes a fully booted peer',
  statement='ggpo_start_spectating transfers inputs only and the registered rollback set is exactly one region {blk, 0x33B18} with no pointers and no allocator state (finding:ggpo_saves_exactly_blk). A spectator derives character select, stage and the match from the input stream ALONE -- but only because it is a peer that already booted: the arena and its carves exist (FUN_140607b50), the arc image is resident at ctx[0], the C++ heap and subsystem registry are constructed, the exe statics hold post-boot values, the same build and the same on-disk game_50.arc are present, and blk has been initialised by mode entry. GGPO assumes all of that identical by construction; it never transfers it.',
  consequence='The correct reading of the observation that the game starts from character select: character select is the earliest point at which the SIM state is complete, portable and input-drivable, and everything after it is reachable by ticking. It is NOT a point at which less host state is needed -- the host construction happened earlier still, at boot. So a complete anchor = a full snapshot of a peer sitting at that point, and the input stream then suffices exactly as it does for a spectator.';

RELATE finding:arena_carve_map -> cites -> source:bin_mvc_dump SET id='e_arena_carve_bin';
RELATE finding:arena_carve_map -> cites -> steam_routine:FUN_140607b50 SET id='e_arena_carve_rtn';
RELATE finding:blk_movement_is_the_arena_rng -> cites -> source:bin_mvc_dump SET id='e_blkrng_bin';
RELATE finding:blk_movement_is_the_arena_rng -> cites -> source:meas_live_session_20260905 SET id='e_blkrng_live';
RELATE finding:default_allocator_is_in_image -> cites -> source:bin_mvc_dump SET id='e_defalloc_bin';
RELATE finding:load_path_readset_unknown -> cites -> finding:determinism_hidden_state_inventory SET id='e_loadgap_tick';
RELATE finding:anchor_phase_validity -> cites -> source:meas_live_session_20260905 SET id='e_phase_live';
RELATE finding:spectator_bound_on_anchor -> cites -> source:bin_mvc_dump SET id='e_spec_bin';

-- promote AFTER the cites edges exist (schema asserts confirmed requires a citation)
UPDATE finding:arena_carve_map SET status='confirmed';
UPDATE finding:blk_movement_is_the_arena_rng SET status='confirmed';
UPDATE finding:default_allocator_is_in_image SET status='confirmed';
UPDATE finding:anchor_phase_validity SET status='confirmed';
UPDATE finding:spectator_bound_on_anchor SET status='confirmed';
