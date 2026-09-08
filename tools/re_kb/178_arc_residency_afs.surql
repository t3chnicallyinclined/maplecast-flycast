-- 178_arc_residency_afs.surql -- the select->battle load blocker was ARC RESIDENCY, not a poke.
-- 2026-09-05. The SUPERGUN runner was making the COMPRESSED game_50.arc resident at ctx[0] instead
-- of the INFLATED AFS; the game's own VS-intro loader read garbage and faulted. Loading the real
-- inflated AFS (890 entries) makes character select -> match load -> the round run from the arc.
USE NS re DB kb;

UPSERT source:supergun_arc_residency_20260905 SET kind='capture', strength='reproduction',
  ref='sg_runner.exe --pre d3dcap/ttd/runs/vs-01/pre --arc <INFLATED AFS> --no-flush --heaps --host-gfx --noop-import 1408db190,1408db108 --inputs <scripted two-player select> --ticks 1400. Inflated AFS produced by d3dcap/replay/rip_texbank.load_afs on the user game_50.arc, m[0x40:] written to a scratchpad .bin.',
  note='Runs sg_afs / sg_afs2 in C:/Users/trist/AppData/Local/Temp. With the RAW arc: log reads "arc AFS at the arena base (65543 entries)" and FUN_140611E90 faults at 0x140611EC3 (tick 423, VS-intro AFS 0x96). With the inflated AFS: log reads "(890 entries)", EXIT=0, phase advances select(G+2=1) -> commit -> VS intro(G+2=2,G+3=2,t450) -> ROUND(G+3=3,t750) -> runs to t1400, clock advancing every tick, roster 0xe/0x17/0x13/0x14/0x16/0x15 intact.';

UPSERT finding:supergun_arc_must_be_inflated_afs SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='SUPERGUN must make the INFLATED AFS (890 entries) resident at ctx[0], not the compressed game_50.arc',
  statement='FUN_14060DCF0(idx, dc_dst) reads an asset straight from ctx[0] = *(0x142ef0ab0): src = ctx[0] + *(u32)(ctx[0]+8+idx*8), size = *(u32)(ctx[0]+0xC+idx*8), memcpy to dcram + (dc_dst-0x0C000000). So ctx[0] MUST be the inflated AFS: "AFS\\0" at +0, u32 count at +4, an 8-byte (offset,size) directory from +8 with offsets relative to ctx[0]. game_50.arc on disk is an ARC v7 / 1-entry container whose payload is zlib-compressed to an IBIS blob; the AFS begins 0x40 into the inflated blob (rip_texbank.load_afs: m=zlib.decompress(arc[doff:doff+csize]); afs=m[0x40:], 112,635,904 B, 890 entries). The runner loaded the RAW 65,003,268-byte container at the arena (load_into(&arc, ARENA, 0)); ctx[0]+4 then reads 65543 (garbage) and every directory offset is wrong. Character select survived because it reads assets from the captured DC-RAM, not ctx[0]; the VS-intro (FUN_14060C370(6) -> block 0x14060c44e) is the first direct arc consumer, so AFS entry 0x96 (a VS/common-UI bank, off 0x517800 size 432634 in the real AFS) decoded from garbage -> wild LZSS pointer -> AV at 0x140611EC3.',
  consequence='Make the inflated AFS resident and the games own loaders run from the arc, no poke and no stub: select -> commit -> VS intro -> round (G+3=3) all execute to completion (sg_afs2, tick 1400, no fault). Interim fix: pass the inflated AFS (m[0x40:]) as --arc (BYOR, scratchpad, gitignored). Proper fix: the runner should inflate game_50.arc itself (ARC v7 -> zlib -> IBIS -> AFS@+0x40) so a single --arc <game_50.arc> works. supersoaker/tools/dcram_build.py already inflates via RT.load_afs; the same belongs in sg_runner.',
  caveat='This unblocks the LOAD. It is not yet gated: Gate 5 (battle-entry blk + DC-RAM byte-match vs a live versus battle-entry anchor) still owns "loaded correctly". And the RENDER is still the runners re-implementation (collect_world/decode_tcw/raster), which garbles battle banks and is a separate concern from the load.';

UPSERT finding:vs_intro_afs_0x96 SET status='resolved', confidence='high', date='2026-09-05',
  title='RESOLVED: the VS-intro AFS-0x96 LZSS fault was the compressed-vs-inflated arc, not a decoder or host-stub bug',
  statement='FUN_14060C370(6) (VS-intro, reached via battle FUN_14060E170 case 2 -> FUN_140628570 -> FUN_1406285F0) dispatches to block 0x14060c44e which runs, in order, FUN_14060DCF0(0x95,0xcc00000); FUN_140611E90 decode; ... FUN_14060DCF0(0x96,0xccf8000); FUN_140611E90(src=dcram+0xcf8000, dst=dcram+0xc60000) at 0x14060c5b8 -- the faulting decode. The LZSS source is exactly the bytes FUN_14060DCF0(0x96) copied from ctx[0]; it is a raw self-terminating stream, no prior relocation, and the --host-gfx ret-stubs are downstream of it. fighter+0x1F8 (the case-6 setter at 0x14060c5f5) never runs on this path and was fully exonerated. Root cause: ctx[0] held the compressed container, so AFS 0x96 was garbage. Fixed by finding:supergun_arc_must_be_inflated_afs.',
  supersedes='The crash-driven attributions (host-graphics band, fighter+0x1F8 null, select never committed) that preceded the per-tick + arc-residency analysis.';

RELATE finding:supergun_arc_must_be_inflated_afs->cites->source:supergun_arc_residency_20260905 SET id='e178_arc_run';
RELATE finding:supergun_arc_must_be_inflated_afs->cites->source:select_decompiles_20260905 SET id='e178_arc_code';
RELATE finding:vs_intro_afs_0x96->cites->source:supergun_arc_residency_20260905 SET id='e178_vs_run';
RELATE finding:vs_intro_afs_0x96->cites->finding:supergun_arc_must_be_inflated_afs SET id='e178_vs_fix';

UPDATE finding:supergun_arc_must_be_inflated_afs SET status='confirmed';
