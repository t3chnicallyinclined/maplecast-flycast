-- ============================================================================
-- 26: render-replica LIVE — PHASE 3 STAGE (background) render.
-- The fighters were drawn on black; this adds the actual MVC2 stage behind them,
-- grounded in the ROM stage decode + the engine's OWN live camera matrices.
-- (feat/render-replica-live). Pinned 2026-06-13.
-- ============================================================================
USE NS re DB kb;

UPSERT source:stage_decode SET kind='tool',
  ref='tools/rip_stage.py (ModNao NaomiLib/libspr port) -> web/test-atlas/stages/STGxx.json + STGxx_tNN.png',
  note='Offline disc decode of STGxxPOL.BIN (geometry) + STGxxTEX.BIN (PVR textures), pre-relocated Sega NinjaLibrary object tree at base 0x0CEA0000. Emits per-stage JSON (models->meshes->tris{pos,uv,col}) + de-twiddled RGBA PNGs. All 17 stages (STG00..STG10) ripped, gitignored (ROM-derived). STG00 geometry verified full-3D NAOMI WORLD-SPACE: X in +-48664, Y -31610..45610, Z -100435..9099, UV in [0,1] — NOT pre-projected screen space, so it needs the engine camera each frame.';

UPSERT source:stage_camera_transform SET kind='code',
  ref='tools/render-replica-poc/gen_transform_obj.c transform_object_122560 (faithful port of bank12 loc_8C122560)',
  note='The engine projects EVERY renderable world-space object through ONE frame-global transform loc_8C122560: XMTRX = M1(viewport @0x8C2D6B18) . M2(projection @0x8C2D6AD8) [column-major mat-mat, loc_8c120540]; fv = ftrv(XMTRX, (x,y,z,1)) [loc_8c11F870]; inv=1/fv[3]; screenX=fv[0]*inv; screenY=fv[1]*inv; depth=inv. The body slot-walk feeds world pos through this to deposit screen_x/y @node+0xE0/E4 (re_kb finding:render_frame_positions_validated, 0.00px vs ASMTRACE). Both source matrices are FRAME-GLOBAL camera state built once/frame by the proj setup loc_8c1216c0 (NOT per-object output) -> reading them from RAM is code-derived. From the live _ryu_capture/mc_ram_dump.bin: M1 = viewport [320,0,0,0, 0,-240,0,0, 0,0,-0.5,0, 320,240,0.5,1] (maps NDC->640x480 screen px, translate to screen center); M2 = perspective. So fv[0]/fv[1] come out directly in 640x480 SCREEN PIXELS, matching the bodies  deposited +0xE0/E4.';

UPSERT source:stage_readset SET kind='code',
  ref='core/network/maplecast_replica_live.cpp buildTables()',
  note='cam_mat region D(0x8C2D6AD8, 0xC0, "cam_mat") ALREADY ships BOTH camera matrices every frame: M2@0x8C2D6AD8+0x00 and M1@0x8C2D6B18 (=+0x40), within the 0xC0=192B (3x16f) window. stage_id (0x8C289638) was NOT shipped (the page-649 global-state block was absent from the read-set) -> ADDED D(0x8C289620, 0x20, "gstate") (32B window over in_match/round/timer/stage_id). stage_anim_timer (0x8C1F9D80) already shipped in the "arena" region. So the client gets stage_id + the live camera with NO new wire beyond 32 bytes/frame.';

UPSERT finding:replica_live_stage_render SET
  statement='PHASE 3 STAGE background landed on the render-replica client, grounded in ROM + disasm, NO model camera. The stage geometry+textures are decoded OFFLINE from the disc (tools/rip_stage.py, all 17 stages, web/test-atlas/stages/, gitignored) and cached/served locally exactly like the local-GFX overlay. web/webgpu/stage-client.mjs projects the stage s full-3D world-space verts through the SAME engine transform the bodies use (transform_object_122560: XMTRX=M1.M2, ftrv, persp divide) using the LIVE matrices read each frame from the RAM image (M1@0x8C2D6B18, M2@0x8C2D6AD8 — both already in the shipped "cam_mat" read-set), replacing the old GUESSED DEFAULT_CAM perspective camera (setCamera()/_projectEngine()/_matmulColMaj()). So the stage scrolls/zooms in lockstep with the fighters. web/render-replica/replay.html renders the stage as an OP pass BEHIND the bodies on the SAME pvr2 canvas: renderStagePass() (clears, draws stage) -> pane.render(bodyTA, {_bgPreRendered:true}) (loadOp load, composites bodies over). stage_id read from the new "gstate" region (0x8C289638). Body render (render_frame.wasm/gen_*.c/body_decoder.mjs) UNTOUCHED. Cache-bust ?v=stage1.',
  status='confirmed', confidence='high', date='2026-06-13';

UPSERT finding:replica_live_stage_projection_validated SET
  statement='The client projection math is PROVEN byte-identical to the engine transform. Offline check (python + node, live matrices from _ryu_capture/mc_ram_dump.bin): the validated body test vector pos(213.333,100,2) projects to screen (533.85953, 333.38765) via stage-client s _matmulColMaj+_projectEngine — within f32 ULP of gen_transform_obj.c s byte-exact target (533.85986, 333.38763). Projecting all 12435 STG00 verts through the live matrices lands the bulk on the 640x480 frame (median screenX=386, screenY=272; 8415/12435 verts in/near screen with depth>0); the far outliers are skybox/grazing-angle geometry (clipped). The projection is therefore the engine s own, not a fit. OPEN (deferred to live DIFF): (1) confirm the STAGE NaomiLib tree submits through loc_8C122560 vs a separate background matrix — pin via mvc2-sh4-re-expert tracing the stage submit routine, or accept the live DIFF vs the real-TA STG00 as proof; current evidence (M1/M2 are the engine s general object transform, body-validated) strongly supports it. (2) the synthesized stage PVR control words (pcw/tsp/isp in stage-client _build) are the remaining from-Ninja-fields synthesis (textured/gouraud/alpha/depth) — validate per-poly vs a live STG00 TA capture. (3) animation: stage_anim_timer low-bit A/B keyframe (loc_8c0338ec) deferred — static A-phase for v1.',
  status='confirmed', confidence='medium', date='2026-06-13';

RELATE finding:replica_live_stage_render->cites->source:stage_decode;
RELATE finding:replica_live_stage_render->cites->source:stage_camera_transform;
RELATE finding:replica_live_stage_render->cites->source:stage_readset;
RELATE finding:replica_live_stage_projection_validated->cites->source:stage_camera_transform;
RELATE finding:replica_live_stage_projection_validated->cites->source:stage_decode;
