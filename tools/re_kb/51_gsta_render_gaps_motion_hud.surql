USE NS re DB kb;

-- ============================================================================
-- 2026-06-29 — A/B diagnosis (GSTA render_frame vs 7200 MIRROR ground truth, _live_fx6)
-- of the 3 post-effect-fix render gaps the user reported. Each CONFIRMED vs INFERRED.
-- ============================================================================

UPSERT source:gsta_render_gaps_ab SET kind='measurement',
  ref='_live_fx6.gsta.mcrr + _live_fx6.mirror.zcst (gameplay+motion+super, 1078 frames); marvelous2 bank15.asm loc_8c15FFB0; web/render-replica/hud-client.mjs; core/network/maplecast_mirror.cpp gstaBuildHudTA',
  note='A/B method: render each GSTA frame via render_frame_node, compare its sprite-TA TCW set + per-quad (Ax,Ay) to the mirror real-TA. Streams are roughly index-aligned (GSTA fi <-> mirror fnum-1) but DRIFT (frame drops), so per-pixel position A/B is skew-limited (~68px avg residual even when index-aligned) — exact-frame alignment needs a synced capture or a single-frame RAM dump.';

-- ISSUE 1a: motion "blocks" = body parts -> stale 0x85xxx during supers (CONFIRMED).
UPSERT finding:gsta_motion_blocks_body_idxtab SET
  statement='The gray/white/pink rectangular BLOCKS floating around characters during fast motion/supers (K_blocks1.png) = render_frame BODY parts resolving to the engine-NEVER-USED 0x85xxx TCW band. CONFIRMED-BY-MEASUREMENT (_live_fx6 fi=440-490, the super): the mirror renders 0x83xxx (27067 quads, real body band) and 0x89-8b (effect) but NEVER 0x85xxx (0/1077 frames); GSTA emits ~7-15 0x85xxx quads/super-frame (Storm body sel 247 / Cable sel 201, bit15-CLEAR -> the tiling body walker). ROOT CAUSE = the SAME char-pass-transient idxtab staleness as the effect fix (re_kb/50), now for the BODY path: during a super the body objects idxtab entries are reverted by the HUD pass, so the body alloc_index resolves to a stale 0x85xxx rectab entry instead of the correct 0x83/84xxx body band. The EFFECT quads were FIXED (per-frame post-pass remap to the contiguous effect block); the BODY parts have no equivalent remap yet. FIX DIRECTION: extend the post-pass / a body idxtab repair for bit15-CLEAR parts whose resolved TCW is the never-engine 0x85xxx (provably wrong) — remap to the correct body band or cull. Validate vs mirror (0x85xxx -> 0 quads). Distinct from the effect over-tile (fixed). re_kb/50 sibling.',
  status='confirmed', confidence='high', date='2026-06-29';

-- ISSUE 1b: the 2-part TRANSPOSITION (OPEN — needs frame-exact alignment).
UPSERT finding:gsta_body_part_transposition SET
  statement='OPEN. The user reports two CLEANLY-rendered (not garbled) Storm body parts PLACED WRONG — swapped diagonally puts them right (an anchor/cursor transposition, distinct from the 0x85xxx stale blocks). NOT yet isolated: in _live_fx6 the GSTA<->mirror streams drift (frame drops), so the per-part (Ax,Ay) A/B has ~68px skew noise that swamps a clean 2-part swap; the specific Storm aerial/cape pose could not be frame-exact aligned. The +0xDC cursor repair is NOT the cause on the cited NON-super aerial pose (resident +0xDC is valid there -> the repair is a no-op). NEEDS: either a frame-exact GSTA+mirror capture of the specific transposition pose, OR a single-frame RAM dump of that pose (MAPLECAST_DUMP_RAM at the moment) so render_frame can be A/B-diffed against the engine TA byte-exact (no skew). Hypotheses to test on aligned data: (a) the wide-part carve col/row rank tie (re_kb per_side_storage_col_reverses) swapping two same-Ax parts; (b) a per-record cursor off-by-one for a specific part count.',
  status='open', confidence='medium', date='2026-06-29';

-- ISSUE 2: HUD magenta bars = NOT A BUG (CONFIRMED via the ROM color table).
UPSERT finding:gsta_hud_bar_magenta_correct SET
  statement='NOT A BUG. The HUD life-bar MAGENTA (yellow->magenta) the user expected to be red/orange is the ENGINE REAL color. CONFIRMED from marvelous2 bank15.asm loc_8c15FFB0 (the life-bar modulate-color table): C1 point char = 0xFFFF40FF (yellow) + 0xFFFFFF00 -> a yellow->MAGENTA(FF40FF) bar; C2 = 0xFF00FF00 green; C3 = 0xFF00C0FF cyan. web/render-replica/hud-client.mjs BAR_COLS[0]=[#FF40FF,#FF9CFF] matches the ROM EXACTLY. MVC2 point-char life bar IS yellow->magenta. The recoverable-damage RED (red_health) renders as the dark-red CHIP (#8a1414) BEHIND the team-color HP (hud-client _lifeBar sub(chipFrac,CHIP_COL)), visible only when chipFrac>hpFrac. No fix needed; the magenta is correct per ROM.',
  status='confirmed', confidence='high', date='2026-06-29';

-- ISSUE 3: HUD meter flat-white (OPEN — needs a HUDQ-armed capture).
UPSERT finding:gsta_hud_meter_open SET
  statement='OPEN (capture-blocked). The bottom hyper-combo meter renders flat WHITE not tracking fill. ROOT-CAUSE constrained: the NATIVE GSTA client (title MapleCast Mirror) renders the HUD ONLY via the HUDQ wire block (maplecast_mirror.cpp gstaBuildHudTA — the engine real HUD quads); it does NOT use the hand-rolled hud-client.mjs meter (that procedural _meter reads meter_fill 0x8C289646/648, which IS shipped in the gstate read-set, and draws a dark base + team-color fill — NOT white). So the white meter is the engine real meter quad rendered from HUDQ. BUT _live_fx6 has ZERO HUDQ blocks (0/1078 — captured without MAPLECAST_HUD_TA), so the meter A/B vs the engine cannot be done from this capture. NEEDS: a capture WITH MAPLECAST_HUD_TA armed (HUDQ present in the GSTA wire) so the meter quads can be A/B-compared to the mirror — then determine if the meter is a fill-geometry/UV the HUDQ carries but render mis-applies, or a white-texture base whose fill overlay is missing. The hand-rolled hud-client.mjs meter (if the BROWSER client is used) reads the right state and should fill; verify which client the user sees.',
  status='open', confidence='high', date='2026-06-29';

RELATE finding:gsta_motion_blocks_body_idxtab->cites->source:gsta_render_gaps_ab;
RELATE finding:gsta_body_part_transposition->cites->source:gsta_render_gaps_ab;
RELATE finding:gsta_hud_bar_magenta_correct->cites->source:gsta_render_gaps_ab;
RELATE finding:gsta_hud_meter_open->cites->source:gsta_render_gaps_ab;
RELATE finding:gsta_motion_blocks_body_idxtab->about->finding:gsta_super_effect_overtile;
