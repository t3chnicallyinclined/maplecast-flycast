-- ============================================================================
-- 84: edge integrity -- the dangling edges the `cites`-only check missed
--
-- I reported "0 dangling cites" and treated that as clean. It was true and it
-- was scoped wrong: the same check over `about` finds TWO edges pointing at
-- records that do not exist. Both were seeded, both look satisfied in a
-- traversal, and neither resolves. Same class as 79 and 81; the lesson is that
-- an integrity check must enumerate the edge tables, not name one.
--
--   finding:gsta_super_effect_overtile      -> about -> finding:atlas_coverage_and_bit15
--   finding:gsta_wire_ext_completeness_...  -> about -> field:render_extra
--
-- Apply: tools/re_kb/rekb.sh @tools/re_kb/84_edge_integrity.surql
-- ============================================================================
USE NS re DB kb;

-- ---- 1. field:render_extra -- the target really should exist ---------------
-- NOT invented. Three independent groundings, checked 2026-09-01:
--   * the citing finding's own text: "0x151 RenderExtra has NO pl_mem symbol
--     but IS cell byte+0x11 bulk-copied into player+0x140..0x153 by anim tick
--     loc_8c034dee (literal 0x0140, r0=0x14=20B copy) => valid"
--   * core/network/maplecast_gamestate.cpp:73
--     `static const uint32_t OFF_RENDER_EXTRA = 0x151;` (and :1809, :2043, :2192)
--   * docs/MARVELOUS2-RE-HANDOFF.md:119  "RenderExtra @kf+0x11 -> player+0x151"
UPSERT field:render_extra SET
  offset='+0x151',
  name='RenderExtra',
  owner='char_struct',
  type='u8',
  class='engine',
  note='Super/aura overlay driver. No pl_mem.asm symbol -- it is keyframe byte +0x11, bulk-copied into player+0x140..0x153 by the anim tick loc_8c034dee (20-byte copy from deref(0x154)). Read live by maplecast_gamestate.cpp (OFF_RENDER_EXTRA @:73) and shipped on the GSTA wire. Row created 2026-09-01 to resolve a dangling about edge.';

RELATE field:render_extra->cites->routine:loc_8c034dee;

-- ---- 2. re-point the bit15 edge at the finding that actually exists --------
-- The target `finding:atlas_coverage_and_bit15` was never created; the claim
-- it meant lives at finding:sprite_id_bit15_dispatch (status=confirmed), which
-- says bit15 is a transform-variant/scale dispatch stripped by &0x7FFF -- the
-- same claim, under the slug that got used.
DELETE about WHERE record::tb(in)='finding'
  AND record::id(in)='gsta_super_effect_overtile'
  AND record::id(out)='atlas_coverage_and_bit15';

RELATE finding:gsta_super_effect_overtile->about->finding:sprite_id_bit15_dispatch;

-- NOTE, and it is a real design question rather than a cleanup:
-- `about` is overloaded. 161 of its ~290 edges point finding->finding, so it
-- means BOTH "this claim is about that entity" AND "this claim relates to that
-- claim". That is why kb.contradictions() cannot work -- grouping by `about`
-- groups claims by other claims. Splitting it (about = finding->entity;
-- refines/relates_to = finding->finding) is the fix, and it is a migration of
-- 161 edges, so it is NOT done here.
