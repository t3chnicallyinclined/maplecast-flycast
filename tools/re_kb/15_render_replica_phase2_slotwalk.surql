USE NS re DB kb;

-- ============================================================================
-- RENDER-REPLICA PHASE 2 — transpile the root slot-table walk (loc_8c0308c2 =
-- "Render_sprites") so the off-SH4 render iterates ALL on-screen BODY objects,
-- and CLOSE the Phase-1 residual: the per-object submit RECORD-ALLOCATION base is
-- now COMPUTED by running the engine's own allocation cursor (the node+0xDC
-- prefix-sum), not RAM-discovered. 2026-06-12, branch feat/render-replica-experiment.
-- ============================================================================

-- The root slot-walk routine.
UPSERT routine:loc_8c0308c2 CONTENT {
  id: routine:loc_8c0308c2,
  pc: "0x8c0308c2",
  bank: "bank03",
  role: "render-root",
  note: "Render_sprites (bank03:1200) = the per-frame slot-table walk = the RENDER ROOT. Walks 16 layers: count u8 @ 0x8C2895E0+L (loop end r8=base+0x10), node ptrs u32 @ 0x8C287DE0 + L*0x180 + i*4 (absolute node bases). Per node reads category byte @+0x3: cat==0 -> bsr loc_8c03093c (BODY / Render Main Sprite); cat!=0 -> bsr loc_8c030af8 (EFFECT, cat 1..4). Outer loop ascends layers 0->15 = draw order (lower layer = behind). Transpiled faithfully (gen_walker_root.c) for the off-SH4 render-replica Phase 2."
};

-- The submit allocation cursor (the load-bearing Phase-2 piece).
UPSERT address:submit_arena_base CONTENT {
  id: address:submit_arena_base,
  addr: "0x8C1F9D94",
  kind: "render-alloc-cursor",
  note: "Per-frame submit-allocation ARENA BASE for the BODY render path. *(0x8C1F9D94) = 16 or 400 (ping-pong by frame parity, set in the frame-setup loc_8c033950 bank03:8460-8469: even branch -> 16, odd -> 400; the other base lives at 0x8C1F9D90). This is the START index into idxtab for the frame's FIRST body object. The body render walker loc_8c0344d4 (bank03:10325-10330) computes the per-tile alloc index *r13 = node+0xDC + *(0x8C1F9D94) + k."
};
UPSERT address:submit_obj_cursor CONTENT {
  id: address:submit_obj_cursor,
  addr: "0x8C1F9D98",
  kind: "render-alloc-cursor",
  note: "Per-frame RUNNING prefix-sum cursor. Each body object's setup loc_8c033b0a (bank03:8751-8772) deposits node+0xDC = *(0x8C1F9D98) (snapshot), then per GFX2 record advances *(0x8C1F9D98) += record_tile_count at loc_8c033d44 (bank03:8057-8059: r3=*r11; r3+=r9; *r11=r3, r11=*(0x8C1F9D98), r9=count). So across the frame node+0xDC = PREFIX-SUM of all prior bodies' tile counts. The 0x8C2DEE54 cursor in loc_8C124910 (bank12) is the same mechanism for OTHER (non-body) submit paths."
};

UPSERT finding:render_replica_phase2_slotwalk CONTENT {
  id: finding:render_replica_phase2_slotwalk,
  statement: "RENDER-REPLICA PHASE 2 DONE (2026-06-12, branch feat/render-replica-experiment, tools/render-replica-poc/). The render ROOT loc_8c0308c2 (Render_sprites, bank03:1200) is transpiled (gen_walker_root.c -> render_sprites_0308c2), so render_frame(ram16mb) walks the ENTIRE slot table and renders EVERY on-screen BODY object via the Phase-1 render_object_full chain. SLOT-WALK (byte-exact hand-port, every line cites a bank03 disasm line): 16 layers; count u8 @ 0x8C2895E0+L (loop end r8=base+0x10, bank03:1214); node ptrs u32 @ 0x8C287DE0 + L*0x180 + i*4 (bank03:1213/1225-1226); per node category byte @+0x3 (bank03:1227). *** BRANCH TRUTH (corrects the task framing): tst r0,r0; bf -> cat!=0 = EFFECT (loc_8c030af8); FALL-THROUGH cat==0 = BODY (loc_8c03093c). So category==0 => BODY, category!=0 => EFFECT (confirmed vs loc_8c030af8 itself which re-reads +0x3 and only processes 0<cat<5, bank03:1538-1552; and re-catalog 00-README category@+0x3). The +0x12C visibility/cull gate lives INSIDE loc_8c03093c (bank03:1287-1290: read +0x12C; if 0 -> bra skip), honored by render_object_full (which runs render_object_setup_03093c first). CURSOR-ALLOCATION CLOSURE (the load-bearing Phase-2 piece, closes Phase-1 open-seam a): the per-object rectab base is COMPUTED by running the engine's own allocation cursor, NOT RAM-discovered. The chain (3 confirming disasm sites): arena base *(0x8C1F9D94)=16|400 (frame parity, frame-setup loc_8c033950 bank03:8460-8469); per-object setup loc_8c033b0a deposits node+0xDC = *(0x8C1F9D98) (the running prefix cursor, bank03:8751-8772) then advances *(0x8C1F9D98) += record_tile_count (loc_8c033d44 bank03:8057-8059); the body render walker loc_8c0344d4 computes the per-tile alloc index *r13 = node+0xDC + *(0x8C1F9D94) + k (bank03:10325-10330, +1/tile at 10752); the submit loc_8C1244B0 maps rectab[idxtab[*r13]] (bank12:9853-9874, the direct idxtab[*r13] path; loc_8c034944 pool confirms the walker's submit = loc_8C1244B0). => for body object n, tile k: idxtab_index = prefix_sum(n) + arena_base + k where prefix_sum(n) = sum of ntiles(0..n-1). render_frame keeps a running_cursor (seeded 0, advanced by each object's emitted tile count in slot-walk order) and ASSERTS computed running_cursor == the engine's resident node+0xDC per object -- the proof the cursor generalizes Phase-1's single RAM-discovered base. VALIDATION (cid23/P2C1 Cable, the only body in the matched dump's slot table, L06): (1) the transpiled slot-walk enumerated the body and rendered 9 tiles; (2) computed running-prefix (0) == engine resident node+0xDC (0) OK; (3) PCW/ISP/TSP/TCW 9/9 byte-exact vs the engine TA; (4) corners 9/9 at 0.0000px vs the engine TA; (5) rendered through the gold-standard pvr2-renderer = 100.0000% pixel match (307200/307200, 0 diff px, maxDelta=0) vs the engine bodies. MULTI-OBJECT scope: the matched dump (mc_ram_dump.bin/mc_engine_ta.bin) is a SINGLE-character-on-screen frame -- only cid23 is in the slot table (the other 5 slots inactive) and the engine TA has only 9 pal-24 sprites, so the byte-exact diff covers ONE real body; the multi-object cursor advance is proven by (a) computed prefix == resident node+0xDC, and (b) a SYNTHETIC 2-body test (render_frame_test.c) confirming object 2's base = object 1's base + ntiles(object1) (arena=16: baseA=16, baseB=25, final cursor=14 for 9+5) per the ROM formula. The verification: for cid23 node+0xDC=0, arena=16, *r13[k]=16+k, idxtab[16..24]=9..17 (contiguous), rectab[9..17] all pal24/fmt5 = EXACTLY Phase-1's RAM-discovered base=9 -- now derived by following node+0xDC + cursor + idxtab indirection. WASM: render_frame.wasm/render_frame.mjs (build_wasm_frame.sh, emcc) exports render_frame_ta(ram16mb,out_ta,cap)/render_frame_body_count()/render_frame_quad_count(); fed the LIVE 16MB RAM dump (NOT a baked image) it enumerates the body via the slot-walk and emits 9 body tiles, params 9/9 + corners 9/9 == native (verify_frame_wasm.mjs). web/render-replica/index.html updated to call render_frame (whole-scene). LEFT FOR PHASE 3: the effect path loc_8c030af8 (cat 1..4 satellites: capes/projectiles/effects, stubbed render_effect_030af8) + HUD + stage.",
  status: "confirmed",
  confidence: "high",
  date: "2026-06-12"
};

UPSERT source:render_replica_phase2_20260612 CONTENT {
  id: source:render_replica_phase2_20260612,
  ref: "tools/render-replica-poc/{gen_walker_root.py->gen_walker_root.c, render_frame.c, build_image_frame.py->image_frame.h, render_frame_test.c, converge_frame.mjs, wasm_entry_frame.c, build_wasm_frame.sh, verify_frame_wasm.mjs}; web/render-replica/{render_frame.wasm,render_frame.mjs,index.html}; aligned dump _ryu_capture/mc_{ram,vram,pvr_regs,engine_ta}.bin (single-body frame, gitignored ROM-derived)",
  note: "render_frame_test.exe: slot-walk -> 1 BODY / 9 tiles; per-object CURSOR PROOF computed_prefix==resident node+0xDC OK; PCW/ISP/TSP/TCW 9/9 byte-exact; corners 9/9 @0.0000px; SYNTH 2-body baseB=25=16+9 OK. converge_frame.mjs: PARAM 9/9 -> render_ta.mjs+diff_png.mjs = 100.0000% pixel match vs engine bodies. verify_frame_wasm.mjs: render_frame.wasm (live 16MB RAM, NOT baked) == native 9/9 params + 9/9 corners. run.cmd step [8/8]."
};

RELATE finding:render_replica_phase2_slotwalk->cites->source:render_replica_phase2_20260612;
RELATE finding:render_replica_phase2_slotwalk->cites->finding:render_replica_phase1_codederived;
RELATE finding:render_replica_phase2_slotwalk->cites->finding:submit_rectab_alloc_index;
RELATE finding:render_replica_phase2_slotwalk->cites->finding:body_walker_tiling;
RELATE finding:render_replica_phase2_slotwalk->about->routine:loc_8c0308c2;
RELATE finding:render_replica_phase2_slotwalk->about->routine:loc_8c03093c;
RELATE finding:render_replica_phase2_slotwalk->about->routine:loc_8c0344d4;
RELATE finding:render_replica_phase2_slotwalk->about->address:submit_arena_base;
RELATE finding:render_replica_phase2_slotwalk->about->address:submit_obj_cursor;
RELATE routine:loc_8c0308c2->reads->global:slot_table_count;
RELATE routine:loc_8c0308c2->reads->global:slot_table_ptrs;
RELATE routine:loc_8c0308c2->calls->routine:loc_8c03093c;
