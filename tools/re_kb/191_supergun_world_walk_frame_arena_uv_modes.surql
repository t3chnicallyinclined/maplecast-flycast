-- 191_supergun_world_walk_frame_arena_uv_modes.surql -- runner-side consequences of re_kb 103
-- (nl_ctx_uv_mode), 107 (texture-patch rule) and 189 (select assist names): what the SUPERGUN world
-- walk must accept and apply to draw the game display list faithfully. 2026-09-06. Forward slashes
-- only in ref/note; no apostrophes.
USE NS re DB kb;

UPSERT source:supergun_world_walk_20260906 SET kind='reproduction', strength='reproduction',
  ref='GhidraMCP over mvc_dump.bin: FUN_1408482a0 (world consumer, UV modes at the per-vertex copy), FUN_140848ee0 (entry builder: ctx+0x1f855c copies records into ctx+0x100030 + cursor, bound 0xE0000), FUN_1407ed640 / FUN_14062b620 (assist name row: template copy with shifted UVs, sets ctx+0x1f855c), FUN_140741510 / FUN_1407ed850 (panel spawn). Runner supergun/supersoaker/runner/sg_runner/src/main.rs collect_world. Headless renders of the vs-01 anchor: select at the anchor and select after a scripted confirm (LP at tick 20, shot at tick 83), CPU raster of the game own list.',
  note='Gate for both findings: the headless confirm render shows the three assist rows (Expansion / Variety / Launcher Type for Venom) after the change and blank boxes before it; the scrolling text ring around the globe renders after the UV-mode port. Numeric pixel gate against a live confirm frame: not yet run.';

UPSERT finding:supergun_world_walk_frame_arena_entries SET status='confirmed', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='A world display-list entry polygon pointer may lie in the FRAME ARENA ctx+0x100030..+0xE0000, not only in DC-RAM; a walk that accepts DC-RAM only drops every texture-patched object (assist name rows, HUD parts)',
  statement='FUN_140848ee0 walks an object record list (obj+0x18, records while the first word is negative, stride +0x4C size + 0x50). When ctx+0x1f855c is set it memcpys each record to ctx+0x100030 + ctx+0x1e0080 (cursor, bound 0xE0000) and hands FUN_1408436a0 an entry whose +0x00 points at that COPY; otherwise the entry points at the record in place (DC-RAM). ctx+0x1f855c is set by objects that rewrite their records before drawing: the select assist name rows (FUN_1407ed640: n = DAT_140aacbf0[cid*3+k], FUN_14062b620 copies template model 0x67 JP / 0x6D EN into the node scratch model with every vertex UV shifted by DAT_140aacb80[n] minus [0]; sheets TCW 0x818 JP / 0x817 EN, static TEX 838 pages) and the HUD parts (re_kb 107 list0c_texture_patch_rule). The runner world walk required poly in [dcram, dcram+size) and skipped the rest: at the confirm sub-state the three name rows were the only such entries, so the boxes and TYPE labels drew and the names did not. The walk now accepts [ctx+0x100030, ctx+0x1e0030) as well.',
  consequence='No upload or request is involved for the assist names; the pages are resident and registered at select entry like the boxes. The --dlist dumper line "polygon lists land IN DC-RAM, N do not" is the instrument: N > 0 at a confirm frame is this case.';

UPSERT finding:supergun_world_walk_uv_modes SET status='confirmed', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='The world walk must apply entry+0x10C UV modes exactly as FUN_1408482a0 does: mode 4 adds entry+0x110/+0x114 to every UV; modes 1 and 2 overwrite the first strip quad from +0x110..+0x11C in two corner orders, then clear',
  statement='In FUN_1408482a0, after each vertex is copied into the 10-dword output (u at [6], v at [7], v low bit cleared): if entry+0x10C == 4 then u += f32 entry+0x110 and v += f32 entry+0x114 for every vertex of every strip (and clampUV = 0, seed 103). After a strip is copied: mode 1 sets output vertices v0 = (u 0x118, v 0x114), v1 = (0x110, 0x114), v2 = (0x118, 0x11C), v3 = (0x110, 0x11C); mode 2 sets v0 = (0x118, 0x11C), v1 = (0x118, 0x114), v2 = (0x110, 0x11C), v3 = (0x110, 0x114); both then set the mode to 0 for the rest of the record. entry+0x10C is the per-object UV mode copied from ctx+0x1f8524 (nl_ctx_uv_mode), which FUN_140848ee0 clears when the registered object ctx+0x1f8528 is not the one being drawn. The runner read +0x10C only to zero clampUV and never applied the offsets; the select globe scrolling text ring (mode 4) drew from the unscrolled page region.',
  consequence='Ported verbatim in collect_world (uvm per entry, uvo = the four floats at +0x110..+0x11C).';

RELATE finding:supergun_world_walk_frame_arena_entries->cites->source:supergun_world_walk_20260906 SET id='e191_arena_src';
RELATE finding:supergun_world_walk_uv_modes->cites->source:supergun_world_walk_20260906 SET id='e191_uv_src';
RELATE finding:supergun_world_walk_frame_arena_entries->cites->finding:list0c_texture_patch_rule SET id='e191_arena_107';
RELATE finding:supergun_world_walk_uv_modes->cites->global:nl_ctx_uv_mode SET id='e191_uv_103';
RELATE finding:supergun_world_walk_frame_arena_entries->about->steam_routine:FUN_140848ee0 SET id='e191_arena_builder';
RELATE finding:supergun_world_walk_uv_modes->about->steam_routine:FUN_1408482a0 SET id='e191_uv_consumer';

UPDATE finding:supergun_world_walk_frame_arena_entries SET status='confirmed';
UPDATE finding:supergun_world_walk_uv_modes SET status='confirmed';
