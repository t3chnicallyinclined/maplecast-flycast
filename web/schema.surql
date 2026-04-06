-- MapleCast SurrealDB Schema
-- KING OF MARVEL — The Arcade Never Forgets
-- Every combo, every clutch, every bodying is recorded forever.

-- ============================================================
-- ONE-TIME CLEANUP — drop legacy stick schema (mac-keyed) so the
-- new username-keyed definition below is the only authority. Safe
-- to re-run: REMOVE on a missing object is silently ignored, and
-- the field re-DEFINE below uses OVERWRITE for the type change on
-- `mac` (string → option<string>). DELETE wipes any rows that
-- pre-date the username field — empty in production at write time,
-- so nothing of value is lost.
-- ============================================================
REMOVE INDEX idx_mac ON TABLE stick;
DELETE stick;
DELETE owns;

-- ============================================================
-- RANK TIERS (ELO-based, inspired by SF6/Tekken)
-- ============================================================
-- Rookie:       0-499
-- Fighter:    500-999
-- Warrior:   1000-1499  (starting rank)
-- Champion:  1500-1999
-- Master:    2000-2499
-- Grand Master: 2500-2999
-- Legend:     3000+

-- ============================================================
-- PLAYERS — identity + lifetime stats + rating
-- ============================================================
DEFINE TABLE player SCHEMAFULL;
DEFINE FIELD username     ON player TYPE string ASSERT string::len($value) >= 4 AND string::len($value) <= 16;
DEFINE FIELD pass_hash    ON player TYPE option<string>;       -- argon2 hash
DEFINE FIELD avatar       ON player TYPE option<string>;       -- emoji icon
DEFINE FIELD created_at   ON player TYPE datetime DEFAULT time::now();
DEFINE FIELD last_seen    ON player TYPE datetime DEFAULT time::now();

-- Rating
DEFINE FIELD rating       ON player TYPE int DEFAULT 1000;       -- ELO rating
DEFINE FIELD peak_rating  ON player TYPE int DEFAULT 1000;       -- all-time high
DEFINE FIELD rank_tier    ON player TYPE string DEFAULT "Warrior"; -- display tier

-- W/L
DEFINE FIELD wins         ON player TYPE int DEFAULT 0;
DEFINE FIELD losses       ON player TYPE int DEFAULT 0;
DEFINE FIELD streak       ON player TYPE int DEFAULT 0;          -- current (negative = losing)
DEFINE FIELD best_streak  ON player TYPE int DEFAULT 0;          -- all-time best win streak
DEFINE FIELD worst_streak ON player TYPE int DEFAULT 0;          -- all-time worst losing streak

-- Combo stats
DEFINE FIELD best_combo       ON player TYPE int DEFAULT 0;      -- all-time highest combo
DEFINE FIELD total_combo_hits ON player TYPE int DEFAULT 0;      -- lifetime combo hits landed
DEFINE FIELD total_combos     ON player TYPE int DEFAULT 0;      -- lifetime combo count

-- Match quality
DEFINE FIELD perfects     ON player TYPE int DEFAULT 0;          -- rounds won taking 0 damage
DEFINE FIELD ocvs         ON player TYPE int DEFAULT 0;          -- won without losing a character
DEFINE FIELD comebacks    ON player TYPE int DEFAULT 0;          -- won from 1 char vs 3
DEFINE FIELD clutch_wins  ON player TYPE int DEFAULT 0;          -- won with timer < 10
DEFINE FIELD timeouts_won ON player TYPE int DEFAULT 0;          -- won by timeout
DEFINE FIELD first_bloods ON player TYPE int DEFAULT 0;          -- landed first hit of match

-- Playstyle
DEFINE FIELD total_matches   ON player TYPE int DEFAULT 0;
DEFINE FIELD total_rounds    ON player TYPE int DEFAULT 0;
DEFINE FIELD avg_match_time  ON player TYPE float DEFAULT 0.0;   -- seconds
DEFINE FIELD unique_chars    ON player TYPE int DEFAULT 0;       -- distinct characters used
DEFINE FIELD unique_teams    ON player TYPE int DEFAULT 0;       -- distinct team compositions used

DEFINE INDEX idx_username ON player FIELDS username UNIQUE;
DEFINE INDEX idx_rating   ON player FIELDS rating;

-- ============================================================
-- STICKS — hardware identity, bound to players (canonical key: username)
-- ============================================================
-- One stick per user. Record id is `stick:<username>` so upserts from the
-- collector are idempotent. ip/port are mutable observed-state (refreshed
-- whenever flycast sees a new packet from the bound source). mac is reserved
-- for a future firmware change that bakes a hardware UID into UDP handshake.
DEFINE TABLE stick SCHEMAFULL;
DEFINE FIELD username    ON stick TYPE string ASSERT string::len($value) >= 4 AND string::len($value) <= 16;
DEFINE FIELD ip          ON stick TYPE string;
DEFINE FIELD port        ON stick TYPE int;
-- mac was previously TYPE string (required, indexed) — OVERWRITE so the type
-- change to option<string> applies cleanly when re-running on an existing DB.
DEFINE FIELD OVERWRITE mac ON stick TYPE option<string>;
DEFINE FIELD OVERWRITE firmware ON stick TYPE option<string>;
DEFINE FIELD registered  ON stick TYPE datetime DEFAULT time::now();
DEFINE FIELD last_input  ON stick TYPE datetime DEFAULT time::now();
DEFINE INDEX idx_stick_user ON stick FIELDS username UNIQUE;

DEFINE TABLE owns SCHEMAFULL TYPE RELATION FROM player TO stick;
DEFINE FIELD since ON owns TYPE datetime DEFAULT time::now();

-- ============================================================
-- CHARACTER STATS — per-player per-character lifetime stats
-- ============================================================
DEFINE TABLE char_stats SCHEMAFULL;
DEFINE FIELD username    ON char_stats TYPE string;
DEFINE FIELD char_id     ON char_stats TYPE int;         -- MVC2 character ID (0-55)
DEFINE FIELD char_name   ON char_stats TYPE string;      -- "Magneto", "Cable", etc.
DEFINE FIELD games       ON char_stats TYPE int DEFAULT 0;
DEFINE FIELD wins        ON char_stats TYPE int DEFAULT 0;
DEFINE FIELD losses      ON char_stats TYPE int DEFAULT 0;
DEFINE FIELD best_combo  ON char_stats TYPE int DEFAULT 0;
DEFINE FIELD total_kills ON char_stats TYPE int DEFAULT 0;  -- KOs scored with this char
DEFINE FIELD total_deaths ON char_stats TYPE int DEFAULT 0; -- times this char was KO'd
DEFINE INDEX idx_user_char ON char_stats FIELDS username, char_id UNIQUE;

-- ============================================================
-- TEAM STATS — per-player per-team-composition
-- ============================================================
DEFINE TABLE team_stats SCHEMAFULL;
DEFINE FIELD username    ON team_stats TYPE string;
DEFINE FIELD team_key    ON team_stats TYPE string;      -- sorted "char1_char2_char3" for dedup
DEFINE FIELD char1       ON team_stats TYPE int;
DEFINE FIELD char2       ON team_stats TYPE int;
DEFINE FIELD char3       ON team_stats TYPE int;
DEFINE FIELD char1_name  ON team_stats TYPE string;
DEFINE FIELD char2_name  ON team_stats TYPE string;
DEFINE FIELD char3_name  ON team_stats TYPE string;
DEFINE FIELD games       ON team_stats TYPE int DEFAULT 0;
DEFINE FIELD wins        ON team_stats TYPE int DEFAULT 0;
DEFINE FIELD losses      ON team_stats TYPE int DEFAULT 0;
DEFINE INDEX idx_user_team ON team_stats FIELDS username, team_key UNIQUE;

-- ============================================================
-- HEAD TO HEAD — per-player-pair W/L record
-- ============================================================
DEFINE TABLE h2h SCHEMAFULL;
DEFINE FIELD player1     ON h2h TYPE string;             -- alphabetically first username
DEFINE FIELD player2     ON h2h TYPE string;
DEFINE FIELD p1_wins     ON h2h TYPE int DEFAULT 0;      -- player1's wins
DEFINE FIELD p2_wins     ON h2h TYPE int DEFAULT 0;      -- player2's wins
DEFINE FIELD last_match  ON h2h TYPE datetime DEFAULT time::now();
DEFINE INDEX idx_h2h     ON h2h FIELDS player1, player2 UNIQUE;

-- ============================================================
-- MATCHES — every game played, full detail
-- ============================================================
DEFINE TABLE match SCHEMAFULL;
DEFINE FIELD started_at   ON match TYPE datetime DEFAULT time::now();
DEFINE FIELD ended_at     ON match TYPE option<datetime>;
DEFINE FIELD duration_s   ON match TYPE option<float>;
DEFINE FIELD stage        ON match TYPE option<int>;
DEFINE FIELD winner_slot  ON match TYPE option<int>;
DEFINE FIELD finish_type  ON match TYPE option<string>;   -- "ko", "timeout", "quit"

-- P1
DEFINE FIELD p1_name      ON match TYPE option<string>;
DEFINE FIELD p1_rating    ON match TYPE option<int>;      -- rating BEFORE match
DEFINE FIELD p1_rating_change ON match TYPE option<int>;  -- +/- after match
DEFINE FIELD p1_chars     ON match TYPE option<array<int>>;
DEFINE FIELD p1_hp_final  ON match TYPE option<array<int>>;
DEFINE FIELD p1_max_combo ON match TYPE option<int>;
DEFINE FIELD p1_total_damage ON match TYPE option<int>;
DEFINE FIELD p1_meter_used ON match TYPE option<int>;
DEFINE FIELD p1_inputs    ON match TYPE option<int>;
DEFINE FIELD p1_pps_avg   ON match TYPE option<int>;
DEFINE FIELD p1_chars_alive ON match TYPE option<int>;    -- chars remaining at end

-- P2
DEFINE FIELD p2_name      ON match TYPE option<string>;
DEFINE FIELD p2_rating    ON match TYPE option<int>;
DEFINE FIELD p2_rating_change ON match TYPE option<int>;
DEFINE FIELD p2_chars     ON match TYPE option<array<int>>;
DEFINE FIELD p2_hp_final  ON match TYPE option<array<int>>;
DEFINE FIELD p2_max_combo ON match TYPE option<int>;
DEFINE FIELD p2_total_damage ON match TYPE option<int>;
DEFINE FIELD p2_meter_used ON match TYPE option<int>;
DEFINE FIELD p2_inputs    ON match TYPE option<int>;
DEFINE FIELD p2_pps_avg   ON match TYPE option<int>;
DEFINE FIELD p2_chars_alive ON match TYPE option<int>;

-- Match quality flags
DEFINE FIELD was_perfect  ON match TYPE bool DEFAULT false;   -- winner took 0 damage
DEFINE FIELD was_ocv      ON match TYPE bool DEFAULT false;   -- winner used only 1 char
DEFINE FIELD was_comeback ON match TYPE bool DEFAULT false;   -- winner was down 1v3
DEFINE FIELD was_clutch   ON match TYPE bool DEFAULT false;   -- timer < 10 at finish
DEFINE FIELD was_timeout  ON match TYPE bool DEFAULT false;

-- Graph edges
DEFINE TABLE played SCHEMAFULL TYPE RELATION FROM player TO match;
DEFINE FIELD slot   ON played TYPE int;
DEFINE FIELD won    ON played TYPE bool;
DEFINE FIELD chars  ON played TYPE option<array<int>>;
DEFINE FIELD rating_change ON played TYPE option<int>;

-- ============================================================
-- BADGES — unlockable achievements
-- ============================================================
DEFINE TABLE badge SCHEMAFULL;
DEFINE FIELD badge_id    ON badge TYPE string;           -- "combo_monster", "iron_wall", etc.
DEFINE FIELD name        ON badge TYPE string;
DEFINE FIELD description ON badge TYPE string;
DEFINE FIELD icon        ON badge TYPE string;           -- emoji or icon key
DEFINE FIELD tier        ON badge TYPE string;           -- "bronze", "silver", "gold"
DEFINE FIELD requirement ON badge TYPE int;              -- threshold value
DEFINE INDEX idx_badge   ON badge FIELDS badge_id, tier UNIQUE;

-- Player earned badge (graph edge)
DEFINE TABLE earned SCHEMAFULL TYPE RELATION FROM player TO badge;
DEFINE FIELD earned_at ON earned TYPE datetime DEFAULT time::now();
DEFINE FIELD value     ON earned TYPE option<int>;       -- the stat value when earned

-- ============================================================
-- EVENTS — significant moments
-- ============================================================
DEFINE TABLE game_event SCHEMAFULL;
DEFINE FIELD ts       ON game_event TYPE datetime DEFAULT time::now();
DEFINE FIELD kind     ON game_event TYPE string;
-- FLEXIBLE: collector writes ad-hoc match metadata (winner, p1_team, combos…)
-- under `data` and we don't want to enumerate every possible key here.
-- OVERWRITE: idempotent across collector restarts.
DEFINE FIELD OVERWRITE data ON game_event TYPE option<object> FLEXIBLE;

-- ============================================================
-- SEED: BADGE DEFINITIONS
-- ============================================================

-- Combo badges
CREATE badge SET badge_id="combo_monster", tier="bronze", name="Combo Starter", description="Hit a 20+ combo", icon="chains", requirement=20;
CREATE badge SET badge_id="combo_monster", tier="silver", name="Combo Fiend", description="Hit a 40+ combo", icon="chains", requirement=40;
CREATE badge SET badge_id="combo_monster", tier="gold", name="Combo God", description="Hit a 60+ combo", icon="chains", requirement=60;

-- Win streak badges
CREATE badge SET badge_id="streak", tier="bronze", name="Hot Streak", description="Win 5 in a row", icon="fire", requirement=5;
CREATE badge SET badge_id="streak", tier="silver", name="On Fire", description="Win 10 in a row", icon="fire", requirement=10;
CREATE badge SET badge_id="streak", tier="gold", name="Unstoppable", description="Win 20 in a row", icon="fire", requirement=20;

-- Match count badges
CREATE badge SET badge_id="veteran", tier="bronze", name="Regular", description="Play 50 matches", icon="joystick", requirement=50;
CREATE badge SET badge_id="veteran", tier="silver", name="Veteran", description="Play 200 matches", icon="joystick", requirement=200;
CREATE badge SET badge_id="veteran", tier="gold", name="Arcade Rat", description="Play 500 matches", icon="joystick", requirement=500;

-- Special moment badges
CREATE badge SET badge_id="perfect", tier="bronze", name="Flawless", description="Win a match without taking damage", icon="shield", requirement=1;
CREATE badge SET badge_id="perfect", tier="silver", name="Untouchable", description="10 perfect wins", icon="shield", requirement=10;
CREATE badge SET badge_id="perfect", tier="gold", name="Iron Wall", description="50 perfect wins", icon="shield", requirement=50;

CREATE badge SET badge_id="ocv", tier="bronze", name="One Man Army", description="Win using only 1 character", icon="skull", requirement=1;
CREATE badge SET badge_id="ocv", tier="silver", name="Solo King", description="10 OCV wins", icon="skull", requirement=10;
CREATE badge SET badge_id="ocv", tier="gold", name="OCV Machine", description="50 OCV wins", icon="skull", requirement=50;

CREATE badge SET badge_id="comeback", tier="bronze", name="Comeback Kid", description="Win from 1 char vs 3", icon="bolt", requirement=1;
CREATE badge SET badge_id="comeback", tier="silver", name="Never Give Up", description="10 comeback wins", icon="bolt", requirement=10;
CREATE badge SET badge_id="comeback", tier="gold", name="Plot Armor", description="50 comeback wins", icon="bolt", requirement=50;

CREATE badge SET badge_id="clutch", tier="bronze", name="Clutch Player", description="Win with timer under 10", icon="clock", requirement=1;
CREATE badge SET badge_id="clutch", tier="silver", name="Buzzer Beater", description="10 clutch wins", icon="clock", requirement=10;
CREATE badge SET badge_id="clutch", tier="gold", name="Time Lord", description="50 clutch wins", icon="clock", requirement=50;

-- Character diversity
CREATE badge SET badge_id="diversity", tier="bronze", name="Explorer", description="Use 10 different characters", icon="globe", requirement=10;
CREATE badge SET badge_id="diversity", tier="silver", name="Variety Pack", description="Use 25 different characters", icon="globe", requirement=25;
CREATE badge SET badge_id="diversity", tier="gold", name="Roster Mastery", description="Use all 56 characters", icon="globe", requirement=56;

-- Loyalty
CREATE badge SET badge_id="loyalty", tier="bronze", name="One True Team", description="50 games with same team", icon="heart", requirement=50;
CREATE badge SET badge_id="loyalty", tier="silver", name="Team Specialist", description="200 games with same team", icon="heart", requirement=200;
CREATE badge SET badge_id="loyalty", tier="gold", name="Ride or Die", description="500 games with same team", icon="heart", requirement=500;

-- Rating milestones
CREATE badge SET badge_id="rating", tier="bronze", name="Champion", description="Reach Champion rank (1500)", icon="crown", requirement=1500;
CREATE badge SET badge_id="rating", tier="silver", name="Master", description="Reach Master rank (2000)", icon="crown", requirement=2000;
CREATE badge SET badge_id="rating", tier="gold", name="Legend", description="Reach Legend rank (3000)", icon="crown", requirement=3000;
