-- MapleCast SurrealDB Schema
-- Every microsecond matters. Every stat tells a story.

-- ============================================================
-- PLAYERS — identity, registration, lifetime stats
-- ============================================================
DEFINE TABLE player SCHEMAFULL;
DEFINE FIELD username    ON player TYPE string ASSERT string::len($value) >= 4 AND string::len($value) <= 12;
DEFINE FIELD created_at  ON player TYPE datetime DEFAULT time::now();
DEFINE FIELD last_seen   ON player TYPE datetime DEFAULT time::now();
DEFINE INDEX idx_username ON player FIELDS username UNIQUE;

-- ============================================================
-- STICKS — hardware identity, bound to players
-- ============================================================
DEFINE TABLE stick SCHEMAFULL;
DEFINE FIELD mac         ON stick TYPE string;   -- "00:08:DC:DC:00:01"
DEFINE FIELD ip          ON stick TYPE string;   -- last known IP
DEFINE FIELD port        ON stick TYPE int;
DEFINE FIELD registered  ON stick TYPE datetime DEFAULT time::now();
DEFINE FIELD last_input  ON stick TYPE datetime DEFAULT time::now();
DEFINE FIELD firmware    ON stick TYPE option<string>;
DEFINE INDEX idx_mac     ON stick FIELDS mac UNIQUE;

-- Player owns stick (graph edge)
DEFINE TABLE owns SCHEMAFULL TYPE RELATION FROM player TO stick;
DEFINE FIELD since ON owns TYPE datetime DEFAULT time::now();

-- ============================================================
-- MATCHES — every game played
-- ============================================================
DEFINE TABLE match SCHEMAFULL;
DEFINE FIELD started_at   ON match TYPE datetime DEFAULT time::now();
DEFINE FIELD ended_at     ON match TYPE option<datetime>;
DEFINE FIELD duration_s   ON match TYPE option<float>;
DEFINE FIELD stage        ON match TYPE option<int>;
DEFINE FIELD winner_slot  ON match TYPE option<int>;   -- 0=P1, 1=P2

-- Per-player match data
DEFINE FIELD p1_name      ON match TYPE option<string>;
DEFINE FIELD p1_chars     ON match TYPE option<array<int>>;  -- [char1, char2, char3]
DEFINE FIELD p1_hp_final  ON match TYPE option<array<int>>;  -- [hp1, hp2, hp3]
DEFINE FIELD p1_max_combo ON match TYPE option<int>;
DEFINE FIELD p1_meter     ON match TYPE option<int>;
DEFINE FIELD p1_inputs    ON match TYPE option<int>;  -- total input changes during match
DEFINE FIELD p1_pps_avg   ON match TYPE option<int>;  -- avg packets/sec

DEFINE FIELD p2_name      ON match TYPE option<string>;
DEFINE FIELD p2_chars     ON match TYPE option<array<int>>;
DEFINE FIELD p2_hp_final  ON match TYPE option<array<int>>;
DEFINE FIELD p2_max_combo ON match TYPE option<int>;
DEFINE FIELD p2_meter     ON match TYPE option<int>;
DEFINE FIELD p2_inputs    ON match TYPE option<int>;
DEFINE FIELD p2_pps_avg   ON match TYPE option<int>;

-- Player played in match (graph edges)
DEFINE TABLE played SCHEMAFULL TYPE RELATION FROM player TO match;
DEFINE FIELD slot   ON played TYPE int;       -- 0=P1, 1=P2
DEFINE FIELD won    ON played TYPE bool;
DEFINE FIELD chars  ON played TYPE option<array<int>>;

-- ============================================================
-- INPUT SNAPSHOTS — sampled input telemetry (1/sec, not per-packet)
-- ============================================================
DEFINE TABLE input_snapshot SCHEMAFULL;
DEFINE FIELD ts          ON input_snapshot TYPE datetime DEFAULT time::now();
DEFINE FIELD slot        ON input_snapshot TYPE int;
DEFINE FIELD player_name ON input_snapshot TYPE option<string>;
DEFINE FIELD pps         ON input_snapshot TYPE int;     -- packets/sec
DEFINE FIELD cps         ON input_snapshot TYPE int;     -- changes/sec (unique inputs)
DEFINE FIELD buttons     ON input_snapshot TYPE int;     -- current button state
DEFINE FIELD lt          ON input_snapshot TYPE int;
DEFINE FIELD rt          ON input_snapshot TYPE int;
DEFINE FIELD input_type  ON input_snapshot TYPE string;  -- "NobdUDP", "BrowserWS"

-- ============================================================
-- GAME STATE SNAPSHOTS — from 253-byte MVC2 RAM reads (1/sec)
-- ============================================================
DEFINE TABLE game_snapshot SCHEMAFULL;
DEFINE FIELD ts          ON game_snapshot TYPE datetime DEFAULT time::now();
DEFINE FIELD frame       ON game_snapshot TYPE int;
DEFINE FIELD in_match    ON game_snapshot TYPE bool;
DEFINE FIELD timer       ON game_snapshot TYPE option<int>;
DEFINE FIELD stage       ON game_snapshot TYPE option<int>;
DEFINE FIELD p1_chars    ON game_snapshot TYPE option<array<int>>;
DEFINE FIELD p2_chars    ON game_snapshot TYPE option<array<int>>;
DEFINE FIELD p1_hp       ON game_snapshot TYPE option<array<int>>;
DEFINE FIELD p2_hp       ON game_snapshot TYPE option<array<int>>;
DEFINE FIELD p1_combo    ON game_snapshot TYPE option<int>;
DEFINE FIELD p2_combo    ON game_snapshot TYPE option<int>;
DEFINE FIELD p1_meter    ON game_snapshot TYPE option<int>;
DEFINE FIELD p2_meter    ON game_snapshot TYPE option<int>;

-- ============================================================
-- STREAM TELEMETRY — mirror/encode performance (1/sec)
-- ============================================================
DEFINE TABLE stream_snapshot SCHEMAFULL;
DEFINE FIELD ts           ON stream_snapshot TYPE datetime DEFAULT time::now();
DEFINE FIELD fps          ON stream_snapshot TYPE int;
DEFINE FIELD kbps         ON stream_snapshot TYPE int;
DEFINE FIELD publish_us   ON stream_snapshot TYPE int;
DEFINE FIELD dirty_pages  ON stream_snapshot TYPE int;
DEFINE FIELD frame        ON stream_snapshot TYPE int;
DEFINE FIELD spectators   ON stream_snapshot TYPE int;

-- ============================================================
-- EVENTS — significant moments (wins, combos, perfects, etc.)
-- ============================================================
DEFINE TABLE game_event SCHEMAFULL;
DEFINE FIELD ts       ON game_event TYPE datetime DEFAULT time::now();
DEFINE FIELD kind     ON game_event TYPE string;  -- "match_start", "match_end", "combo", "perfect", "timeout"
DEFINE FIELD data     ON game_event TYPE option<object>;
