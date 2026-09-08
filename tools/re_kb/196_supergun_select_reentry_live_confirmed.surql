-- 196_supergun_select_reentry_live_confirmed.surql -- operator confirmation on screen (live run
-- rr_play/cgeom19, 2026-09-06) of the three select re-entry findings that were gated headless only.
-- Forward slashes only in ref/note; no apostrophes.
USE NS re DB kb;

UPSERT source:supergun_select_reentry_live_20260906 SET kind='observation', strength='reproduction',
  ref='Live run rr_play/cgeom19 with runner commit 4ef0e94 (supergun): first select opens with empty slot-1/2 banners on both sides and slot 0 on the default table; window stretched keeps the 4:3 stage; after the KO, P1 SAME TEAM locked its three slots by itself and P2 CHANGE TEAMS came up on its own last team on all three banners.',
  note='Operator words: that worked. Visual confirmation only; the numeric render gate (RENDER-PIXEL-GATE) is still the open item.';

RELATE finding:supergun_select_reentry_through_slot0_entry->cites->source:supergun_select_reentry_live_20260906 SET id='e196_live_193';
RELATE finding:select_preview_cache_compare_rebuild->cites->source:supergun_select_reentry_live_20260906 SET id='e196_live_194';
RELATE finding:select_side_entry_placeholder_pages->cites->source:supergun_select_reentry_live_20260906 SET id='e196_live_195';
