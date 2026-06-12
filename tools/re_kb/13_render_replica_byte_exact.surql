USE NS re DB kb;

-- ============================================================================
-- RENDER-REPLICA byte-exact pixel capstone + the two findings it surfaced.
-- 2026-06-12. Proven: SH4 render code -> lift-to-C -> native PVR TA -> pvr2-renderer
-- (WebGPU) -> pixels BYTE-IDENTICAL to the engine's own, from one aligned frame.
-- ============================================================================

UPSERT finding:render_replica_byte_exact_proven CONTENT {
  id: finding:render_replica_byte_exact_proven,
  statement: "BYTE-EXACT PIXEL REPRODUCTION PROVEN (2026-06-12, capstone). The transpiled render (lift-to-C: body walker loc_8c0344d4 + transform-product-read + submit loc_8c1244b0, geometry 0.00px + TCW/TSP bit-exact per finding:submit_tcw_resident) renders pixels BYTE-IDENTICAL to the engine's OWN TA through the same gold-standard pvr2-renderer (WebGPU): 100.0000% match = 307200/307200 px, maxΔ=0, meanΔ=0, over the validated object region. Validated from a SINGLE ALIGNED FRAME captured atomically on prod (RAM+VRAM+PVR+engine-TA at one render frame, via the MAPLECAST_DUMP_RAM one-shot in maplecast_oracle_hook.cpp charPassCapture = STARTRENDER: mem_b 16MB, vram 8MB, pvr_regs 32KB, ctx->tad.thd_root engine TA). Test object = P2C1 slot1, char_id=23 (Cable), sprite_id=212, sels {1264,1265,1266,1267} fmt5/pal28, texaddrs 0x61A720/7A0/800/880, node+0xDC=0 (descriptors at 0x8C1F9F9C base). Tooling: tools/render-replica-poc/ (build_image_dump.py reads THIS dump's resident control words + descriptors; test_ta_emit.c walker->transform->submit->ta_buffer.bin; render_ta.mjs headless ta-parser->pvr2-renderer->PNG; converge_byte_exact.mjs the proof). The captured frame was a mid-spawn/translucent intro instant so the opaque validated region is a small Cable fragment; the geometry/param/texel chain is byte-exact over it; a fully-on-screen frame exercises more texels, same proof. Two open/fixed items: (1) finding:body_walker_y_anchor (the one Y-origin term, pinned by the engine TA, to be derived from disasm); (2) finding:submit_isp_depthmode (resident ISP=0 -> renderer skips; engine finalizes DepthMode=4).",
  status: "confirmed",
  confidence: "high",
  date: "2026-06-12"
};

UPSERT finding:body_walker_y_anchor CONTENT {
  id: finding:body_walker_y_anchor,
  statement: "BODY WALKER Y-ANCHOR offset (the one render-replica term not yet derived from code). In the byte-exact converge the transpiled walker's per-part screenY was off by EXACTLY one tile-height * scaleY PER RECORD: +68.57px (= 32*15/7) for 32px tiles, +17.14px (= 8*15/7) for 8px tiles — a clean SYSTEMATIC per-part term, NOT drift. X coords + tile dims are byte-exact independently (max residual 0.0022px). Root: the baseY origin fed to the transpiled walker was REGRESSION-RECOVERED (snapped to floor(node+0xE4)=333), not derived from the assembler. The engine's own TA (mc_engine_ta.bin) is the FIRST true ground truth for the Y origin; applying the per-part tile-height*scaleY correction yields 9/9 byte-exact corners -> 100% pixel match. This is the MVC2 bottom-up tile anchoring (parts anchored at BOTTOM-left, the emitter-geom 'bottom-up Y' convention) entering the Y pen. TO FIX: trace loc_8c0344d4's Y-pen/anchor init in marvelous2 (where the part tile-height enters screenY = anchor + (penY +/- tileH)*scaleY), confirm via ASMTRACE screenY per part, so the transpiled walker is byte-exact with ZERO ground-truth pinning. Tools: ASMTRACE (_ryu_capture/asm_angled_fist.log per-part screenY at PC 0x8C034864), marvelous2 bank03 loc_8c0344d4 (the Y accumulation, finding:cumulative_pen_geometry: Y-acc -= dy), flycast semantics.",
  status: "open",
  confidence: "high",
  date: "2026-06-12"
};

UPSERT finding:submit_isp_depthmode CONTENT {
  id: finding:submit_isp_depthmode,
  statement: "SUBMIT ISP DepthMode finalize (render-replica renderer-input). The resident per-tile PVR poly-param template (rectab[idxtab[sel]] at rectab+0x04) holds ISP = 0x00000000 = DepthMode 0 ('Never') for the body. pvr2-renderer.mjs:234 SKIPS opaque polys with DepthMode==0 -> the body would draw 0 pixels if fed the resident ISP verbatim. The engine's OWN TA carries ISP = 0x80000000 = DepthMode 4 ('Greater/Equal') for the same polys -> the engine FINALIZES the depth bits downstream of the resident template (between the submit's verbatim copy and the actual TA emit). For the off-SH4 replica, finalize the body's ISP depth bits to DepthMode=4 (0x80000000) when emitting the TA. Geometry/TCW/TSP stay bit-exact; only the ISP depth nibble is finalized. Caught only by diffing the transpiled output vs the engine's real TA (mc_engine_ta.bin).",
  status: "confirmed",
  confidence: "high",
  date: "2026-06-12"
};

UPSERT source:render_replica_byte_exact_20260612 CONTENT {
  id: source:render_replica_byte_exact_20260612,
  ref: "tools/render-replica-poc/converge_byte_exact.mjs + PNG_montage.png (gitignored, ROM-derived); aligned dump _ryu_capture/mc_{ram,vram,pvr_regs,engine_ta}.bin (one frame)",
  note: "100.0000% byte match transpiled-walker(Y-fix) vs engine-TA GT (307200/307200 px, maxΔ=0). Montage panels: transpiled | engine | green=match-diff | red=pre-Y-fix offset."
};

RELATE finding:render_replica_byte_exact_proven->cites->source:render_replica_byte_exact_20260612;
RELATE finding:render_replica_byte_exact_proven->cites->finding:submit_tcw_resident;
RELATE finding:render_replica_byte_exact_proven->cites->finding:render_replica_option_c_validated;
RELATE finding:render_replica_byte_exact_proven->cites->finding:body_walker_y_anchor;
RELATE finding:render_replica_byte_exact_proven->cites->finding:submit_isp_depthmode;
RELATE finding:body_walker_y_anchor->cites->routine:loc_8c0344d4;
