USE NS re DB kb;
UPSERT finding:gsta_sprite_id_sampling_phase CONTENT {
  id: finding:gsta_sprite_id_sampling_phase,
  statement: "ROOT CAUSE of 'wrong sprite during fast moves' (Sentinel rocket 213/214 vs diagonal 428-430; Storm dash; electric arc) = GSTA sprite_id SAMPLING PHASE, not a wrong field/atlas. The body walker loc_8c0344d4 (bank03:10232-10243) reads char+0x144 masked &0x7FFF (loc_8c0345fc=0x160, add 0xE4 imm8 sign-ext -0x1C => 0x144; loc_8c0345fe=0x7fff) the SAME engine phase it is written. char+0x144 is written by the anim tick loc_8c034dee (bank03:11567): decrements anim_timer@0x142 (add 0xFF), and ON expiry advances current_cell@0x154 by +0x14 (20B keyframe) and duff-copies (bank12.loc_8c1294c8, mov 0x14,r0) the 20B keyframe from deref(0x154) into player+0x140, so +0x144 = keyframe[4] = deref(0x154)+4. Engine ground-truth (Oracle mc_oracle_charPassCapture at rend_start_render Renderer_if.cpp:649, reads +0x144 at the STARTRENDER moment = same phase as loc_8c0344d4): _ryu_capture/oracle_hook_20260611.jsonl node 0x0C2688E4 (Sentinel cid23) cycles 213(178491)>214(178494)>428(178499)>429(178501)>430(178504)>215(178507); diagonal 428-430 spans ~6 frames 178499-178504. The wire (GSTA) samples +0x144 via readGameState in serverPublish at render()/DequeueRender (mirror.cpp:2323), which lags the engine render phase by ~1 frame under threaded rendering + MVC2 multi-pass STARTRENDER (QueueRender single-slot DROPS the character pass; only the HUD/composite pass survives to render(), Renderer_if.cpp:634-651). The 'ASSEMBLY RE-KEY' read read16(cellP+4) is NOT fresher (gamestate.cpp:1169-1170 own comment: keyframe[4]==sid144). FIX = move the GSTA readGameState sample EARLIER, into rend_start_render alongside mc_oracle_charPassCapture (the STARTRENDER/character-pass phase where loc_8c0344d4 ran), OR latch sprite_id (+0x144) and anim_timer (+0x142) into a render-phase snapshot in mc_oracle_charPassCapture and have readGameState ship the latched value. Same lag affects OBJS satellites: readAllDrawn (gamestate.cpp:381) reads node+0x144 (line 406). Read-only, determinism-safe (no guest writes).",
  status: "confirmed",
  confidence: "high",
  date: "2026-06-11"
};
UPSERT source:oracle_sentinel_sid_20260611 CONTENT {
  id: source:oracle_sentinel_sid_20260611,
  ref: "_ryu_capture/oracle_hook_20260611.jsonl",
  note: "Frame-Oracle charPassCapture: Sentinel (node 0x0C2688E4, cid23) body sprite_id per frame 178491-178517 = engine render-phase ground truth. Diagonal-down 428-430 spans frames 178499-178504."
};
RELATE finding:gsta_sprite_id_sampling_phase->cites->source:oracle_sentinel_sid_20260611;
RELATE finding:gsta_sprite_id_sampling_phase->cites->routine:loc_8c034dee;
RELATE finding:gsta_sprite_id_sampling_phase->cites->routine:loc_8c0344d4;
