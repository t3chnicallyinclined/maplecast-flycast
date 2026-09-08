-- 188_select_highlight_upload.surql -- CHARACTER SELECT: the per-highlight preview/banner build + upload chain,
-- the per-slot "highlight changed" cache DAT_142eeea40, the select atlas (AFS 0x95 at DC 0x0C420000), and the
-- SUPERGUN write sets that make the game re-upload BOTH seats' pages at select entry.
-- 2026-09-06, SUPERGUN lane. Every routine claim = Ghidra decompile/disassembly of mvc_dump.bin (dumpproj);
-- every value claim = bytes read out of mvc_dump.bin or the vs-01 anchor; the builder formulas are gated by a
-- byte-exact offline replay against the anchor's own DC-RAM pages. Idempotent. Use forward slashes in refs.
USE NS re DB kb;

-- ── sources ───────────────────────────────────────────────────────────────────────────────────────
UPSERT source:select_highlight_decompiles_20260906 SET kind='disasm', strength='code',
  ref='Ghidra dumpproj/mvc_dump.bin via GhidraMCP: FUN_1406163e0, FUN_140616910, FUN_140616730 (+ full disassembly), FUN_140616960, FUN_140616610, FUN_140845830, FUN_14062b130, FUN_14062c480 (+ disassembly), FUN_14062aa00, FUN_140629dc0, FUN_14062b060, FUN_140629ab0, FUN_14062c830, FUN_1407ed2c0, FUN_14062bc80, FUN_14062b3c0, FUN_140608a00, FUN_14060b8a0, FUN_140607d60, FUN_140741510, FUN_1407412a0, FUN_14060c370 case 5 (HTTP bridge :8080); capstone over 0x14062a460..0x14062a810, 0x1407413d0..0x140741510, 0x140632c50..0x140632ca0, 0x140036560; byte tables DAT_140a6c070/080/0c0/100/210/218/220, 0x1408e2f80, DAT_140a6fad0 read from mvc_dump.bin (file offset = addr - 0x140000000).',
  note='Static read. Writers of 0x142edf543/0x142edf546 and readers of 0x142eeea40 enumerated by get_xrefs_to.';

UPSERT source:select_highlight_anchor_replay_20260906 SET kind='capture', strength='reproduction',
  ref='mvc-live-skins-quarters/d3dcap/ttd/runs/vs-01/pre/{blk.bin,dcram.bin,exe_image.bin}; scripts in the 2026-09-06 scratchpad (anchor_probe4.py, blockmap.py): replay of FUN_140616610+FUN_140616730 from the anchor page and FUN_140616960 from the atlas, compared byte-for-byte; md5 block map of every 8 KiB / 2 KiB block of the two preview pages over the whole 32 MiB image.',
  note='vs-01: G+1..4 = [1,1,3,0]; both sides outer 6 / inner 1 / slot 0; hovered P1 0x0E, P2 0x17; G+0x94 = 0x0C420000; G+0x29 = 1; 0x142edf543/546 = 0/0; DAT_142eeea40 = [0x0E, 0x17, -1, -1, -1, -1]. Result: 0/64 2-KiB blocks differ on both preview pages, both banners byte-exact.';

-- ── routines ──────────────────────────────────────────────────────────────────────────────────────
UPSERT steam_routine:FUN_1406163e0 SET addr='0x1406163e0', name='FUN_1406163e0',
  role='DEFERRED TEXTURE RE-UPLOAD DISPATCHER (one job per frame). Gate: 0x142edf543 (pending) ; job: 0x142edf546. kind 1 = FUN_140616910 (P1: slot 0x810 <- dcram+0xDB0000, 0x81C <- dcram+0xDF0000); kind 2 = 0x811 <- dcram+0xDD0000, 0x81D <- dcram+0xDF4800; kind 3 = kind 1 now then flag=1,kind=2 (P2 next frame); 4/5 = 0x81C <- dcram 0x420000/0x4A0000 (post-match); 6..10 = 0x81D+n (results, FUN_140616390); 0xB/0xC = 0x816/0x817/0x81B from G+0x94 pages by G+0xAD; 0xD..0x1C = 0x910/0x911 pages from G+0x94; 0x1D = 0x914 stage thumbnail dcram+(blk+0x6D04+0x18C)*0x8000. Every upload = FUN_1408458a0(0) + FUN_140845830(slot, src) -> FUN_140048910(slot,w,h,fmt,src) with w/h/fmt from ctx+0x1e00a0+slot*0x18. Called from FUN_14060b8a0 <- FUN_140608a00 <- FUN_140607d60 (0x140607d85).';
UPSERT steam_routine:FUN_140616910 SET addr='0x140616910', name='FUN_140616910',
  role='P1 select-page upload: FUN_1408458a0(0); FUN_140845830(0x810, dcram+0xDB0000); FUN_140845830(0x81C, dcram+0xDF0000). 0x810 = 256x256 fmt 0x0101 RGB565 twiddled (0x20000 B), 0x81C = 256x256 fmt 0x0300 VQ (0x4800 B) per the slot table / runner measurement.';
UPSERT steam_routine:FUN_140616730 SET addr='0x140616730', name='FUN_140616730',
  role='SELECT PREVIEW PAGE BUILDER (DC-RAM -> DC-RAM memcpy, no AFS read, no upload). i = side + 2*slot; cid = u8 blk+0x4478+i*0x738 (fighter+0x6C0 = HOVERED id). Portrait column 4x8KiB: dst = dcram+0xDB0000 + (c210[i]&0xFC)*0x8000 + (c070[(c210[i]&3)+4j]<<13); src = dcram+(G+0x94-0x0C000000) + (c080[cid]&0xFC)*0x8000 + (c070[(c080[cid]&3)+4j]<<13). Name column 8x2KiB: dst = dcram+0xDB0000+(i&1)*0x20000+(c218[k]<<11); src = dcram + (G+0x94) + ((c218[nidx&7] + ((nidx&0xF8)-0x2F88)*4)<<12) + (c218[k]<<11), nidx = (G+0x29 ? DAT_140a6c100 : DAT_140a6c0c0)[cid] (name atlas = (G+0x94)+0x1E0000). Tables: DAT_140a6c210 = {1,5,2,6,3,7}; DAT_140a6c070 = 4x4 read as COLUMNS; DAT_140a6c080 = identity except 0x18..0x1A -> 0x18 and >= 0x1B -> cid-2; DAT_140a6c218 = {0,1,4,5,0x10,0x11,0x14,0x15}. Assist type (+0x655) is NOT read.';
UPSERT steam_routine:FUN_140616960 SET addr='0x140616960', name='FUN_140616960',
  role='SELECT BANNER BUILDER: 0x4800 B memcpy; dst = dcram+0xDF0000+(i&1)*0x4800; src = dcram + (G+0x94) - 0x0BD20000 + idx*0x4800 (banner atlas = (G+0x94)+0x2E0000), idx = cid < 0x1B ? cid : cid-3; cid = u8 blk+0x4478+i*0x738.';
UPSERT steam_routine:FUN_140616610 SET addr='0x140616610', name='FUN_140616610',
  role='SELECT PLACEHOLDER PORTRAITS: for k in 0..2 copy atlas portrait idx 0x39+k (page 14, cols 1..3) into side-page column DAT_140a6c220[side*3+k] = {1,2,3} (P1) / {5,6,7} (P2). Called from outer state 1 (FUN_14062b060) at side entry.';
UPSERT steam_routine:FUN_14062b130 SET addr='0x14062b130', name='FUN_14062b130',
  role='PER-SIDE CURSOR TICK (both sides, from FUN_14062c480 at 0x14062c4fe / 0x14062c511, unconditional). HIGHLIGHT-CHANGED COMPARE: for i in {side, side+2, side+4}: if (DAT_142eeea40[i] >= 0 && DAT_142eeea40[i] != fighter_i+0x6C0) { 0x142edf543 = 1; 0x142edf546 |= side+1; FUN_140616730(i); FUN_140616960(i); DAT_142eeea40[i] = cid }. Then if ((blk+0x33a12 & param_1+0x654) == 0) dispatch PTR_FUN_140a6fb70[*(u8*)(param_1+4)](param_1).';
UPSERT steam_routine:FUN_140629dc0 SET addr='0x140629dc0', name='FUN_140629dc0',
  role='OUTER CURSOR STATE 3 = PTR_FUN_140a6fb70[3] = TEAM-SLOT ENTRY. Sets +4 = 4 (or 9 when all three slot bits of blk+0x33a0a+side are set), +0x28 = slot fighter struct; hovered blk+0x4478+i*0x738 from the defaults DAT_140a6fad0 (first available by roster mask blk+0x33a28+side*8) or from the rematch picks config+0x6b4+i*4 (config+0x840 == 2); FUN_14073ed90; REQUEST (0x142edf543 = 1, 0x142edf546 |= side+1); FUN_140616730(i); FUN_140616960(i); DAT_142eeea40[i] = cid; blk+0x33a3c+side = slot+1; FUN_14073ec70.';
UPSERT steam_routine:FUN_14062b060 SET addr='0x14062b060', name='FUN_14062b060',
  role='OUTER CURSOR STATE 1 = PTR_FUN_140a6fb70[1]. sub 0: blk+0x33a08+side = 0; FUN_1407404a0(side,0); FUN_140616610(side) (placeholders). sub 1: REQUEST kind |= side+1 (upload the placeholder page; no cache write). sub 1/2: when blk+0x33a08+side == 0 -> outer state 2.';
UPSERT steam_routine:FUN_140616390 SET addr='0x140616390', name='FUN_140616390',
  role='results-screen page decompress: FUN_140611e90(dcram + 0x520000 + table[idx]*, dcram + (param_1+0x39)*0x20000); idx = param_2 < 0x1B ? param_2 : param_2-3. Feeds dispatcher kinds 6..10 (slots 0x81D+n).';
UPSERT steam_routine:FUN_140741510 SET addr='0x140741510', name='FUN_140741510',
  role='ASSIST TYPE SELECT PANEL object (INFERRED role): spawned at confirm from FUN_14062aa00; FUN_14061dbe0(0,5,1) list-5 node; +0x20 = current team-slot fighter struct; model = *(PTR_DAT_142edf5a0+0x310) P1 / +0x348 P2 (select-bank POL 837 models); +0xF0 = 0x80F; per-side position DAT_140a99b40+side*0xC; tick LAB_1407413d0 (jump table, rewrites +0x5C/+0x60). Writes NO texture request: the per-character type text is a record/UV selection over static common-bank pages.';
UPSERT steam_routine:FUN_1407412a0 SET addr='0x1407412a0', name='FUN_1407412a0',
  role='select name/portrait callout object (INFERRED role): spawned at confirm; model = *(PTR_DAT_142edf5a0+0x220+side*8); per-cid UV +0x5C/+0x60 from DAT_140a99790[(cid + side*0x3B)*8] floats; +0xF0 = 0x806; +1 = cid.';

-- ── globals ───────────────────────────────────────────────────────────────────────────────────────
UPSERT global:select_upload_pending SET addr='n/a', steam_off='0x142edf543', type='u8',
  note='Pending flag of the deferred texture re-upload dispatcher FUN_1406163e0. Set to 1 by every request site; cleared by the dispatcher after each job and by FUN_14061e170 / FUN_14060af70. Inside the exe page 0x142edf300..+0x400 the runner restores. No effect on blk (DETERMINISM-CONTRACT.md).';
UPSERT global:select_upload_kind SET addr='n/a', steam_off='0x142edf546', type='u8',
  note='Job kind of FUN_1406163e0. Select pages: bit0 = P1 (0x810/0x81C), bit1 = P2 (0x811/0x81D); 3 = both (P1 this frame, P2 next). Other values: 4/5 post-match 0x81C, 6..10 results, 0xB/0xC arcade continue/streak pages, 0xD..0x1C 0x910/0x911 pages, 0x1D stage thumbnail 0x914.';
UPSERT global:select_highlight_cache SET addr='n/a', steam_off='0x142eeea40', type='i32[6]',
  note='THE per-slot "highlight changed" state. Indexed by fighter slot i = side + 2*team_slot. -1 = no preview built for this slot yet (select entry FUN_14062bc80 writes 6 x -1 from 0x1408e2f80); otherwise the cid whose portrait/name/banner is currently in the side page. Written by FUN_14062b130 / FUN_14062aa00 / FUN_140629dc0 / FUN_140629ab0 / FUN_14062c830 (= cid), compared only in FUN_14062b130. NOT in blk; NOT in the 0x142edf300..+0x400 page the runner restores; it IS inside exe_image.bin (the runner loads the whole image at startup, main.rs:2829).';
UPSERT global:select_atlas_base SET addr='n/a', steam_off='G+0x94 (blk+0x3D4C)', type='u32 DC address',
  note='Select atlas base = 0x0C420000 (0x0C810000 when G+0x84 == 1), written by FUN_14060c370 case 5 (select entry) after FUN_14060dcf0(0x95 -> DC 0x0CC00000) + FUN_140611e90 word-LZSS decompress into it. Layout: portraits at +0 (4 per 0x20000 page, 64-wide columns), names at +0x1E0000 (8 per page), banners at +0x2E0000 (53 x 0x4800 VQ). Case 9 (another screen) writes the swapped pair. Reused by FUN_1406163e0 kinds 0xB..0x1C for other screens.';

-- ── findings ──────────────────────────────────────────────────────────────────────────────────────
UPSERT finding:select_highlight_upload_chain SET status='inferred', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='Character-select preview/banner: the scene REQUESTS (0x142edf543/546) and BUILDS the DC-RAM pages itself; FUN_1406163e0 uploads one seat per frame',
  statement='Every select-scene site does the same four things: 0x142edf543 = 1; 0x142edf546 |= side+1; FUN_140616730(i) (preview page: hovered portrait column + name column, DC-RAM memcpy from the atlas at G+0x94); FUN_140616960(i) (banner, 0x4800 B from (G+0x94)+0x2E0000); then DAT_142eeea40[i] = cid. i = side + 2*team_slot, cid = fighter_i+0x6C0 (the HOVERED id). Sites: FUN_140629dc0 (team-slot entry), FUN_14062aa00 (hovered id changed), FUN_14062b130 (cache compare, both sides every frame while G+3 == 3), FUN_14062b060 (placeholders at side entry, request only), FUN_140629ab0 / FUN_14062c830 / FUN_1407ed2c0 (CPU side / reveal / VS object). The frame function FUN_140607d60 -> FUN_140608a00 -> FUN_14060b8a0 -> FUN_1406163e0 consumes the request: kind bit0 -> FUN_140616910 = FUN_1408458a0(0) + FUN_140845830(0x810, dcram+0xDB0000) + FUN_140845830(0x81C, dcram+0xDF0000); bit1 -> 0x811 <- dcram+0xDD0000, 0x81D <- dcram+0xDF4800; both -> P1 now, P2 next frame. FUN_140845830 -> FUN_140048910(slot, w, h, fmt, src) from the slot table. NO FUN_14060dcf0 call per highlight: the AFS read is once, at select entry, FUN_14060c370 case 5: AFS 0x95 -> DC 0x0CC00000 -> LZSS -> G+0x94 = DC 0x0C420000 (the atlas), plus 0x345/0x346 (common bank POL/TEX, TCW 0x810..0x81D), 0x97|0x98 by G+0x29, 0x96.',
  consequence='The four select slots 0x810/0x811/0x81C/0x81D are RUNTIME pages (pixels at DC 0x0CDB0000/0x0CDD0000/0x0CDF0000/0x0CDF4800, outside TEX 838), rebuilt from the atlas on every highlight event and uploaded only on request. A shell that restores DC-RAM but never triggers a request shows whatever its texture cache last held for those slots.';

UPSERT finding:select_highlight_cache_is_exe_global SET status='inferred', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='The "did the highlight change" state is DAT_142eeea40 (i32 x 6, exe .data), not a blk byte; -1 = not built, else the cid in the page',
  statement='FUN_14062b130 compares DAT_142eeea40[i] (i = side, side+2, side+4) against fighter_i+0x6C0 and rebuilds+requests on (cache >= 0 && cache != cid). FUN_14062bc80 (select entry) initialises all six to -1 (0x1408e2f80 = 16 x 0xFF plus a literal -1). The runner loads the whole exe_image.bin at startup (sg_runner main.rs:2829) so the cache equals the capture cids (vs-01: [0x0E, 0x17, -1, -1, -1, -1] == the hovered ids) and no request ever fires; reenter_select() restores only 0x142edf300..+0x400, so the cache keeps the previous session values (a moved seat mismatches and refreshes, a never-moved seat matches and never uploads). The upload is therefore neither unconditional per tick nor cursor-substate driven.',
  consequence='Two write sets, both through the game code. (A) upload only: wr8(0x142edf543,1); wr8(0x142edf546,3) AFTER the exe-page restore -> P1 pages this frame, P2 next; correct iff the restored dcram pages match blk (true for a self-consistent anchor; proven byte-exact on vs-01). (B) rebuild + upload via the compare, when the shell itself wrote fighter+0x6C0 (--roster) or dcram and blk are not from one capture: for side s, cur = *(i16*)(blk+0x3DB8+s*0x738+0x32); for k in 0..=min(cur,2): wr32(0x142eeea40 + (s+2k)*4, 0x7F) (any value >= 0 outside the 0..0x37 roster). Requires G+3 == 3. Never poke a slot the side has not reached (cache == -1): the game would paint the default team ids over the placeholders and move the banner to slot 2. Both remain a CHANGE TO THE SHELL until the captured uploads are gated byte-identical against the game (Gate 1/N1).';

UPSERT finding:select_atlas_layout_gated SET status='inferred', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='Select preview/banner builders reproduced BYTE-EXACT against the vs-01 anchor pages from the anchor atlas',
  statement='Replaying FUN_140616610(side) + FUN_140616730(side + 2k, k <= current slot) starting from the anchor page gives 0/64 differing 2-KiB blocks on BOTH 0x0CDB0000 and 0x0CDD0000; FUN_140616960 reproduces both banners at 0x0CDF0000/0x0CDF4800 exactly (P1 cid 0x0E, P2 cid 0x17). An md5 block map locates every written block at the formula address: P1 column 1 = 0xC490000/0xC492000/0xC498000/0xC49A000 (= atlas page 3 column 2 = c080[0x0E]&0xFC, &3), columns 2/3 = page 14 columns 2/3 (placeholders 0x3A/0x3B), name blocks 0xC634000.. = 0xC600000 + (0x0E&0xF8)*0x4000 + c218[6]*0x1000. The 8 never-written 2-KiB blocks (x in [32,64) of column 0) equal the DC-RAM fill pattern at 0x0C000000. DAT_140a6c070 is indexed as COLUMNS ((q&3)+4j): a preview page is four 64-px-wide columns (name 32 px, portraits 64 px), i.e. the strips are stored rotated.',
  consequence='A forced rebuild in the shell is a DC-RAM no-op for a self-consistent anchor: the game re-derives identical pages and then uploads them. The atlas AFS 0x95 is therefore the only asset the select preview needs beyond the common bank; dcram_build.py can place it demand-driven like the results bank.';

UPSERT finding:select_assist_panel_no_upload SET status='inferred', confidence='medium', date='2026-09-06',
  title='The ASSIST TYPE SELECT panel requests no texture upload; it is a list-5 scene object over static common-bank pages',
  statement='The assist inner state (0x14062a460..0x14062a810 = PTR_LAB_140a6fbc8[2], inside FUN_1407f3570) has no writer of 0x142edf543/546 (xref set) and its only calls are FUN_14060f990 (rand), FUN_140609510 (sfx), 0x140036560 (a ret stub) and FUN_14060b8c0 (capstone). FUN_140616730/FUN_140616960 never read fighter+0x655. The panel is spawned at CONFIRM by FUN_140741510(side struct): a FUN_14061dbe0(0,5,1) node with +0x20 = the current team-slot fighter, model *(PTR_DAT_142edf5a0+0x310|+0x348) from the select-bank POL, key 0x80F, whose tick LAB_1407413d0 rewrites +0x5C/+0x60 (UV/frame). The companion FUN_1407412a0 (name/portrait callout, key 0x806) selects a per-cid UV from DAT_140a99790.',
  caveat='Which TCW and UV table LAB_1407413d0 uses is UNKNOWN (its jump table was not read). Consequence for SUPERGUN: a never-moved seat needs only its two dynamic pages forced; the assist text comes with the static bank.';

-- ── edges ─────────────────────────────────────────────────────────────────────────────────────────
RELATE finding:select_highlight_upload_chain->cites->steam_routine:FUN_1406163e0 SET id='e188_chain_disp';
RELATE finding:select_highlight_upload_chain->cites->steam_routine:FUN_140616910 SET id='e188_chain_p1';
RELATE finding:select_highlight_upload_chain->cites->steam_routine:FUN_140616730 SET id='e188_chain_build';
RELATE finding:select_highlight_upload_chain->cites->steam_routine:FUN_140616960 SET id='e188_chain_banner';
RELATE finding:select_highlight_upload_chain->cites->steam_routine:FUN_140629dc0 SET id='e188_chain_slot';
RELATE finding:select_highlight_upload_chain->cites->steam_routine:FUN_14062aa00 SET id='e188_chain_choose';
RELATE finding:select_highlight_upload_chain->cites->steam_routine:FUN_14062b130 SET id='e188_chain_tick';
RELATE finding:select_highlight_upload_chain->cites->steam_routine:FUN_14062b060 SET id='e188_chain_outer1';
RELATE finding:select_highlight_upload_chain->cites->steam_routine:FUN_140845830 SET id='e188_chain_reupload';
RELATE finding:select_highlight_upload_chain->cites->global:select_atlas_base SET id='e188_chain_atlas';
RELATE finding:select_highlight_cache_is_exe_global->cites->global:select_highlight_cache SET id='e188_cache_g';
RELATE finding:select_highlight_cache_is_exe_global->cites->global:select_upload_pending SET id='e188_cache_flag';
RELATE finding:select_highlight_cache_is_exe_global->cites->global:select_upload_kind SET id='e188_cache_kind';
RELATE finding:select_highlight_cache_is_exe_global->cites->steam_routine:FUN_14062b130 SET id='e188_cache_cmp';
RELATE finding:select_highlight_cache_is_exe_global->cites->steam_routine:FUN_14062bc80 SET id='e188_cache_init';
RELATE finding:select_atlas_layout_gated->cites->steam_routine:FUN_140616730 SET id='e188_gate_build';
RELATE finding:select_atlas_layout_gated->cites->steam_routine:FUN_140616960 SET id='e188_gate_banner';
RELATE finding:select_atlas_layout_gated->cites->steam_routine:FUN_140616610 SET id='e188_gate_ph';
RELATE finding:select_assist_panel_no_upload->cites->steam_routine:FUN_140741510 SET id='e188_assist_obj';
RELATE finding:select_assist_panel_no_upload->cites->steam_routine:FUN_1407412a0 SET id='e188_assist_name';
RELATE finding:select_highlight_upload_chain->cites->finding:select_dispatch_tables SET id='e188_prior_tables';
RELATE finding:select_highlight_upload_chain->cites->finding:texbank_hud_runtime_slots SET id='e188_prior_runtime_slots';
RELATE steam_routine:FUN_14062b130->reads->global:select_highlight_cache SET id='e188_r_cache';
RELATE steam_routine:FUN_14062b130->writes->global:select_highlight_cache SET id='e188_w_cache';
RELATE steam_routine:FUN_14062bc80->writes->global:select_highlight_cache SET id='e188_w_cache_init';
RELATE steam_routine:FUN_140616730->reads->global:select_atlas_base SET id='e188_r_atlas_730';
RELATE steam_routine:FUN_140616960->reads->global:select_atlas_base SET id='e188_r_atlas_960';
RELATE steam_routine:FUN_1406163e0->reads->global:select_upload_pending SET id='e188_r_flag';
RELATE steam_routine:FUN_1406163e0->reads->global:select_upload_kind SET id='e188_r_kind';

-- ── evidence, then promotion ──────────────────────────────────────────────────────────────────────
RELATE finding:select_highlight_upload_chain->cites->source:select_highlight_decompiles_20260906 SET id='e188s_a0';
RELATE finding:select_highlight_upload_chain->cites->source:select_highlight_anchor_replay_20260906 SET id='e188s_b0';
RELATE finding:select_highlight_cache_is_exe_global->cites->source:select_highlight_decompiles_20260906 SET id='e188s_a1';
RELATE finding:select_highlight_cache_is_exe_global->cites->source:select_highlight_anchor_replay_20260906 SET id='e188s_b1';
RELATE finding:select_atlas_layout_gated->cites->source:select_highlight_decompiles_20260906 SET id='e188s_a2';
RELATE finding:select_atlas_layout_gated->cites->source:select_highlight_anchor_replay_20260906 SET id='e188s_b2';
RELATE finding:select_assist_panel_no_upload->cites->source:select_highlight_decompiles_20260906 SET id='e188s_a3';

UPDATE finding:select_highlight_upload_chain SET status='confirmed';
UPDATE finding:select_highlight_cache_is_exe_global SET status='confirmed';
UPDATE finding:select_atlas_layout_gated SET status='confirmed';
-- select_assist_panel_no_upload stays 'inferred': single decompile, the object's UV table unread.
