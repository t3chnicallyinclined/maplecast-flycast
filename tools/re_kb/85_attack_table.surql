-- ============================================================================
-- 85: the `attack` table -- MVC2's frame-data layer
--
-- anotak's per-character attack tables were CRAWLED, PARSED, cached in
-- ingest/data/anotak_PL*.json ... and then dropped. gen_anotak() never
-- referenced `rec["attacks"]`. 3,384 records across 59 characters -- damage,
-- hit reaction, block flags, undizzy, hitstop, knockdown duration, juggle
-- X/Y, hitspark, invincibility -- sat on disk, un-queryable, at zero network
-- cost to ingest.
--
-- docs/GAME-DATA-COMPLETENESS-LEDGER.md flags a frame-data / hitbox viewer as
-- wanted. This is its data layer.
--
-- An `attack` is an ARTIFACT, not a claim: it has an address in the DAT and a
-- raw byte blob, and anyone with the ROM re-derives it. So it carries no
-- `status` and never touches `finding` -- same rule as `docnote` and
-- `routine`.
-- ============================================================================
USE NS re DB kb;

DEFINE TABLE IF NOT EXISTS attack SCHEMALESS
  COMMENT 'One MVC2 attack record from the PLxx_DAT attack table (anotak). Fields: char_id, name, damage, hit_reaction, block_flags, damage_type, undizzy, hitstop, kd_duration, juggle_x, juggle_y, hitspark, impact_sound, invincibility, flags, addr, raw. An ARTIFACT re-derivable from the ROM -- deliberately has NO status.';
