-- finding:body_walker_y_anchor RESOLVED 2026-06-12 (feat/render-replica-experiment)
-- The last render-replica term derived from CODE, byte-exact with ZERO ground-truth pinning.
USE NS re DB kb;

UPDATE finding:body_walker_y_anchor SET
  status = "confirmed",
  confidence = "high",
  date = "2026-06-12",
  statement = "BODY WALKER Y-ANCHOR — RESOLVED, code-derived, byte-exact (NO ground-truth pinning). " +
    "CLOSED FORM (per body part, simple+scale path facing=0/flags=0): " +
    "screenY = baseY + scaleY * (s16)(penY_acc + m*pitchY) ; where " +
    "baseY = leaf_e460(node+0xE4) + scaleY*(s16)node[0x136]  (leaf_e460 = floor-toward-neg-inf via the ftrc/0x4b000000-magic leaf bank11 loc_8C11E460, called at loc_8c0344d4 entry on the node+0x104==0 path; node[0x136]=Y hotspot, =0 for this object); " +
    "scaleY = node+0xF0 (=15/7 here); penY_acc = cumulative pen, Y-acc -= record.dy per record (finding:cumulative_pen_geometry); " +
    "m*pitchY = the per-TILE tile-height term = r5 = A*desc[3] - slot20  (A=m=descriptor byte0, desc[3]=pitchY, slot20=0 unless 0x4000 X-mirror), computed in loc_8c03478e. " +
    "The walker's screenY (logged at PC 0x8C034864) is the part's BOTTOM-LEFT anchor (MVC2 bottom-up tile anchoring); the engine submit loc_8c1244b0 lays the TA quad UPWARD so vertex A(top-left).Y = screenY - tileHeight, D(bottom-left).Y = screenY (CONFIRMED: engine TA sprite maxY == ASMTRACE screenY, 0.004px). " +
    "ROOT CAUSE of the old +tile_height*scaleY-per-record error: the harness build_image_dump.py fabricated baseY by a regression that integerized (sY-baseY)/scaleY (snapped to floor(node+0xE4)=333) AND zeroed node+0x136 — the integer-index solve aliased the true origin by exactly m*pitchY*scaleY (the dominant tile term). gen_walker.c (the transpiled walker) was ALREADY byte-exact; the fix was purely (a) feed the REAL f32 node+0xE0/E4/EC/F0 + real node+0x134/0x136 from the RAM dump and let the walker's own leaf_e460 floor + hotspot fmac derive the origin (no regression), and (b) lay TA corners upward in test_ta_emit.c to match the engine's bottom-anchor convention. " +
    "VALIDATION (raw walker, NO external Y-fix): per-part screenY vs ASMTRACE _ryu_capture/asm_angled_fist.log cid23 frame10766 = MAX dX 0.0033px / dY 0.0043px (within the trace's 0.01px log quantization); converge_byte_exact.mjs = 9/9 corners byte-exact vs the engine's own TA (mc_engine_ta.bin) 0.0000px; render+pixel-diff (render_ta.mjs -> diff_png.mjs) RAW ta_walker.bin vs engine GT = 100.0000% / 0 diff px / maxΔ=0. " +
    "CITE: marvelous2 bank03 loc_8c0344d4 (10271-10314 hotspot fmac origin; 10616-10683 per-tile m*pitch; entry leaf floor) + bank11 loc_8C11E460 (floor leaf) + bank12 loc_8c1244b0 (TA submit/quad layout). Tools: tools/render-replica-poc/{gen_walker.c (unchanged), build_image_dump.py, test_ta_emit.c, converge_byte_exact.mjs}."
WHERE id = finding:body_walker_y_anchor;

-- source: the validation run
UPSERT source:render_replica_y_anchor_20260612 SET
  kind = "validation",
  ref = "tools/render-replica-poc converge_byte_exact.mjs + test_ta_emit.c RAW-WALKER gate (feat/render-replica-experiment)",
  note = "Raw transpiled walker (loc_8c0344d4) per-part screenY byte-exact vs ASMTRACE (dY 0.0043px) and 100.0000% pixel match vs engine TA, code-derived Y origin, no ground-truth pinning. 2026-06-12.",
  date = "2026-06-12";

RELATE finding:body_walker_y_anchor->cites->source:render_replica_y_anchor_20260612;
RELATE finding:body_walker_y_anchor->cites->routine:loc_8c0344d4;
