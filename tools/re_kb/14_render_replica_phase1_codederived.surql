USE NS re DB kb;

-- ============================================================================
-- RENDER-REPLICA PHASE 1 — the per-object body render made FULLY CODE-DERIVED.
-- 2026-06-12. Removes the snapshot/engine-TA PINNING of the per-frame deposited
-- state: the anchor (node+0xE0/E4), per-axis scale (node+0xEC/F0), and the
-- texture params (PCW/ISP/TSP/TCW) are now COMPUTED from the transpiled
-- loc_8c03093c + transform + submit-finalize, reading ONLY resident RAM (the raw
-- char struct + frame-global camera matrices + the resident rectab) -- ZERO bytes
-- read from the engine TA. Validated byte-exact + 100% pixel match.
-- ============================================================================

UPSERT finding:render_replica_phase1_codederived CONTENT {
  id: finding:render_replica_phase1_codederived,
  statement: "RENDER-REPLICA PHASE 1 DONE (2026-06-12, branch feat/render-replica-experiment, tools/render-replica-poc/). The per-object body render is now FULLY CODE-DERIVED -- the per-frame deposited state that was previously READ from the snapshot / pinned to the engine TA is COMPUTED. Three deposits transpiled + validated byte-exact vs the engine's OWN deposited values, ZERO engine-TA reads: (1) ANCHOR node+0xE0/E4 = the world->screen transform loc_8C122560 (bank12): ftrv of the composed XMTRX over node pos (0x34/38/3C) + perspective divide (inv=1/fv[3]; x*=inv; y*=inv). The composed XMTRX = M1.M2 where M1=viewport matrix @0x8C2D6B18, M2=projection @0x8C2D6AD8 -- FRAME-GLOBAL camera matrices (built once/frame by loc_8C1216C0, the proj-setup loc_8c03093c calls FIRST at bank03:1295 with fr4=640/fr5=480), legitimately resident (camera, not this object's render output). Reproduced node+0xE0=533.85986/node+0xE4=333.38766 byte-exact (engine 533.85986/333.38763, within f32 ULP). gen_transform_obj.c (hand-verified faithful port: load_mat + column-major ftrv matching codegen.py + matmul_colmaj M1.M2 + perspective divide; cites loc_8c120950 load / loc_8c120540 matmul / loc_8c11F870 ftrv / loc_8C122560 divide). (2) SCALE node+0xEC/F0 = loc_8c03093c (bank03:1336-1381): first scaleX = CpsXScale*node[0x50], scaleY = CpsYScale*node[0x54] (CpsXScale=0x3fd55555=5/3, CpsYScale=0x40092492=15/7, IMMEDIATES, work.asm:44-45), THEN divided by (0x444b16de / camZ) where camZ=*(0x8c26a538)=*(0x8c26a518+0x20)=812.357 (bank03:1369-1381). Final scaleX=1.666667/scaleY=2.142858 byte-exact. Transpiled via gen_render_object.py (loc_8c03093c, 180 insns, the jsr to loc_8c122560 routed to transform_object_122560). (3) TEXTURE PARAMS PCW/ISP/TSP/TCW = the submit finalize loc_8C1244B0 + loc_8C124910/124a82: read the RESIDENT 0x20-byte poly-param template rectab[idxtab[*rec]] (rectab=*(0x8C2DAD4c), idxtab=*(0x8C2DAD3c)), then finalize: PCW=(P&0xF8FCFFFF)|0x02000000 (loc_8c1246b0/124720); ISP=(I&0x1FFFFFFF)|(4<<29)=0x80000000 (DepthMode 4, loc_8c12467a); TSP=(T&0x03278FFF)|(((*(0x8C2AA4C4)^0xFC)<<24)|0x00100000|0x00800000)=0x949004D2 (loc_8c1246b0/124708); TCW=(T&0xF81FFFFF)|(palbank<<21) PalSelect inject (loc_8c124a82, palbank=24 = P2C1 slot formula 16*(0+1)+8*1). gen_submit_params.c. ALL 9 body tiles PCW/ISP/TSP/TCW byte-exact vs engine TA (test_render_object_full.exe). VALIDATION (cid23/P2C1 Cable sid212, frame10766, 9 body tiles): computed anchor/scale/params == engine's actual deposited values byte-exact; full pipeline render_object_setup_03093c -> transform -> walker(loc_8c0344d4, 9 tiles) -> submit_params -> sprite TA assembled with computed params+corners+UV == engine TA 864/864 sprite bytes IDENTICAL; rendered through the gold-standard pvr2-renderer (WebGPU) = 100.0000% pixel match (307200/307200, 0 diff px, maxΔ=0) vs the engine's own TA. NO PINNING: nothing read from mc_engine_ta.bin for any load-bearing field. SUPERSEDES the engine-TA-pinned texture-binding of finding:render_replica_byte_exact_proven (which kept the engine TA's param block). OPEN/transient seams honestly flagged: (a) the per-frame submit RECORD-ALLOCATION base index (rec_index 16..24 -> rectab[9..17]) is transient submit state, already consumed/reset (0x8C2DEE54=0) at the STARTRENDER dump instant; for THIS object it is RAM-DISCOVERABLE (the unique contiguous run of >=ntile pal24/fmt5 rectab entries == base 9) but the general per-frame cursor is not resident -- Phase 2 (the slot-walk) must run the submit's own allocation counter to derive it. (b) loc_8C1216C0 (the proj-matrix SETUP that BUILDS M1/M2 from camera params) is not yet transpiled (stubbed); M1/M2 read resident. Transpiling it would make the camera matrices themselves code-derived (only needed for objects whose camera state isn't in a dump).",
  status: "confirmed",
  confidence: "high",
  date: "2026-06-12"
};

-- The endianness correction that unblocked the sub-word reads.
UPSERT finding:render_replica_sh4_little_endian CONTENT {
  id: finding:render_replica_sh4_little_endian,
  statement: "MVC2's SH4 runs LITTLE-ENDIAN; the prod RAM dump (mem_b) is verbatim LE (a node+0x34 posX value reads as dump bytes 55555543 = LE 0x43555555 = 213.33). The render-replica PoC's sh4ctx.h was originally BIG-ENDIAN accessors + a byteswapped image -- a DOUBLE inversion that cancelled for 32/16-bit ALIGNED reads (so the walker passed) but CORRUPTED sub-word BYTE reads (node+0x12c entry guard, node+0x5d zoom guard, the descriptor bytes read via mov.b). Phase 1 needs those byte reads (loc_8c03093c's node+0x12c/0x5d guards), so sh4ctx.h was switched to LITTLE-ENDIAN accessors + a VERBATIM dump copy (build_image_dump.py be32/bef32 flipped to LE; build_image_full.py copies verbatim). Re-verified: the proven walker test (FULL-WALKER NUMERIC) stays 9/9 @ 0.00px with the negative control correctly failing; leaf 21/21, transform 18/18 unchanged. CITE: tools/render-replica-poc/sh4ctx.h, build_image_dump.py, build_image_full.py.",
  status: "confirmed",
  confidence: "high",
  date: "2026-06-12"
};

-- The texture-param indexing correction (the engine-TA pinning root cause).
UPSERT finding:submit_rectab_alloc_index CONTENT {
  id: finding:submit_rectab_alloc_index,
  statement: "SUBMIT TEXTURE-PARAM INDEXING -- CORRECTED. The body submit loc_8C1244B0 indexes rectab by *r13 = the per-tile ALLOCATION index the walker writes at stack[r15+0x2C], NOT by sprite_id/sel. The earlier build_image_dump.py used idxtab[sel*2] (e.g. sel1264 -> idxtab[1264]=986 -> rectab[986], pal=28, texaddr 0x61A720) which is the WRONG entry -- it decoded plausibly but did NOT match the engine TA (pal=24, texaddr 0x415400). That mismatch is exactly why the texture params were PINNED to mc_engine_ta.bin. The REAL mapping: the engine allocated this object's 9 tiles a CONTIGUOUS run rectab[9..17] (= idxtab[16..24]), already FULLY FINALIZED in the STARTRENDER dump (PCW=A2000009/ISP=80000000/TSP=949004D2/TCW=2B082A80.. all byte-exact == engine TA, pal=24 baked). Walker tile k (emission order) -> rectab[base+k] 1:1 (verified: bottom-left anchors byte-identical, X-corners 463/516/569). base=9 is RAM-discoverable as the unique contiguous run of >=ntile pal-bank/fmt5 rectab entries. So the texture params read straight from the resident rectab[base+k] -- no engine-TA needed. CITE: marvelous2 bank12 loc_8C124520 (r8=idxtab[*r13]) / loc_8C124534 (rectab+r8*0x20); bank03 loc_8c034864 (submit call, r4=r15+0x2C) / loc_8c034744 region (walker stack record). Supersedes the build_image_dump.py STALE-pointer comment.",
  status: "confirmed",
  confidence: "high",
  date: "2026-06-12"
};

UPSERT source:render_replica_phase1_20260612 CONTENT {
  id: source:render_replica_phase1_20260612,
  ref: "tools/render-replica-poc/{gen_render_object.py->gen_render_object.c, gen_transform_obj.c, gen_submit_params.c, build_image_full.py->image_full.h, test_render_object_full.c, converge_full_computed.mjs}; aligned dump _ryu_capture/mc_{ram,vram,pvr_regs,engine_ta}.bin (one frame, gitignored ROM-derived)",
  note: "test_render_object_full.exe: ANCHOR+SCALE computed byte-exact, PCW/ISP/TSP/TCW 9/9 byte-exact (NO engine-TA). converge_full_computed.mjs: computed sprite TA 864/864 bytes == engine TA -> render_ta.mjs+diff_png.mjs = 100.0000% pixel match (0 diff, maxΔ=0). run.cmd step [7/7]."
};

RELATE finding:render_replica_phase1_codederived->cites->source:render_replica_phase1_20260612;
RELATE finding:render_replica_phase1_codederived->cites->finding:submit_rectab_alloc_index;
RELATE finding:render_replica_phase1_codederived->cites->finding:render_replica_sh4_little_endian;
RELATE finding:render_replica_phase1_codederived->confirms->finding:render_replica_byte_exact_proven;
RELATE finding:render_replica_phase1_codederived->confirms->finding:submit_tcw_resident;
RELATE finding:render_replica_phase1_codederived->about->routine:loc_8c03093c;
RELATE finding:render_replica_phase1_codederived->about->routine:loc_8c1216c0;
