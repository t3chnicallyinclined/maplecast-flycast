-- 192_supergun_one_stream_rematch_roster.surql -- two runner-side rules settled 2026-09-06 on the
-- running game: sprites and world entries must be one depth-tested stream in the flush order, and
-- the rematch roster is config state the select copies into blk at slot entry (so a post-entry
-- anchor needs slot 0 applied by hand). Forward slashes only in ref/note; no apostrophes.
USE NS re DB kb;

UPSERT source:supergun_one_stream_20260906 SET kind='reproduction', strength='reproduction',
  ref='re_kb 109 (flush order, category-3 sort), 103 (depth presets), 190 (consumer rule, NDC, converted z); GhidraMCP decompile of FUN_140629dc0 (team-slot entry, rematch roster block under config+0x840 == 2); runner supergun/supersoaker/runner/sg_runner/src/main.rs collect_world / emit_sprite / reenter_select; live runs cgeom13 / cgeom14 (operator confirmed on screen: hit counter, combo counter and hit sparks in front of the characters after the change, behind them before it).',
  note='Gate for the stream rule is the on-screen confirmation on two live runs; the numeric per-draw order gate against a captured frame (RENDER-PIXEL-GATE) has not been run.';

UPSERT finding:supergun_one_stream_depth_order SET status='confirmed', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='A shell must draw sprite quads and world entries as ONE stream in the flush order against one depth buffer; a separate sprite layer puts the HUD under the characters and lets a category-3 body paint over a nearer category-0 hit spark',
  statement='The flush FUN_140843eb0 -> FUN_140842e30 walks passes 2, 3, 0 and categories 0..3, dispatching every entry by kind in that order: world kinds 0..3 to FUN_1408482a0, sprite kind 0xC to FUN_1408458b0, category 3 qsorted by key descending then sequence ascending (re_kb 109). The HUD list carries the smallest keys and so draws LAST; a category-0 (mode 0) sprite such as a hit spark is drawn with depth WRITE (preset 3/4) before the category-3 bodies, which are depth-TESTED only (preset 2), so a nearer spark occludes a body drawn after it (re_kb 103 depth presets; sprite vertex z = rec+0x0C, the converted sort key, comparable with the world z). The runner previously drew all world entries on the GPU and then blitted every sprite from a CPU raster over them: the HUD landed under the characters and, with no depth buffer in the sprite pass, the later-drawn body always won over the spark. The fix emits each 0xC record as a GPU quad into the same batch list in the walk (emit_sprite: consumer geometry from re_kb 190, NDC x = (x-hw)/hw, y = (hh-y)/hh with hw/hh = *(ctx+0x1f82a4/8)*0.5, z = rec+0x0C, w = 1; sampler clamp+point, no cull, texture-alpha shader, blend normal or the additive family on flags 0x1000, depth write for categories 0..2 and test-only for 3), with the category-3 sort applied to the whole bucket so both kinds interleave.',
  consequence='Hit counter, combo counter and hit sparks draw in front as in the game; the CPU sprite raster is retired from the window path (kept for --dlist-shot). Textures per sprite key 0x1000000 | bank<<12 | slot: the slot texels through the live bank palette, cached on a content signature of texels + palette.';

UPSERT finding:supergun_rematch_roster_slot_entry SET status='confirmed', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='The rematch roster is config state (config+0x6B4 + i*4 cid, config+0x694 + i*4 assist, i = side + 2*slot) that the team-slot entry FUN_140629dc0 copies into blk when config+0x840 == 2; nothing about teams is in the arc',
  statement='FUN_140629dc0 (team-slot entry) under config+0x840 == 2: for slot > 0 it first clears (-1) a rematch id equal to an already-placed same-side slot; then if config[0x6B4 + i*4] >= 0 it writes fighter_i+0x6C0 (blk+0x4478 + i*0x738, the hovered id) and fighter_i+0x001 (blk+0x3DB9) = that id and fighter_i+0x655 (blk+0x440D) = config[0x694 + i*4]; otherwise the hovered id comes from the default table DAT_140a6fad0. It then requests the preview upload (0x142edf543 = 1, 0x142edf546 |= side+1), builds the pages (FUN_140616730 / FUN_140616960) and caches DAT_142eeea40[i] = cid. The Collection host fills config+0x6B4/0x694 when it re-enters the mode after a match; the runner reload of the captured config restored the CAPTURE session roster there (an auto-confirm showed Venom / Zangief / SonSon). The runner now writes its match snapshot (cid, assist per fighter index) into those config words at select re-entry, and -- because the vs-01 anchor was captured with both seats already inside slot 0, so slot-0 entry never runs again -- applies the three slot-0 writes by hand for i = 0 and 1 (the post-entry-anchor rule the runner already uses for the win tallies). The preview cache keeps the anchor ids, so the game per-side compare FUN_14062b130 rebuilds and re-uploads both previews and banners on its own.',
  consequence='After a KO, a seat that picks SAME TEAM confirms through its last team; a seat that picks CHANGE TEAMS starts from it. Costume is not in this block (not carried).';

RELATE finding:supergun_one_stream_depth_order->cites->source:supergun_one_stream_20260906 SET id='e192_stream_src';
RELATE finding:supergun_one_stream_depth_order->cites->finding:steam_translucent_sort_order SET id='e192_stream_109';
RELATE finding:supergun_one_stream_depth_order->cites->finding:supergun_sprite_consumer_flip_anchor_rule SET id='e192_stream_190';
RELATE finding:supergun_rematch_roster_slot_entry->cites->source:supergun_one_stream_20260906 SET id='e192_roster_src';
RELATE finding:supergun_rematch_roster_slot_entry->cites->finding:select_highlight_upload_chain SET id='e192_roster_188';
RELATE finding:supergun_rematch_roster_slot_entry->about->steam_routine:FUN_140629dc0 SET id='e192_roster_fn';

UPDATE finding:supergun_one_stream_depth_order SET status='confirmed';
UPDATE finding:supergun_rematch_roster_slot_entry SET status='confirmed';
