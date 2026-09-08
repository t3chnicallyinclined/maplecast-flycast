-- 168_ctx_display_list.surql -- THE ENGINE'S OWN DISPLAY LIST, decoded. Closes UL4 of
-- supergun/docs/SUPERGUN-LATENCY-BUDGET.md (was: "is the ctx display list self-contained?").
-- senior-re-generalist lane, 2026-09-05. Idempotent and replayable; explicit edge ids.
USE NS re DB kb;

UPSERT source:steam_disasm_cache_20260905 SET kind='ghidra', strength='code',
  ref='mvc-live-skins-quarters/d3dcap/replay/re_map/cache/steam_disasm.jsonl -- the cached full disassembly of all 29678 Steam functions; FUN_1408436a0, FUN_140842e30, FUN_140848ee0, FUN_1400483c0 and FUN_140843eb0 read in full',
  note='The GhidraMCP bridge at :8080 was NOT running. Every address below came from the offline export. Its one blind spot is indirect calls, which is why the caller of FUN_140843eb0 is recorded as UNKNOWN rather than guessed.';

UPSERT source:ctx_out_display_list_parse SET kind='reproduction', strength='reproduction',
  ref='TEMP/rrcap_emu/frame_20260903-000941_chain_r0.ctx_out.bin (4 MB whole-ctx image, ctx base VA 0x0f7c1000, dcram base 0x0fbc1000) parsed with the layout read out of FUN_1408436a0',
  note='Deterministic and re-runnable offline from an artefact already on disk: it needs no game, no window and no GPU. The parse either decodes to a self-consistent list or it does not.';

UPSERT finding:ctx_display_list_layout SET status='open', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='The engine builds a complete 32-byte-per-item display list at ctx+0x30, and the layout is now fully decoded',
  statement='FUN_1408436a0(ECX=kind, RDX=record, R8D=record_len) is the append. Read from the binary: layer = *(ctx+0x1f828c); sub = 0..3 (3 is the sorted translucent bucket); bucket = sub + layer*4; count = *(ctx+0x1e0030 + bucket*4) capped at 0x800; the item is written at ctx + 0x30 + ((bucket*2048) + count)*32 (0x140843d8c-0x140843d9b: LEA R8,[RAX+RSI*4]; SHL R8,0xb; ADD R8,RCX; SHL R8,0x5). ITEM = { u32 index, u32 kind, u64 payload_ptr, u32 payload_len, f32 sort_key, u8 sub, u8 flag, u8 state }. The record is then memcpy(ctx+0x100030 + *(ctx+0x1e0080), record, len) at 0x140843df0, and three counters step: bucket count at ctx+0x1e0030+bucket*4, layer total at ctx+0x1e0070+layer*4, payload cursor at ctx+0x1e0080. The arena bound 0xE0000 is checked at 0x1408436c6.',
  method='ARITHMETIC CLOSURE, which is what makes this more than a reading: 16 buckets * 2048 items * 32 B = 0x100000, and ctx+0x30+0x100000 = ctx+0x100030 = the payload arena base, whose 0xE0000 extent ends exactly at ctx+0x1e0030 = the counts table. Every region abuts the next with no slack.',
  measured='Parsed out of frame_20260903-000941_chain_r0.ctx_out.bin: bucket counts (124,0,0,286,0,...) summing to 410; layer totals (410,0,0,0) -- the two agree; payload cursor 68640 = 0x10C20, so the payload arena spans ctx[0x100030..0x110C50). All 410 item payload pointers land inside that span. The sort key of the first translucent item is 106.2367, and 106.2357 is the depth this project already recorded for fighter slot 0 from the sprite walker on this same anchor.';
RELATE finding:ctx_display_list_layout -> cites -> source:steam_disasm_cache_20260905 SET id='ctx_dl_layout_disasm';
RELATE finding:ctx_display_list_layout -> cites -> source:ctx_out_display_list_parse SET id='ctx_dl_layout_parse';
UPSERT finding:ctx_display_list_layout SET status='confirmed';

UPSERT finding:ctx_display_list_partially_self_contained SET status='open', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='The display list is SELF-CONTAINED for 2D sprites and POINTS INTO DC-RAM for 3D world geometry -- 65 percent versus 35 percent on the measured frame',
  statement='Item kinds split cleanly by payload. On frame 20260903-000941, of 410 items: 250 of kind 12 (80 B, translucent bucket) and 16 of kind 16 (304 B) carry NO pointer -- the 80-byte record decodes as a finished screen-space quad (u32 texture/sprite id, f32 screen_x 464.3, f32 screen_y 261.6, f32 w, four UV floats, an RGBA quad, then mode and flag dwords). The remaining 144 items (kinds 0/1/2/3, 304 B = 0x130) hold an ABSOLUTE POINTER at record+0x00 into the DC-RAM window -- verified: every one of the 144 lands inside [dcram, dcram+32 MB) -- with the rest of the 304 bytes being three 4x4 matrices copied from ctx+0x1f816c, a constant block at 0x140ab20c0 and ctx+0x1f80ec (the NaomiLib matrix stack), plus render-state dwords from ctx+0x1f825c..0x1f8274.',
  consequence='CONSUMING THE DISPLAY LIST IN-PROCESS IS SUFFICIENT, because the runner owns DC-RAM in the same address space. SENDING IT OVER A WIRE IS NOT: 35 percent of the draws would arrive as dangling pointers, and the bytes they point at are exactly what the tape aobjs and nodes columns already carry. This is the fact that decides whether a display-list capture could ever replace the tape. It cannot, for the offline and browser paths.',
  falsifies='Reading R1 of SUPERGUN-LATENCY-BUDGET section 3.2 -- that the TA records hold references into DC-RAM -- is TRUE for the 3D kinds and FALSE for the 2D kinds. The single-answer framing of UL4 was wrong; the answer is per-kind.';
RELATE finding:ctx_display_list_partially_self_contained -> cites -> source:ctx_out_display_list_parse SET id='ctx_dl_split_parse';
RELATE finding:ctx_display_list_partially_self_contained -> cites -> source:steam_disasm_cache_20260905 SET id='ctx_dl_split_disasm';
UPSERT finding:ctx_display_list_partially_self_contained SET status='confirmed';

UPSERT finding:host_render_command_ring SET status='open', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='The recompile already has a flat host render-command protocol, and it is GATED OFF by one dword',
  statement='FUN_140842e30(ECX=layer, EDX=sub) is the display-list CONSUMER: it reads the bucket count, takes RDI = ctx+0x30+(bucket<<0x10), qsorts the bucket when sub is 3 (comparator 0x1408434d0, through FUN_140817f80), then dispatches per item on the item state byte at +0x1a through a 0x37-entry table at 0x140843220 / 0x140843244. It emits through FUN_1400483c0(ECX=cmd, RDX=src, R8D=len), which appends a record of u32 cmd, u32 len, u32 1, u32 seq, u64 0 then the payload to a ring at *(0x140acd3a8)+0xdbd20, with a 0x200000 bound and a length counter at +0xdbd28. FUN_140843eb0 is the per-frame pass that walks the layers, emitting cmd 0 with the layer id and then calling FUN_140842e30 per bucket.',
  gate='EVERY host emission is guarded by CMP dword ptr [*(0x140acd3a0) + 0x774], 0x0 followed by JZ return, at 0x1400483d3 and at 0x14004837b. With that dword zero the whole render path executes and writes nothing, which is consistent with the runner observing zero graphics calls inside the tick.',
  open='UNKNOWN: whether FUN_140843eb0 executes inside the tick the runner calls. No caller of it appears in the offline disassembly, which means it is reached through a function-pointer table -- the known blind spot of that export. The test is a containment query on the whole-frame p-code trace, which logs executed PCs and needs no live process.';
RELATE finding:host_render_command_ring -> cites -> source:steam_disasm_cache_20260905 SET id='host_ring_disasm';

UPSERT finding:ctx_texture_page_attribution_corrected SET status='open', confidence='high', date='2026-09-05',
  title='CORRECTION: the 68,640 bytes per frame at ctx+0x100030 are the display-list PAYLOAD arena, not decoded texture pages',
  statement='SUPERGUN-LATENCY-BUDGET section 1.3 attributed ctx[0x100030..0x110c50), 68,640 B per frame, to FUN_140800620/640 with the role "decoded texture pages". That attribution was by WRITING FUNCTION, and the writing function is the CRT memcpy -- called by FUN_1408436a0 at 0x140843df0 to copy each display-list record into the arena. The captured frame settles it numerically: the payload cursor at ctx+0x1e0080 reads 68,640, and 0x100030 + 0x10C20 = 0x110C50, exactly the extent that had been attributed to texture decode.',
  lesson='Attribution by writing function names the memcpy, not the caller that owns the bytes. Any byte-attribution table built that way needs a second pass through the call site before a role is assigned to a region.';
RELATE finding:ctx_texture_page_attribution_corrected -> cites -> source:ctx_out_display_list_parse SET id='ctx_tex_attr_parse';
UPSERT finding:ctx_texture_page_attribution_corrected SET status='confirmed';

UPSERT finding:push_tape_discards_the_live_roster SET status='open', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='The live HUD and sprite roster bug is push_tape dropping a header the pushed tape ALREADY carries -- the same shape as the aobjs bug fixed the same day',
  statement='RetroReceipts-agent/rr-render/src/feed.rs push_tape decodes the pushed chunk tape and consumes only frames, nodes, anodes, palrows and aobjs. It ignores that same tape p1_team, p2_team, assist, costume and stage_id fields. Those fields are NOT stale in the chunk: supergun/supersoaker/runner/sg_runner/src/live_tape.rs begin() calls rr_agent::harvest::read_match_start, whose team_ids[i] is read from fighter_i + 0x6C0 -- the exact byte that sg_runner --roster writes at main.rs:1268 -- and flush_chunk stamps them into every chunk through build_gamestate_record. The correct roster is therefore inside the bytes handed to push_tape sixty times a second and is thrown away, while Emitter::new keeps the SEED tape roster in pack.atlases (feed.rs:57), in resolve_palrow_slots (sprites.rs:291) and in WorldState::new through hud_portrait_pages (world.rs:263).',
  consequence='Deriving the roster from blk inside sg_runner would create a SECOND implementation of game-memory-to-tape-field, which is what the Gate 2 invariant recorded in sg_runner/Cargo.toml exists to forbid. Adopting the pushed tape own header keeps one derivation and fixes the browser live path at the same time.',
  open='Two MORE whole-tape properties are inherited from the seed and are NOT closed by a roster fix: world_on latches on a non-empty anodes table at open (feed.rs:61), and unknown_slots (sprites.rs:284) is an accumulator over the seed tape nodes that push_live_row never recomputes, even though it does update bank_slot from the pushed rows.';
RELATE finding:push_tape_discards_the_live_roster -> cites -> source:steam_disasm_cache_20260905 SET id='push_tape_roster_code';
