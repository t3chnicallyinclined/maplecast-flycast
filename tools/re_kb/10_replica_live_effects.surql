USE NS re DB kb;

-- ===========================================================================
-- finding:replica_live_effects — render-replica PHASE 3 EFFECTS pass (hitsparks /
-- electric / energy). The pure-effect pool nodes (cat 1..4, GFX base in the shared
-- "Effect Poly" bank 0x0CED0000, NO body GFX2) that the body walker loc_8c0344d4
-- emits 0 tiles for, now render CLIENT-side in web/render-replica/replay.html.
-- ===========================================================================
UPSERT finding:replica_live_effects SET
  status = "implemented",
  confidence = "high",
  date = "2026-06-13",
  statement = "RENDER-REPLICA PHASE 3 EFFECTS (hitsparks) — pure-effect pool nodes render client-side, validated. GAP: render_frame.c render_object_full_ex runs the body walker loc_8c0344d4 for cat 1..4 satellites; a PURE effect (GFX base node+0x15C in Effect Poly [0x0CED0000,0x0CEE0000), NO body GFX2 cell records) finds no records => emits 0 tiles (render_frame.c names the unimplemented 'loc_8c1294c8 cell processor for non-body effects'). NO change to render_frame/gen_*/body_decoder/stage/HUD/GFX tails — ADDED a separate effects pass (like stage + HUD). GROUNDED BINDING (NOT the unproven sprite_id guess in fx_atlas.json keymap): the Effect-Poly DIRECTORY at *(0x0CED0008) is 0x10-byte entries {e0=w|h<<16, e4=fmt, e8=texelPtr}; an effect node's GFX base node+0x15C IS a pointer to its dir entry, so dirIdx = (node+0x15C - dirBase) / 0x10 (exact, off must be 0x10-aligned). Quad SIZE = directory e0 (w,h); ANCHOR = node+0xE0/E4 (the same own-origin anchor loc_8c030af8 deposits for satellites), point-centered (dx=-w/2 dy=-h/2, matching tools/reshape_fx_atlas.py); xflip = node+0x130; additive blend (reference_mvc2_effects_bank: Effect-Poly GFX is the proven additive signal). ZERO server change + ZERO per-frame effect wire: the effect-node STATE is ALREADY in the replica read-set (slot_cnt 0x8C2895E0 + slot_ptr 0x8C287DE0 + objpool 0x8C26AA54, maplecast_replica_live.cpp) and the 0x0C... Effect-Poly bank IS the area-3 16MB RAM the replica ships as ram16, so dirBase + the directory + the texels all read live from the client RAM image. The effect TEXTURES are a static ROM asset baked OFFLINE (tools/build_replica_effects_atlas.py from effects-capture/efx_NNN.png + mc_effects.log dir dump -> web/render-replica/effects/effects.{png,json}, 25 dir entries, gitignored). VERIFIED: (a) directory read from a live .mcrr RAM image matches effects-capture/fx_atlas.json byte-for-byte (dirBase=0x0CED03D8, idx0=128x128 e8=0x0CDA4000, ...); (b) tools/render-replica-poc/shot_effects.mjs pokes a synthetic effect node bound via node+0x15C->dir entry at known anchors and reads back the additive overlay: idx0 128x128 @ (320,240) -> centroid (320,243) bbox 127x127 PASS; idx5 256x256 @ (200,180) -> (208,187) PASS; idx13 128x128 @ (450,320) -> (450,320) PASS. CLIENT: web/render-replica/effects-client.mjs (EffectsClient.readEffectNodes + render, 2D additive overlay ABOVE bodies/stage, BELOW HUD); wired into replay.html showFrame + liveApplyFrame (renderEffectsPass before renderHudPass); cache-bust ?v=fx1. OPEN: (1) NO local capture contains a real hit — all .mcrr captures (rot_cap/side_cap/live2/live/scramble) have ZERO effect nodes (idle/body-debug play); validate against a PROD live hit (replica-live stream during contact) or a fresh capture. (2) v1 renders EVERY effect dir-bound node point-centered; per-TYPE anchor/scale/frame-animation tuning is the documented follow-up. (3) the directory->idx binding is proven arithmetically + via synthetic poke, but NOT yet against a LIVE pure-effect node's node+0x15C (needs the hit capture); the offline-decode QUALITY of some dir textures (idx0 shows a grid-like detwiddle) is independent of the binding and a separate decode-format follow-up.";

-- routines (effect path)
RELATE finding:replica_live_effects->cites->routine:loc_8c0344d4 SET note = "body walker — emits 0 tiles for a pure-effect node (no GFX2 cell records)";
RELATE finding:replica_live_effects->cites->routine:loc_8c030af8 SET note = "satellite/effect setup — deposits node+0xE0/E4 anchor (same as bodies)";

-- data / memory
RELATE finding:replica_live_effects->about->memdata:effect_poly_bank SET note = "0x0CED0000 Effect Poly; dir at *(0x0CED0008); entries {e0=w|h<<16,e4=fmt,e8=texelPtr}";

-- tools
RELATE finding:replica_live_effects->about->tool:build_replica_effects_atlas SET note = "offline atlas builder: efx_NNN.png + mc_effects.log -> effects.{png,json} keyed by dir idx";
RELATE finding:replica_live_effects->about->tool:shot_effects SET note = "headless validation: poke synthetic effect node, read back additive overlay centroid";

-- relate to the prior effects-bank + blend findings (one connected effects subgraph)
RELATE finding:replica_live_effects->about->finding:objs_effect_blend_gap SET note = "shares the is_effect = GFX-in-0x0CED0000 additive signal";

UPSERT tool:build_replica_effects_atlas SET path = "tools/build_replica_effects_atlas.py", purpose = "bake the Effect-Poly directory textures into a dir-idx-keyed local atlas for the render-replica effects pass";
UPSERT tool:shot_effects SET path = "tools/render-replica-poc/shot_effects.mjs", purpose = "headless puppeteer validation of the replica effects pass (synthetic effect node + overlay readback)";
UPSERT memdata:effect_poly_bank SET addr = 0x0CED0000, name = "Effect Poly bank", note = "shared hitspark/electric/energy textures; directory base at *(0x0CED0008)";
