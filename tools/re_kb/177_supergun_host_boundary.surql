-- 177_supergun_host_boundary.surql -- the SUPERGUN host boundary for the LOAD path, as ONE set, and
-- the process trap that had a session rediscover it one access violation at a time.
-- 2026-09-05. Paths use forward slashes: a backslash in a single-quoted SurrealQL string is an
-- invalid escape, the request returns HTTP 400, and rekb.sh used to report that as success.
USE NS re DB kb;

UPSERT source:supergun_load_boundary_20260905 SET kind='capture', strength='reproduction',
  ref='sg_runner.exe --pre d3dcap/ttd/runs/vs-01/pre --arc game_50.arc --no-flush --heaps --host-gfx --noop-import 1408db190,1408db108 --ticks 1100 --inputs <scripted two-player team select>. Runs sg_vs..sg_vs9 in C:/Users/trist/AppData/Local/Temp.',
  note='Each successive fault address, in the order the shell hit them: 0x1402704C8 (allocator service locator FUN_140270490), ntdll!RtlEnterCriticalSection trap, 0x140271059 (FUN_140270930 IRender->getDevice()->CreateTexture2D), 0x1402728A8 (FUN_140272700 texture map), 0x140052040 (FUN_140051F70 detwiddle writing to a null *(hostdev+0xDBD10)), 0x140272F19 (FUN_140272EA0 upload). All six were already enumerated in the graph before the run started.';

UPSERT finding:supergun_load_host_boundary SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='The complete host boundary a SUPERGUN shell must satisfy to run the match LOAD path',
  statement='Assembled from finding:load_path_static_global_set, finding:load_path_external_call_set, finding:load_path_new_external_calls_vs_tick and finding:load_path_indirect_dispatch_boundary, then confirmed by running each to its fault. (1) FIVE pointer globals a battle tick never touches: 0x140ACD3A8 host device, 0x142EBD8F0 graphics backend, 0x142E12CF8 and 0x142E12CD0 allocator registries, 0x142EBCE38 allocator descriptor. (2) The 64-entry allocator registry at 0x142E12CD0 plus the two standalone allocator globals 0x142E12CF8 / 0x142E12D20 -- all repointable at the in-image Default heap 0x142E12EF0, whose vtable 0x1408FF408 has real functions at +0x38 alloc, +0x40 alloc-aligned and +0x60 free. (3) The host-graphics band: FUN_140270930 nDraw::Texture::create (calls IRender->getDevice()->CreateTexture2D/CreateShaderResourceView/CreateUnorderedAccessView through *(*(0x142EBD8F0)+0xB8) vtable +0x20/+0x28/+0x30/+0x38/+0x40, all taking the OUT pointer as the 4th argument), FUN_140272700 map, FUN_140272EA0 upload, FUN_140396D00, FUN_140397050, FUN_1402B34A0. (4) The host-device STAGING TRIO at hostdev+0xDBD08/+0xDBD10/+0xDBD18 -- three buffers 0x210000 apart, captured as 0x17FD00A0/0x181E00A0/0x183F00A0 -- which is where FUN_14004BA50 detwiddles a DC-RAM texture through FUN_140051F70; it is NOT the D3D mapped pointer, and a stale or null value there faults at 0x140052040. (5) Exactly two IAT slots beyond the tick set: 0x1408DB190 RtlEnterCriticalSection and 0x1408DB108 RtlLeaveCriticalSection, both genuinely inert on a single-threaded runner.',
  consequence='A shell that satisfies those five items runs character select to G+2 = 2 and enters the load machine. SUPERGUN never uses the host texture objects -- it decodes pages out of DC-RAM -- so a synthetic device that returns a synthetic COM handle, a scratch mapping, and a ret-stubbed upload are consistent with the architecture rather than workarounds.',
  caveat='EVERY item is a change to the simulation until gated. The gate is unchanged and is not "it stopped crashing": the battle-entry blk must equal one the real game produced for the same roster and stage.';

UPSERT finding:trap_crash_driven_enumeration SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='TRAP: letting an access violation enumerate the host boundary, when the graph already had it',
  statement='On 2026-09-05 a SUPERGUN session drove the Versus anchor into the match load and then fixed faults one at a time -- allocator locator, critical section, CreateTexture2D, texture map, detwiddle target, upload -- re-running between each. Every one of those six was already recorded: the 24-slot IAT set, the 689-global reference set with its five host pointers, the two extra critical-section slots, and the host-graphics band BY NAME. The session queried the graph only after the user objected, and the query returned the complete set in one answer.',
  consequence='Crash-driven work looks like progress because each step really does advance, which is exactly why it is hard to notice. It also produces per-fault patches instead of one boundary-shaped change, and it spends the run budget rediscovering settled facts. RULE, now in supergun/docs/RE-METHOD.md: when a shell faults on a host dependency, the next action is a KB query for the closure that contains it -- not another patch.',
  next='Same rule applies to the remaining fault at 0x140611EC3 (inside the load machine, RVA 0x611EC3, READ 0xE0D70000): query the closure and the read set before patching anything.';

UPSERT finding:rekb_request_level_failure_is_silent SET status='inferred', confidence='high', date='2026-09-05',
  title='TOOLING: rekb.sh reported success for a seed file that applied ZERO statements',
  statement='175_select_state_machine.surql contained a Windows path inside a single-quoted string. SurrealDB rejected the whole request with HTTP 400 "Parse error: Invalid escape sequence" and returned a JSON OBJECT rather than the usual array of per-statement results. rekb.sh only greps the response for "status":"ERR", which a request-level object does not contain, so it printed the error body and exited 0. Nine findings appeared to apply and none existed.',
  consequence='rekb.sh now fails when the response is not a JSON array, printing the body (self-tested: a deliberately malformed request exits 1). Seed files use forward slashes in ref/note strings. This is the second silent-success mode found in that script; the first is finding:trap_kb_seed_failures.';

RELATE finding:supergun_load_host_boundary->cites->source:supergun_load_boundary_20260905 SET id='e177_bound_run';
RELATE finding:supergun_load_host_boundary->cites->finding:load_path_static_global_set SET id='e177_bound_globals';
RELATE finding:supergun_load_host_boundary->cites->finding:load_path_external_call_set SET id='e177_bound_iat';
RELATE finding:supergun_load_host_boundary->cites->finding:load_path_indirect_dispatch_boundary SET id='e177_bound_band';
RELATE finding:supergun_load_host_boundary->cites->finding:default_allocator_is_in_image SET id='e177_bound_alloc';
RELATE finding:trap_crash_driven_enumeration->cites->source:supergun_load_boundary_20260905 SET id='e177_trap_run';
RELATE finding:trap_crash_driven_enumeration->cites->finding:supergun_load_host_boundary SET id='e177_trap_bound';

UPDATE finding:supergun_load_host_boundary SET status='confirmed';
UPDATE finding:trap_crash_driven_enumeration SET status='confirmed';
