USE NS re DB kb;

-- ============================================================================
-- Option C STAGE 2: walker -> transform(resident) -> submit-corners -> TA quads
-- (tools/render-replica-poc/ step [6/6]; test_ta_emit.c + gen_submit.py)
-- Seeded 2026-06-12. Re-apply with: tools/re_kb/rekb.sh @tools/re_kb/12_render_replica_ta_emit.surql
-- ============================================================================

UPSERT routine:loc_8c1216c0 SET label='loc_8c1216c0', bank='bank12', pc='0x8C1216C0',
  role='world->screen transform',
  note='World->screen matrix transform (ftrv/fsca/frchg tree). Runs ONCE PER OBJECT upstream of the body walker; deposits its result into node+0xE0/E4 (screen anchor) and node+0xEC/F0 (per-axis scale). CONFIRMED from RAM dump: node+0xE0=533.86, node+0xEC=5/3, node+0xF0=15/7 are byte-exact the baseX/scaleX/scaleY the walker consumes. Need NOT be re-run to reproduce a resident object quads; its product is a node field read by walker loc_8c0344d4 at loc_8c0347c8.';

UPSERT routine:loc_8c124ab0 SET label='loc_8C124AB0', bank='bank12', pc='0x8C124AB0',
  role='submit corner-transform',
  note='Submit corner-transform: 2x2 rotation+scale of 4 corner-offset pairs around the screen anchor via fmac. fr8=scaleX*cos, fr5=scaleX*sin, fr4=scaleY*cos, fr9=scaleY*sin built by caller 9962-9966. Loop loc_8c124afa reads @r7+ offset pairs; out X += offsetX*fr8 + offsetY*fr9, out Y += offsetX*fr4 + offsetY*fr5. angle=0 (cos=1,sin=0) collapses to axis-aligned. TRANSPILED to gen_submit.c (71 insns), exercises fmac/shar/shad/and-imm/cmp-imm.';

UPSERT finding:render_replica_ta_emit SET
  status='confirmed', confidence='high', date='2026-06-12',
  statement='OPTION C STAGE 2 CLOSED (tools/render-replica-poc/ step [6/6], test_ta_emit.c + gen_submit.py). The lift-to-C chain now emits the GAME-NATIVE PVR TA command stream: WALKER loc_8c0344d4 (per-tile top-left screenX/Y, proven 9/9 @0.00px) -> TRANSFORM loc_8c1216c0 (RESIDENT: world->screen result is already node+0xE0/E4 anchor + node+0xEC/F0 scale; the RAM dump byte-exactly holds 533.86/5over3/15over7, so the 9850-insn ftrv tree need not be re-run for a resident object) -> SUBMIT loc_8C1244B0->loc_8C124AB0 (4 corners = anchor + R(angle).(scale*unit_offset); body angle=0 => axis-aligned, confirmed vs probe_body_uv.json all quads A.y==B.y B.x==C.x). RESULT cid23 frame 10766: 9 quads, corner extent ROM-exact (m=descriptor byte[0]; W=m*scaleX H=m*scaleY; maxerr=0.0000), written to ta_buffer.bin (1472B/46 params) and ROUND-TRIPPED through the real web/webgpu/ta-parser.mjs (verify_ta.mjs) -> 9 opaque textured polys / 36 vertices, corners byte-exact. NEW codegen.py opcodes for the matrix/submit core: ftrv (4x4 XF-bank*FV column-major, xf[16] separate from fr[16]), frchg (FR<->XF + FPSCR.FR), fschg (FPSCR.SZ), fsca (sin/cos from 16-bit angle), shad/shar/shll16/shll8/shlr16/shlr8/shlr2/shlr, xor/not/cmp-hs/cmp-hi/pref, sign-ext cmp/eq #imm, fcnvsd/ds identity. FP/bank subtlety: ftrv reads the XF (secondary) bank frchg makes active for the projection matrix; body path never flips FR/SZ so the walker+corner chain needs no bank routing. TA buffer = standard LE PowerVR2 32B params: Polygon-param(pt4 PCW/ISP/TSP/TCW)+4 Vertex-params(pt7 x/y/z/u/v/col, strip TL/TR/BL/BR, last=EndOfStrip)+EndOfList(pt0). x/y=final screen px (exact), z=1; tcw=sel-keyed PLACEHOLDER (no GFX1 pixels in dump; body sels pixels from CHARQ/BODYCAP) and u/v unit[0,1] (harness applies real per-sel UV from probe_body_uv.json) -- geometry exact, tex-binding is the documented seam. NOT deployed (offline harness only). OPEN: (1) full ftrv matrix tree loc_8c1216c0 transpile for objects whose node+0xE0/E4 is not dump-resident; (2) submit tcw/tsp/pcw bit-assembly (loc_8c1246da..loc_8c1248ea) transpile for real PVR control words.';

RELATE finding:render_replica_ta_emit->about->routine:loc_8c1244b0;
RELATE finding:render_replica_ta_emit->about->routine:loc_8c1216c0;
RELATE finding:render_replica_ta_emit->about->routine:loc_8c124ab0;
