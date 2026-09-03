USE NS re DB kb;

-- ============================================================================
-- 110_frame_background.surql
-- 2026-09-03 -- The frame BACKGROUND: how Steam MvC2 clears a frame (three full-screen quads, no
-- ClearRenderTargetView on the scene RT) and where the background colour comes from
-- (FUN_1406101b0 == SH4 loc_8c02dc4c: stage words blk+0x6CB8.. x the deck multiplier blk+0x6CA8, the
-- fade word blk+0x6CE4 / colour blk+0x6CF0, the entity blackout byte). Read in Ghidra (mvc_dump.bin,
-- bridge :8080) and in marvelous2 bank02/bank03; gated byte-exact on 3 capgate packs.
-- Doc: mvc-live-skins-quarters/docs/FRAME-BACKGROUND-GHIDRA.md. Consumer: d3dcap/replay/tape_to_seq.py
-- (bg_rule.py). Gate: d3dcap/replay/bg_gate.py.
-- Apply: PYTHONIOENCODING=utf-8 python tools/re_kb/apply_seed.py tools/re_kb/110_frame_background.surql
-- ============================================================================

UPSERT source:frame_background_ghidra SET kind='disassembly', strength='code',
  ref='mvc_dump.bin (Ghidra dumpproj, bridge :8080): FUN_1406101b0 (per-frame background rule, called from FUN_14060a1f0 @0x14060a220), FUN_1408450e0 (writes ctx+0x1f8248/4c/50 and +0x1f8254 = DAT_1408e35e4 1e7; sole match-time writer), FUN_1408450a0 (menu writer, caseD_0 @0x140611cb8), FUN_14060b400 (G[0..2] == 2,1,2), FUN_14060b430 (G+0x2E & 1), FUN_140619960 (returns blk+0x6CE4), FUN_140619970 (fade step: blk+0x6CEC counter, 0x6CE8 kind -> 0x6CE4), FUN_140619910/a80/ac0/b00/b50/b80 (fade starts: 0x6CE8 kind, 0x6CEC frames, 0x6CF0 colour 0xffffff/0), FUN_1406104b0 (mode 0 setter == loc_8c02dc1c), FUN_140610480 (mode 2 setter == loc_8c02dc32), FUN_140620200 (stage load: DAT_14097db20[stage*0xC]; stages 3/0xC -> mode 2 two words, else mode 0), FUN_140620420 (per-frame re-assert from FUN_1406283a0), FUN_14061f030 (G+0x98 = entity+0x96 each frame), FUN_140843eb0 (frame flush: 4-vertex quad format 0x4000 stride 0x1c texId 0xffff, colours ctx+0x1f8248/4c/50/50 | 0xff000000, order TL BL TR BR), FUN_140070940 (ring executor: format-0x4000 branch converts to the 40-B layout, colour bytes 0 and 2 swapped -> R,G,B,A; entry: FUN_14033c5c0(dev, 0x3f, black) = the host clear quad; command 0 (pass begin): FUN_14033c5c0(dev, 0x30) = depth+stencil clear quad), FUN_14033c5c0 (clear-by-quad, state hash 0x?0b16, colour table DAT_142eb36c0 = zeros). SH4: loc_8c02dc4c (rule), loc_8c02dcd6 (multiplied path: fmul/ftrc per byte), loc_8c02de12 (raw path), loc_8c02dc1c/24/32/3e (mode 0/1/2/3 setters), loc_8c11d630 (colour sink, fr4 = 0x4b189680 1e7), loc_8c03591e/3e (gates), loc_8c0310f2 (fade word), data 0x8c26a8b4 (mode), 0x8c26a8a4+4 (deck), 0x8c26a8f0 (fade colour), *0x8c2896b0 +0x76/+6 (entity gates). Constants: DAT_1408e6048 255.0, DAT_1408dd5a8 1.0.',
  note='Read 2026-09-03. CONFIRMED = decompiled/read on both sides where a pair exists. UNKNOWN: the SH4 twin of the Steam per-stage colour table DAT_14097db20 (loc_8c045ce0 does not carry it); the meaning of entity+6; the issuer of ctx+0x1f8254 consumers.';

UPSERT source:frame_background_gate SET kind='measurement', strength='reproduction',
  ref='mvc-live-skins-quarters/d3dcap/replay/bg_gate.py over capgate/frame_4445, 4505, 7279 .pack + capgate/state dumps: the three preamble quads rebuilt from the dump bytes with bg_rule.py are BYTE-EXACT against the packs (9/9 quads x 4 vertices, 9/9 index orders (0,1,2,2,1,3), 3/3 1x1 pages ffffffff); scene RT most-common viewport colour == derived background on 2/2 frames with a bmp (4505: 355,600 px, 7279: 382,641 px). Limit: all three packs are stage 0x0B whose stage word is 0 -> the multiplied / gradient colour path is confirmed by decompile only, not by a pixel.',
  note='2026-09-03. Deterministic gate (frozen dump + frozen pack).';

-- routines
UPSERT steam_routine:FUN_1406101b0 SET addr='0x1406101b0', name='FUN_1406101b0', role='per-frame background colour rule (== SH4 loc_8c02dc4c): fight path multiplies the stage bytes blk+0x6CB8.. by the deck floats blk+0x6CA8.. (int trunc per byte), fade word blk+0x6CE4 != 0 -> blk+0x6CF0 raw with mode forced 0, entity+0x96 && entity+6 -> black with mode 0; menu path reads the words raw; mode blk+0x6CB4 0..3 -> (A,A,A)/(A,B,A)/(A,A,B)/(A,B,C) -> FUN_1408450e0', matched=true;
UPSERT steam_routine:FUN_1408450e0 SET addr='0x1408450e0', name='FUN_1408450e0', role='background colour sink: ctx+0x1f8248/4c/50 = c0/c1/c2, ctx+0x1f8254 = 1e7 (== SH4 loc_8c11d630 with fr4 = 1e7)', matched=true;
UPSERT steam_routine:FUN_1408450a0 SET addr='0x1408450a0', name='FUN_1408450a0', role='menu background colour sink (ctx+0x1f8254 = 1e5); caller caseD_0 @0x140611cb8', matched=false;
UPSERT steam_routine:FUN_140619970 SET addr='0x140619970', name='FUN_140619970', role='fade/strobe step: counter blk+0x6CEC, kind blk+0x6CE8 -> blk+0x6CE4 (kind 0: 1; 1,3..6: (cnt>>1)&1; 2: cnt&1; 7/8: 2; 9: 3; counter 0 -> 0)', matched=false;
UPSERT steam_routine:FUN_140619960 SET addr='0x140619960', name='FUN_140619960', role='returns blk+0x6CE4 (fade/strobe active word) (== SH4 loc_8c0310f2)', matched=true;
UPSERT steam_routine:FUN_140620420 SET addr='0x140620420', name='FUN_140620420', role='per-frame stage update (from FUN_1406283a0): re-asserts the stage background words from DAT_14097db20 (mode 2 for stages 3/0xC); animates the fog block blk+0x6A8C/0x6A94 on stages 0 and 4', matched=false;
UPSERT steam_routine:FUN_14061f030 SET addr='0x14061f030', name='FUN_14061f030', role='per-frame: G+0x98 (blk+0x3D50) = entity+0x96 (the super-blackout byte; bit 4 selects the entity slot)', matched=false;
UPSERT steam_routine:FUN_140070940 SET addr='0x140070940', name='FUN_140070940', role='host ring executor (drawJack): entry FUN_14033c5c0(dev,0x3f) clear quad; cmd 0 (pass begin) FUN_14033c5c0(dev,0x30) depth+stencil clear quad; format 0x4000 draws -> 40-B layout (pos, normal (0,1), colour bytes 0/2 swapped, colour1, uv); texId 0xffff = 1x1 white page', matched=false;
UPSERT steam_routine:FUN_14033c5c0 SET addr='0x14033c5c0', name='FUN_14033c5c0', role='host clear-by-quad: flags 0x3f colour+depth+stencil (stride 28, blend off, ALWAYS, stencil REPLACE 0), 0x30 depth+stencil only (ps null, colour mask 0); colour DAT_142eb36c0 = black', matched=false;
UPSERT steam_routine:FUN_140620200 SET addr='0x140620200', name='FUN_140620200', role='stage load: fog block from DAT_140a6ed40[stage], background words from DAT_14097db20[stage] (stages 3/0xC: FUN_140610480 mode 2; else FUN_1406104b0 mode 0), per-stage init callbacks PTR_PTR_140a6ec20', matched=true;

UPSERT routine:loc_8c02dc4c SET addr='0x8c02dc4c', name='loc_8c02dc4c', bank='bank02', role='background colour rule (DC): gates loc_8c03591e/3e, multiplied path loc_8c02dcd6 or raw loc_8c02de12, mode 0x8c26a8b4 -> loc_8c11d630(c0,c1,c2, 1e7)';
UPSERT routine:loc_8c02dcd6 SET addr='0x8c02dcd6', name='loc_8c02dcd6', bank='bank02', role='background words x deck floats 0x8c26a8a8/ac/b0 per byte (fmul, ftrc, extu.b, shll16/shll8); fade word (loc_8c0310f2) -> raw 0x8c26a8f0; entity +0x76 && +6 -> 0';
UPSERT routine:loc_8c02de12 SET addr='0x8c02de12', name='loc_8c02de12', bank='bank02', role='background words read raw as 3-byte triples (menu path)';
UPSERT routine:loc_8c02dc1c SET addr='0x8c02dc1c', name='loc_8c02dc1c', bank='bank02', role='background setter mode 0: 0x8c26a8b4 = 0, +4 = colour';
UPSERT routine:loc_8c02dc32 SET addr='0x8c02dc32', name='loc_8c02dc32', bank='bank02', role='background setter mode 2: 0x8c26a8b4 = 2, +4 = A, +8 = B (horizontal gradient A left -> B right)';

RELATE steam_routine:FUN_1406101b0->recompiles->routine:loc_8c02dc4c SET kind='recompiles', confidence='confirmed', evidence='both decompiled/read: same gates (G[0..2]==2,1,2; G+0x2E&1), same three-way mode switch, same (A,A,A)/(A,B,A)/(A,A,B)/(A,B,C) argument order, same far constant 1e7';
RELATE steam_routine:FUN_1406104b0->recompiles->routine:loc_8c02dc1c SET kind='recompiles', confidence='confirmed', evidence='mode 0 + one word (function-map row 998)';
RELATE steam_routine:FUN_140610480->recompiles->routine:loc_8c02dc32 SET kind='recompiles', confidence='confirmed', evidence='mode 2 + two words';
RELATE steam_routine:FUN_140619960->recompiles->routine:loc_8c0310f2 SET kind='recompiles', confidence='confirmed', evidence='both return the fade word (blk+0x6CE4 / 0x8c26a8e4 through the block map); called at the same point of the rule';

-- globals
UPSERT global:bg_mode SET name='bg_mode', dc_addr='0x8c26a8b4', steam_addr='blk+0x6CB4', type='u32', note='background gradient mode 0..3 (0 flat, 1 TL/TR/BR = A, BL = B, 2 left A / right B, 3 TL A, BL B, right C); stage constant (FUN_140620200/420), forced 0 by the fade/blackout branches of FUN_1406101b0';
UPSERT global:bg_words SET name='bg_words', dc_addr='0x8c26a8b8', steam_addr='blk+0x6CB8', type='u32[3]', note='three packed 0x00RRGGBB background words (byte 2 = R x deck[0], byte 1 = G x deck[1], byte 0 = B x deck[2]); stage table DAT_14097db20 (12 B/stage): 0x7f7f7f stages 0,4,6,7,0xD,0xF,0x10; 0x6061e3 stages 1,0xA; 0 stages 2,5,8,9,0xB,0xE; stages 3/0xC mode 2 (0 -> 0xb0459a)';
UPSERT global:fade_word SET name='fade_word', dc_addr='0x8c26a8e4', steam_addr='blk+0x6CE4', type='u32', note='fade/strobe active (FUN_140619970 output); != 0 -> background = fade colour raw, mode 0; also read by the render dispatcher FUN_140620960';
UPSERT global:fade_colour SET name='fade_colour', dc_addr='0x8c26a8f0', steam_addr='blk+0x6CF0', type='u32', note='0xffffff (white flash: FUN_140619910/b00) or 0 (black: a80/ac0/b50/b80)';
UPSERT global:deck_colour SET name='deck_colour', dc_addr='0x8c26a8a8', steam_addr='blk+0x6CA8', type='f32[3]', note='R,G,B multipliers (FUN_1406105a0 sets all three; FUN_1406104d0 resets to 1.0): applied to the deck (POL model 0), the fog colour bytes (FUN_140610730) and the background words (FUN_1406101b0)';
UPSERT global:host_bg_colour SET name='host_bg_colour', steam_addr='DAT_142ef0ab0+0x1f8248', type='u32[3] + f32', note='ctx background words c0 (TL), c1 (BL), c2 (TR+BR) and far 1e7; sole match-time writer FUN_1408450e0; read by FUN_140843eb0 into the 0x4000 quad';

-- finding
UPSERT finding:frame_background_rule SET date='2026-09-03', confidence='high', status='confirmed',
  contribution_candidate=true,
  statement='THE FRAME IS CLEARED BY THREE DRAWS AND THE BACKGROUND COLOUR IS A blk FUNCTION. Every captured frame opens with (i) the host clear quad (FUN_14033c5c0(dev, 0x3f, black) at the ring executor entry: stride 28, TL/TR/BL/BR, blend off, DepthFunc ALWAYS, depth write, stencil REPLACE ref 0), (ii) the engine background quad (FUN_140843eb0: format 0x4000, TL/BL/TR/BR, colours ctx+0x1f8248/4c/50/50 | 0xff000000, texId 0xffff = the 1x1 white page; the executor converts it to the 40-B layout with the colour bytes 0 and 2 swapped -> R,G,B,A, normal (0,1), uv 0; blend SRC_ALPHA/INV_SRC_ALPHA, LESS_EQUAL, depth write, stencil off) and (iii) the depth+stencil clear quad at the first pass-begin command (FUN_14033c5c0(dev, 0x30): ps null, colour mask 0, ALWAYS, write). The colour words come from FUN_1406101b0 (== SH4 loc_8c02dc4c), run once per frame from FUN_14060a1f0: in a fight (G[0..2] == 2,1,2 and !(G+0x2E & 1)) -- fade word blk+0x6CE4 != 0 -> mode 0 and A = blk+0x6CF0 raw; entity+0x96 (== G+0x98 blackout) && entity+6 -> mode 0 and A = 0; else A/B/C = per-byte (int)((float)byte * deck[k]) & 0xff of blk+0x6CB8/BC/C0 with deck = blk+0x6CA8 (R), 0x6CAC (G), 0x6CB0 (B); outside a fight the words are used raw. Mode blk+0x6CB4: 0 (A,A,A), 1 (A,B,A), 2 (A,A,B), 3 (A,B,C), else no update. The words are per-stage constants (DAT_14097db20, set at stage load and re-asserted every frame). Gate: bg_gate.py 9/9 quads byte-exact, 3/3 white pages, 2/2 scene-RT background pixels on the three capgate packs (all stage 0x0B = black; the multiplied/gradient path is decompile-confirmed only).',
  note='CONFIRMED by decompile of FUN_1406101b0, FUN_1408450e0, FUN_140843eb0, FUN_140070940 (0x4000 branch and cmd 0), FUN_14033c5c0, FUN_140620200/420, FUN_140619970, FUN_14061f030 and by reading SH4 loc_8c02dc4c/dcd6/de12. Implemented in tape_to_seq.py (preamble draws, --no-preamble) via bg_rule.py with a per-stage table fallback for tapes without the 0.3.42 columns. Open: entity+6 meaning (INFERRED non-zero in a fight); a non-black-stage capture to pixel-gate the multiplied path and the mode-2 gradient (stages 3/0xC).';
RELATE finding:frame_background_rule->cites->source:frame_background_ghidra SET kind='cites';
RELATE finding:frame_background_rule->cites->source:frame_background_gate SET kind='cites';
RELATE finding:frame_background_rule->about->steam_routine:FUN_1406101b0 SET kind='about';
RELATE finding:frame_background_rule->about->steam_routine:FUN_1408450e0 SET kind='about';
RELATE finding:frame_background_rule->about->steam_routine:FUN_140843eb0 SET kind='about';
RELATE finding:frame_background_rule->about->steam_routine:FUN_140070940 SET kind='about';
RELATE finding:frame_background_rule->about->steam_routine:FUN_14033c5c0 SET kind='about';
RELATE finding:frame_background_rule->about->steam_routine:FUN_140620200 SET kind='about';
RELATE finding:frame_background_rule->about->steam_routine:FUN_140620420 SET kind='about';
RELATE finding:frame_background_rule->about->steam_routine:FUN_140619970 SET kind='about';
RELATE finding:frame_background_rule->about->routine:loc_8c02dc4c SET kind='about';
RELATE finding:frame_background_rule->about->global:bg_mode SET kind='about';
RELATE finding:frame_background_rule->about->global:bg_words SET kind='about';
RELATE finding:frame_background_rule->about->global:fade_word SET kind='about';
RELATE finding:frame_background_rule->about->global:fade_colour SET kind='about';
RELATE finding:frame_background_rule->about->global:deck_colour SET kind='about';
RELATE finding:frame_background_rule->about->global:host_bg_colour SET kind='about';
RELATE steam_routine:FUN_1406101b0->reads->global:bg_mode SET kind='reads', confidence='confirmed', evidence='*(DAT_142edf560 + 0x6cb4) at three sites';
RELATE steam_routine:FUN_1406101b0->reads->global:bg_words SET kind='reads', confidence='confirmed', evidence='uint3 reads 0x6cb8/0x6cbc/0x6cc0 (raw path) and byte reads 0x6cb8..0x6cc2 (multiplied path)';
RELATE steam_routine:FUN_1406101b0->reads->global:deck_colour SET kind='reads', confidence='confirmed', evidence='fVar1/2/3 = 0x6ca8/0x6cac/0x6cb0';
RELATE steam_routine:FUN_1406101b0->reads->global:fade_colour SET kind='reads', confidence='confirmed', evidence='uVar8 = *(0x6cf0) when FUN_140619960() != 0';
RELATE steam_routine:FUN_1406101b0->writes->global:bg_mode SET kind='writes', confidence='confirmed', evidence='0x6cb4 = 0 in the fade and blackout branches';
RELATE steam_routine:FUN_1406101b0->calls->steam_routine:FUN_1408450e0 SET kind='calls', confidence='confirmed', evidence='CALL @0x140610470 (c0, c1, c2, DAT_1408e35e4)';
RELATE steam_routine:FUN_1406101b0->calls->steam_routine:FUN_140619960 SET kind='calls', confidence='confirmed', evidence='CALL @0x140610267';
RELATE steam_routine:FUN_1408450e0->writes->global:host_bg_colour SET kind='writes', confidence='confirmed', evidence='stores to ctx+0x1f8248/4c/50/54';
RELATE steam_routine:FUN_140843eb0->reads->global:host_bg_colour SET kind='reads', confidence='confirmed', evidence='local_74/58/3c = ctx+0x1f8248/4c/50 | 0xff000000 -> FUN_140048370(0x4000, quad, 0x1c, 4, 0xffff, 0, 0) @0x14084403b';
RELATE steam_routine:FUN_140619970->writes->global:fade_word SET kind='writes', confidence='confirmed', evidence='0x6ce4 per kind of 0x6ce8';
RELATE steam_routine:FUN_140620200->writes->global:bg_mode SET kind='writes', confidence='confirmed', evidence='via FUN_140610480 / FUN_1406104b0 from DAT_14097db20[stage*0xC]';
RELATE steam_routine:FUN_140620420->writes->global:bg_mode SET kind='writes', confidence='confirmed', evidence='same two setters, every frame from FUN_1406283a0 @0x14062851b';
RELATE steam_routine:FUN_14060a1f0->calls->steam_routine:FUN_1406101b0 SET kind='calls', confidence='confirmed', evidence='CALL @0x14060a220 (per frame, before the render dispatch)';
