USE NS re DB kb;

-- ============================================================================
-- 116_portrait_pages.surql
-- 2026-09-04 -- The twelve RUNTIME-PATCHED HUD slots (TCW 0xC9A..0xCA5): where
-- the in-picture portrait and name-plate pixels come from, why resolving them
-- out of the capture-derived TCW library drew a FOREIGN ROSTER on every tape
-- (the "Sentinel on a Magneto/Storm/Colossus recording" bug), and the gate that
-- closes TEXTURE-BANKS-GHIDRA.md s8.4 / RENDER-STATUS-2026-09-03.md tweak 4.
-- Doc: mvc-live-skins-quarters/docs/PORTRAIT-PAGES-GHIDRA.md
-- Rip + gate: mvc-live-skins-quarters/d3dcap/replay/rip_portraits.py [--gate]
-- Consumers: rr-render/src/world.rs hud_portrait_pages ; tape_to_seq.py
--            hud_portrait_pages
-- Apply with tools/re_kb/apply_seed.py after the documented backup. Idempotent.
-- APPLIED 2026-09-04 by the SUPERGUN ENGINE lane, after folding in the GATE 3 raw-byte DC-RAM gate
-- (source:portrait_dcram_gate below): the rule is now gated against LIVE GAME MEMORY, not only
-- against the capture-derived TCW library. See mvc-live-skins-quarters/docs/RECEIPT-RUNNER-GATE3.md s4.4.
-- ============================================================================

-- sources ---------------------------------------------------------------------
UPSERT source:ghidra_portrait_writers SET kind='ghidra', strength='code',
  ref='mvc_dump.bin via :8080 bridge decompile_function: FUN_14060d560, FUN_140611e90, FUN_1406162e0, FUN_140616330; capstone disassembly of 0x14060f450 and 0x14062a460; DAT_140a6aac4 / DAT_140a6aac8 / DAT_140a6d190 bytes read directly from mvc_dump.bin (file offset = addr - 0x140000000)',
  note='FUN_14060d560 loops the six fighter slots (blk + s*0x738, s < 6 by the 0x2b50 bound), takes data = *(u32**)(blk + 0x3fb0 + s*0x738) (= fighter+0x1F8, the per-character HUD/portrait file), calls FUN_140611e90(data + data[0], dcram + 0xE60000) (16-bit LZSS), then k = DAT_140a6aac8[s] and memcpy 0x800 B from dcram + (DAT_140a6aac4[*(u8*)(blk+0x440d+s*0x738)] + 0x1CC0)*0x800 into texHdr[10+k].loc and 0x800 B from dcram+0xE61000 into texHdr[16+k].loc, where texHdr = *(PTR_DAT_142edf598+0xb08) (16-B records, loc at +8; the decompiled +0xa8 / +0x108 are records 10 and 16). TCW = 0xC90 + record, so portrait = 0xC9A+k and name plate = 0xCA0+k. HUD AFS 835 records 10..21 are 32x32 fmt 1 (RGB565) type 1 (twiddled).';

UPSERT source:portrait_dat_gate SET kind='measurement', strength='gate',
  ref='mvc-live-skins-quarters/d3dcap/replay/rip_portraits.py --gate (game_50.arc AFS entry 3+cid vs d3dcap/replay/tcw_pages/index.json, 2026-09-04)',
  note='29/29 -- every captured 0xC9A..0xCA5 page in the TCW library (up to four content variants per key, from several capture sessions) is reproduced BYTE-EXACT by decompressing AFS 3+cid sub-blob 0 and taking the page the rule names, for cids 42/44/50/52/23/53. Zero contradictions. Validates simultaneously: the LZSS port, DAT_140a6d190 = 3+cid, the four-page layout (0/1/3 = portrait per assist type, 2 = name plate) and the slot->record mapping. Falsification arm: pristine git HEAD emit_seq vs the patched Python oracle on 6 frames of tape 59616461 = 150 tex[0] mismatches; patched Rust vs patched Python = 0.';

UPSERT source:portrait_emitter_gate SET kind='measurement', strength='gate',
  ref='rr-render/tools/gate_l1.sh MODE=full (4 tapes, 5 clips) and rr-render/tools/gate_l3.mjs on tape 59616461 rows 200..229, 2026-09-04',
  note='L1 5/5 PASS, draws exact 24574 / 17969 / 21061 / 24481 / 2588 with the rule live on BOTH sides. L3 30/30 frames byte-exact (browser wasm tape path vs the Python .seq). Deliberate re-baseline: 6-12 of the twelve HUD pages change on every previously rendered tape (59613662 8/12, 59613506 10/12, 59614009 10/12, 59612784 12/12), so any stored scene-RT hash for a frame that draws list 11 is invalidated on purpose.';

UPSERT source:portrait_dcram_gate SET kind='measurement', strength='gate',
  ref='mvc-live-skins-quarters/d3dcap/receipt/dcram_build.py --gate (GATE 3, 2026-09-04) against d3dcap/ttd/runs/receipt-20260903-stage9-anchor/pre/dcram.bin and d3dcap/ttd/runs/20260903-000941/pre/dcram.bin',
  note='STRONGER THAN THE LIBRARY GATE: reproducing the twelve uploads from the arc and writing them into a rebuilt 32 MB DC-RAM image matches the LIVE image byte-for-byte -- all 12 pages x 0x800 B on BOTH dumps (rosters [42,23,52,12,44,46] stage 9 and [42,52,44,23,52,53] stage 0x0B), 0 differing bytes, with the destinations taken from the live POL texHdr locs (0x0D0BB800.. and 0x0D0BE800..). Cross-checks the rule end to end: the same (cid, assist) yields the same live page in a DIFFERENT slot on the other dump, and cid 52 appearing twice in one roster with assist 0 and assist 2 correctly selects pages 1 and 3. Trap recorded: rip_portraits.char_pages returns RGBA, not the raw 0x800 twiddled page -- comparing its output against DC-RAM shows a false mismatch on all twelve pages.';

-- Steam routines ---------------------------------------------------------------
UPSERT steam_routine:FUN_14060d560 SET addr='0x14060d560', name='FUN_14060d560', role='HUD bank load + the twelve per-character portrait / name-plate uploads (TCW 0xC9A..0xCA5)',
  note='CONFIRMED both read (already paired with loc_8c032696 in the function map; this seed adds the portrait semantics). Runs at every match load, after FUN_14060d8f0(PTR_DAT_142edf598, 0xd082000). The G+0x29 != 0 branch (DAT_142edf580+0x29) selects sub-blob 3 instead of 0 for cids {16,28,30,31,33,38,58} (bit masks 0xd0010000 and 0x4000042 on *(u8*)(blk+0x4478+s*0x738) = fighter+0x6C0 = char id) -- NOT reproduced, probably a non-battle mode: UNKNOWN.';

UPSERT steam_routine:FUN_140611e90 SET addr='0x140611e90', name='FUN_140611e90', role='16-bit LZSS decompressor (character HUD/portrait file -> DC 0x0CE60000)',
  note='CONFIRMED read and ported verbatim (rip_portraits.lzss16). A control word supplies 16 flags MSB-first; flag 0 = literal word; flag 1 = match word w with count = w>>11 and offset = w & 0x7FF, and when count == 0 the NEXT word is the count and w itself is the offset. offset 0 with count > 0 writes count zero words; offset 0 with count 0 ends the stream. Offsets and counts are in 16-bit WORDS into the output. Gate: the three sub-blobs of every character file decompress to exactly 0x2000 / 0x4800 / 0x1800 B, the sizes HUD records 10..21 / 9 / 22..24 need.';

UPSERT steam_routine:FUN_1406162e0 SET addr='0x1406162e0', name='FUN_1406162e0', role='character-switch re-upload of TCW 0xC99 (256x256 fmt1 type3 VQ, the large win/super artwork)',
  note='CONFIRMED read: FUN_140611e90(*(fighter+0x1f8) + *(u32*)(*(fighter+0x1f8)+4), dcram+0xE60000); FUN_1408458a0(0); FUN_140845830(0xc99, dcram+0xE60000). Sub-blob 1 of the same per-character file.';

UPSERT steam_routine:FUN_140616330 SET addr='0x140616330', name='FUN_140616330', role='character-switch re-upload of TCW 0xCA6 + (*(fighter+0x230)>>1) (128x128 fmt1 type3 VQ)',
  note='CONFIRMED read. Sub-blob 2 of the same per-character file.';

UPSERT steam_routine:FUN_14062a460 SET addr='0x14062a460', name='FUN_14062a460', role='character select: writes the ASSIST TYPE byte fighter+0x655',
  note='CONFIRMED read (capstone). Slot = ((*(i8*)(cursor+0x6b0)) & 1) + (*(i16*)(cursor+0x32))*2, times 0x738 -- i.e. EVEN = P1 / ODD = P2, team position in +0x32. Random select writes rand() % 3 (call 0x14060f990 then the 0xaaaaaaab reciprocal); the A1 (0x2000) / A2 (0x1000) button paths at [blk + slot*2 + 0x33a68] write (prev -/+ 1 + 3) % 3. Three values = alpha / beta / gamma.';

UPSERT steam_routine:FUN_14060f450 SET addr='0x14060f450', name='FUN_14060f450', role='rematch / slot shuffle: copies cid (+0x6C0), assist (+0x655), costume (+0x6C1) and +0x6F0 between fighter slots',
  note='CONFIRMED read (capstone, undefined function in Ghidra: body 0x14060f450..0x14060f8xx). Also contains lea rdx,[rax + 0x3db8] / lea r9,[rax + 0x44f0], re-confirming the fighter-slot base blk + 0x3DB8 + i*0x738 (finding:steam_object_base_fix).';

-- fields / globals --------------------------------------------------------------
UPSERT field:steam_fighter_0x655 SET name='assist_type', offset='+0x655', owner='steam_object', type='u8', class='logical',
  semantics='Assist type alpha=0 / beta=1 / gamma=2, chosen at character select (FUN_14062a460, values are taken mod 3). Read pervasively by the per-character move scripts (0x140636000..0x14073b000 band). FUN_14060d560 indexes DAT_140a6aac4 = {1,0,3,0} with it to pick which 0x800-B page of the character DAT is uploaded as that the HUD portrait of that slot, so the portrait carries the assist-type colour field (page 0 green, 1 red, 3 blue; 200-280 of 1024 texels differ between them). Absolute address blk + 0x440d + s*0x738. The agent already ships it as tape `assist[slot]`: harvest.rs OFF_ASSIST = 0x4E9 off the pre-fix base and 0x4E9 + 0x16C = 0x655.',
  status='confirmed';

UPSERT field:steam_fighter_0x1F8 SET name='hud_portrait_file', offset='+0x1F8', owner='steam_object', type='ptr', class='asset',
  semantics='Host pointer to the 32 KB per-character HUD/portrait file, loaded at the VS screen by FUN_14060c370 case 6 (SH4 loc_8c032cbe) via FUN_14060dcf0(DAT_140a6d190[cid], fighterBase+0x148000). Layout: u32 off[3] then three 16-bit-LZSS blobs -- off[0] -> four 0x800-B 32x32 RGB565 twiddled pages (0/1/3 = portrait per assist type, 2 = NAME PLATE), off[1] -> the 0xC99 256x256 VQ page, off[2] -> the 0xCA6+ 128x128 VQ page. Absolute address blk + 0x3FB0 + s*0x738.',
  status='confirmed';

UPSERT global:steam_DAT_140a6aac8 SET name='fighter_slot_to_hud_record', addr='0x140a6aac8', note='u8[6] = 00 03 01 04 02 05 (read from mvc_dump.bin). Fighter slot s -> HUD texHdr record offset k: portrait record 10+k (TCW 0xC9A+k), name record 16+k (TCW 0xCA0+k). With EVEN = P1 / ODD = P2 this collapses to: P1 team member i -> 0xC9A+i / 0xCA0+i, P2 team member i -> 0xC9D+i / 0xCA3+i.', status='confirmed';

UPSERT global:steam_DAT_140a6aac4 SET name='assist_to_portrait_page', addr='0x140a6aac4', note='u8[4] = 01 00 03 00 (read from mvc_dump.bin). Indexed by *(fighter+0x655) (assist type 0..2) to choose the 0x800-B page of the character DAT blob uploaded as the HUD portrait. Entry 3 is unreachable from the select code (values are mod 3).', status='confirmed';

UPSERT global:steam_DAT_140a6d190 SET name='char_hud_file_afs_index', addr='0x140a6d190', note='u16[59] = 3 + cid for every cid, with cids 25 and 26 collapsed onto entry 27 (read from mvc_dump.bin). The AFS entry inside game_50.arc holding the per-character HUD/portrait file. RECEIPT-RUNNER-DCRAM.md already gated all eleven per-character files 66/66 byte-exact against the live DC-RAM image.', status='confirmed';

-- findings ----------------------------------------------------------------------
UPSERT finding:portrait_pages_are_per_character SET
  title='HUD portrait and name-plate pages (TCW 0xC9A..0xCA5) are per-CHARACTER, patched at every match load from the six fighters own DATs',
  status='confirmed',
  statement='HUD TEX records 9..24 have loc >= 0x1E000 in a 0x1E000-byte file: they carry no static pixels. FUN_14060d560 fills records 10+k (portrait, TCW 0xC9A+k) and 16+k (name plate, TCW 0xCA0+k) for each fighter slot s with k = DAT_140a6aac8[s] = {0,3,1,4,2,5}[s], from AFS entry 3+cid decompressed with the 16-bit LZSS FUN_140611e90: portrait = page DAT_140a6aac4[*(fighter+0x655)] = {1,0,3,0}[assist type], name plate = page 2 (fixed). Slot order is EVEN = P1 / ODD = P2 with team position s/2, so P1 team i -> 0xC9A+i / 0xCA0+i and P2 team i -> 0xC9D+i / 0xCA3+i. The whole rule is resolvable offline from a tape with zero free parameters: p1_team, p2_team and assist[6] are all already carried.',
  gate='rip_portraits.py --gate: 29/29 captured library pages reproduced byte-exact from the character DATs, 0 contradictions.';

UPSERT finding:portrait_capture_library_wrong_roster SET
  title='Resolving TCW 0xC9A..0xCA5 out of the capture-derived TCW library draws the CAPTURE session roster on every tape (the "Sentinel on a Mag/Storm/Colossus recording" bug)',
  status='confirmed',
  statement='A real (non-synthetic) tape carries no `pages`, so world textures resolve by TCW key through tape_pages -> lib_pages = d3dcap/replay/tcw_pages/index.json. Its bare 0xC9A..0xCA5 entries decode to SENTINEL / STORM / MAGNETO / STORM / SENTINEL / MAGNETO portraits and the matching name plates -- the roster of the D3D capture the library was built from -- so EVERY tape rendered those. On tape 76561197999665347_76561198047120675_76561198047120675_59616461 (p1_team [42,44,50], p2_team [42,44,8]) that is a Sentinel portrait and name plate for a match containing no Sentinel. The tape SELECTION was never wrong: list-11 binds all twelve keys per frame and they drop out and return exactly on KO / round restart (0xC9B and 0xCA1 vanish together at clock 2487 and return at 4906), which is also why the picture appears not to follow tags -- the twelve slots are all resident and only their pixels were frozen. Not an index/base-arithmetic error and not the DOM overlay (overlay.meta and p1_team/p2_team are correct end to end).',
  gate='pristine git HEAD emit_seq vs the patched Python oracle on 6 frames = 150 tex[0] mismatches; patched Rust vs patched Python = 0; L1 5/5 and L3 30/30 after the fix.',
  fix='rr-render/src/world.rs hud_portrait_pages() inserts the twelve resolved keys into WorldState.tape_pages (which is consulted before lib_pages); tape_to_seq.py hud_portrait_pages() does the same; pages ship in the asset pack as portraits/index.json + pc_<cid>_p<k>.png (tools/pack_assets.py, rip_portraits.py).';

UPSERT finding:portrait_big_slots_open SET
  title='TCW 0xC99 and 0xCA6..0xCA8 remain unbound: the slot is global and re-uploaded on a character switch',
  status='open',
  statement='FUN_1406162e0 and FUN_140616330 re-upload 0xC99 (256x256 VQ) and 0xCA6+(*(fighter+0x230)>>1) (128x128 VQ) from sub-blobs 1 and 2 of the SAME per-character file whenever a character switches in. Both slots are global across the two sides, so "whose art is in it this frame" is stateful. rip_portraits.py --big can now produce the pages per cid; the emitter still logs "no page for 00000C99" and drops the draw (missing, never wrong). 0xC99 appears on 476 of 8302 frames of tape 59616461. Deriving the owner from the tape drawn[6] column is INFERRED, not gated.';

-- edges --------------------------------------------------------------------------
RELATE finding:portrait_pages_are_per_character->cites:portrait_pages_ghidra->source:ghidra_portrait_writers SET note='FUN_14060d560 / FUN_140611e90 / FUN_1406162e0 / FUN_140616330 decompiled; DAT tables read from the exe image';
RELATE finding:portrait_pages_are_per_character->cites:portrait_pages_gate->source:portrait_dat_gate SET note='29/29 byte-exact';
RELATE finding:portrait_capture_library_wrong_roster->cites:portrait_wrong_roster_writers->source:ghidra_portrait_writers SET note='the decompiled writer that makes the twelve slots per-match, which is why a static capture-derived library cannot be right';
RELATE finding:portrait_capture_library_wrong_roster->cites:portrait_emitter_gate->source:portrait_emitter_gate SET note='L1 5/5, L3 30/30, 150 -> 0 tex mismatches';
RELATE finding:portrait_capture_library_wrong_roster->about:portrait_bug_writer->steam_routine:FUN_14060d560 SET note='the writer whose per-match behaviour the static library ignored';
RELATE finding:portrait_pages_are_per_character->about:portrait_assist_field->field:steam_fighter_0x655 SET note='the byte that picks the portrait page; already on the wire as tape assist[slot]';
RELATE finding:portrait_pages_are_per_character->about:portrait_file_field->field:steam_fighter_0x1F8 SET note='the per-character HUD file the pages come from';
RELATE steam_routine:FUN_14060d560->reads:FUN_14060d560_DAT_140a6aac8->global:steam_DAT_140a6aac8 SET confidence='confirmed';
RELATE steam_routine:FUN_14060d560->reads:FUN_14060d560_DAT_140a6aac4->global:steam_DAT_140a6aac4 SET confidence='confirmed';
RELATE steam_routine:FUN_14060d560->calls:FUN_14060d560_FUN_140611e90->steam_routine:FUN_140611e90 SET confidence='confirmed';
RELATE steam_routine:FUN_1406162e0->calls:FUN_1406162e0_FUN_140611e90->steam_routine:FUN_140611e90 SET confidence='confirmed';
RELATE steam_routine:FUN_140616330->calls:FUN_140616330_FUN_140611e90->steam_routine:FUN_140611e90 SET confidence='confirmed';
RELATE steam_routine:FUN_14062a460->writes:FUN_14062a460_0x655->field:steam_fighter_0x655 SET confidence='confirmed', evidence='mov byte [rsi+0x440d], r8b at 0x14062a4fe and 0x14062a58e';
RELATE steam_routine:FUN_14060f450->writes:FUN_14060f450_0x655->field:steam_fighter_0x655 SET confidence='confirmed', evidence='mov byte [rax+0x440d], dl at 0x14060f727 plus the five 0x738-strided siblings';

-- ---------------------------------------------------------------------------
-- GATE 3 fold-in (SUPERGUN ENGINE lane, 2026-09-04): the same rule, gated against live game memory.
UPSERT finding:portrait_pages_are_per_character SET
  gate_dcram='dcram_build.py --gate (GATE 3): the twelve uploads rebuilt from game_50.arc match the LIVE DC-RAM image byte-exactly, 12 x 0x800 B, on two dumps with different stages and rosters (one containing the same cid twice with two different assist types). 0 differing bytes; UNEXPLAINED: NONE across the whole loader-placed set.';
RELATE finding:portrait_pages_are_per_character->cites:portrait_pages_dcram_gate->source:portrait_dcram_gate SET note='12/12 pages byte-exact vs live DC-RAM, two dumps';
