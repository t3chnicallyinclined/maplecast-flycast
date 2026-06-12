USE NS re DB kb;

-- ============================================================================
-- RENDER-REPLICA / OPTION C (AOT lift-to-C transpile of the render call-tree)
-- 2026-06-12. The strategic pivot: instead of re-implementing each engine render
-- rule in JS (assembly/facing/blend/tiling/palette whack-a-mole), AOT-transpile
-- MVC2's REAL render SH4 code -> C -> wasm, run it client-side over a frozen
-- snapshot + per-frame GSTA patches, emit the game's own TA stream, feed the
-- existing pvr2-renderer (WebGPU). See docs/RENDER-REPLICA-PLAN.md.
-- ============================================================================

UPSERT finding:render_replica_option_c_validated CONTENT {
  id: finding:render_replica_option_c_validated,
  statement: "OPTION C PROVEN (PoC GO, 2026-06-12, commit 050c4a01b, tools/render-replica-poc/). A lift-to-C transpiler (lift.py parse + codegen.py ~45-opcode table harvested from flycast core/hw/sh4/interpr/sh4_opcodes.cpp + emit_func.py delay-slot-aware control flow) mechanically translates MVC2's real per-frame body-render SH4 -> native C, bit-exact. RESULTS: leaf loc_8c11e460 (bank11 vertex builder, 24 insns) = 21/21 BIT-EXACT; transform core loc_8c0347c8..loc_8c034864 (per-tile FP+integer screen math) = 18/18 BIT-EXACT X+Y (scaleX=5/3 baseX=533, scaleY=15/7 baseY=116.57, residual 0.000px, within ASMTRACE's own 0.01px log quantization); full walker loc_8c0344d4 (464 insns/20 BBs, both paths, all loops+4 leaf-dispatch sites) compiles/runs/balanced-stack (r15 delta=0). FP-EXACTNESS RISK CLOSED: straight C float is bit-exact for the +/-/* transform chain (compiled /fp:precise, RM=0, single precision throughout per the §8 opcode analysis). The ONE opcode needing special-casing = fmac -> std::fmaf (fused, single rounding; flycast sh4_fpu.cpp:559; naive fr0*frm+frn rounds twice and diverges; 5 sites in the walker). Integer subtleties handled: 8-bit imm sign-ext (load-bearing: add 0xE4,r0 = +(-28) derives node+0x144 sprite_id from node+0x160), sign-ext mov.w/mov.b loads, ftrc 0x7fffff80 overflow clamp, exts.w before float, muls.w 16x16->32. GROUND TRUTH = _ryu_capture/asm_angled_fist.log (ASMTRACE logs loc_8c0344d4's per-part screenX/screenY AT PC 0x8C034864 = the exact output of the transpiled fn). OPEN: full NUMERIC walker run needs the 0x8C1F9F9C tile-descriptor table (load-time built, NOT on disc) + GFX1 part-dim bytes from a 16MB RAM dump (absent from repo) -- a one-time Oracle memcpy of those static regions closes it. Lifter GENERALIZES to the full 9850-insn tree: add ftrv/frchg/fsca/fschg, wire the 2 ROM jump tables + 1 RAM vtable as switches, transpile trig leaves loc_8c11e2e0/e860 + bank12 submit.",
  status: "confirmed",
  confidence: "high",
  date: "2026-06-12"
};

UPSERT finding:render_calltree_scope CONTENT {
  id: finding:render_calltree_scope,
  statement: "The MVC2 per-frame RENDER call-tree (root loc_8c0308c2 Render_sprites slot-walk; loc_8c0305d8 is a sibling setup entry) is BOUNDED: 114 functions / 842 basic blocks / ~9850 SH4 instructions across banks 02/03/11/12/13/17 = ~3% of the game. 85 distinct opcodes; FPU-heavy (ftrv x20 = the world->screen matrix*vec transform core in bank12 loc_8c1216c0/loc_8c1219b0; frchg x12 + fschg x22 = FPSCR.FR/SZ bank+size state MUST be modeled; fsca x3 sin/cos table; fmac x14). SINGLE-PRECISION throughout (1 isolated fcnvsd). ZERO privileged/MMU/trap/SMC/MMIO instructions (no trapa/sleep/ldtlb/movca/ldc/rte/fipr/fmtrx) -- clean user-mode FP+integer. INDIRECT BRANCHES ~96% statically resolved: 253 jsr/jmp via ROM pool constants (trivial def-use), 2 enumerable ROM jump tables (the cell-processor Duff device loc_8c1295bc=loc_8c12951c with 17 targets bounds-checked cmp/hs 0x40; the render-mode braf_8c0310ac switch with 8 targets), and only 1 TRUE RAM vtable (loc_8c0305ce: jmp @[obj+0x28], a per-object render-override method ptr; ~5 store sites in bank03; handle via one-site switch(pc) escape hatch). No general indirect-jump resolver needed. CORRECTION to a prior plan assumption: the bank11/bank12 leaf bodies (loc_8c1216c0, loc_8c11e2e0/e460/e860, loc_8c1244b0) are PRESENT in the marvelous2 checkout (earlier greps missed UPPERCASE loc_8C... labels) -- ZERO functions referenced-but-missing. The GFX1 LZSS/part decoder loc_8c033d78 is NOT in the render tree (load-time only) so the AOT tree auto-excludes it.",
  status: "confirmed",
  confidence: "high",
  date: "2026-06-12"
};

UPSERT finding:body_walker_tiling CONTENT {
  id: finding:body_walker_tiling,
  statement: "STRETCHED-LIMB TILING (root cause of Sentinel rocket forearm too-short/flat; affects Dhalsim limbs, beams roster-wide). The body assembler loc_8c0344d4 walks TWO parallel streams: r11 = the 8-byte GFX2 cell records [dx s16][dy s16][FLAGS u16][sel u16] (advanced +8 at the outer boundary), AND r13 = a SEPARATE 4-byte-per-tile descriptor stream based at global 0x8C1F9F9C (r13 = *(player+0xDC)*4 + 0x8C1F9F9C, bank03:10247-10253; advanced +4 every inner iteration at the loop back-branch delay-slot, bank03:10760). The INNER tile/repeat loop is loc_8c03478e (bank03:10616); REPEAT COUNT = r13[1]+1 (loc_8c0345c4, bank03:10357-10361 mov.b @(0x1,r13),r0; add 0x01,r0); STEP per tile = (m*r13[2], m*r13[3]) where m=r13[0] (bank03:10619-10638 muls.w), screen = node+0xE0/E4 + (local_pen+step)*node_scale(0xEC/F0). The main cumulative pen (acc) advances by the record's dx/dy ONCE per record (NOT x count); tiles touch only the local pen, so acc stays constant across a record's tiles. FLAGS 0x4000=X-mirror / 0x8000=Y-mirror (NOT tiling); 0x10/0x20 = list-type select bits. The 0x8C1F9F9C table is built at LOAD time (not on disc) so the off-SH4 atlas/transpiler must dump it from RAM. EVIDENCE: ASMTRACE _ryu_capture/asm_angled_fist.log cid52 sid 0x131 = 9 raw GFX2 records expanded to 19 emitted parts (sel 2334 x4, 2394 x4, 2380 x4, 2382 x2, rest x1), tile count fixed-per-sprite_id.",
  status: "confirmed",
  confidence: "high",
  date: "2026-06-12"
};

UPSERT routine:loc_8c0308c2 CONTENT {
  id: routine:loc_8c0308c2,
  name: "Render_sprites",
  bank: "bank03",
  addr: "0x8C0308C2",
  role: "THE per-frame render ROOT / slot-table walk. Loads slot table 0x8C2895E0 (16 layers, stride 0x180) + node-ptr array 0x8C287DE0 (x4), per node reads category@+0x3, dispatches body loc_8c03093c (cat!=0) or effect/satellite loc_8c030af8 (cat==0). The render-replica entry point. Smallest hermetic subtree that emits the per-object display lists. Terminated by rts to pr (sentinel-pr driver stops cleanly).",
  status: "confirmed",
  date: "2026-06-12"
};

UPSERT routine:loc_8c11e460 CONTENT {
  id: routine:loc_8c11e460,
  name: "vertex_builder_leaf",
  bank: "bank11",
  addr: "0x8C11E460",
  role: "Leaf vertex builder called by loc_8c0344d4 (jsr). 24 insns. First fully-transpiled-to-C function (Option C PoC), 21/21 bit-exact.",
  status: "confirmed",
  date: "2026-06-12"
};

UPSERT source:render_replica_poc_20260612 CONTENT {
  id: source:render_replica_poc_20260612,
  ref: "tools/render-replica-poc/ (commit 050c4a01b, branch feat/render-replica-experiment)",
  note: "Option C lift-to-C PoC: lift.py/codegen.py/emit_func.py transpiler + harness + run.cmd. Diffs transpiled-C output vs ASMTRACE ground truth. RESULT GO: leaf 21/21 + transform-core 18/18 bit-exact; FP risk closed (fmac->fmaf only special case)."
};

RELATE finding:render_replica_option_c_validated->cites->source:render_replica_poc_20260612;
RELATE finding:render_replica_option_c_validated->cites->routine:loc_8c0344d4;
RELATE finding:render_replica_option_c_validated->cites->routine:loc_8c11e460;
RELATE finding:render_replica_option_c_validated->cites->finding:render_calltree_scope;
RELATE finding:render_calltree_scope->cites->routine:loc_8c0308c2;
RELATE finding:body_walker_tiling->cites->routine:loc_8c0344d4;
RELATE finding:body_walker_tiling->cites->source:render_replica_poc_20260612;
