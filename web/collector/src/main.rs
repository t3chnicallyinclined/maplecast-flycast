//! MapleCast Data Collector — Rust service bridging flycast WS → SurrealDB.
//!
//! Subscribes to the WS:7200 status broadcast and writes:
//! - Input snapshots (1/sec per player)
//! - Game state snapshots (1/sec)
//! - Stream telemetry (1/sec)
//! - Match events (start/end)
//! - Player records
//!
//! Zero impact on game loop — reads from the existing broadcast.

use futures_util::StreamExt;
use serde::Deserialize;
use surrealdb::engine::any::{self, Any};
use surrealdb::opt::auth::Root;
use surrealdb::Surreal;
use tokio_tungstenite::connect_async;

// Defaults — overridable via env:
//   MAPLECAST_WS=ws://host:port      (where flycast or relay broadcasts)
//   MAPLECAST_DB=ws://host:port      (SurrealDB endpoint, or rocksdb://path)
//   MAPLECAST_DB_USER / MAPLECAST_DB_PASS  (only for ws/http remote)
const DEFAULT_WS_URL: &str = "ws://localhost:7200";
const DEFAULT_DB_URL: &str = "rocksdb://maplecast.db";

#[derive(Debug, Deserialize)]
struct StatusMsg {
    r#type: Option<String>,
    p1: Option<PlayerStatus>,
    p2: Option<PlayerStatus>,
    spectators: Option<i64>,
    queue: Option<Vec<serde_json::Value>>,
    frame: Option<i64>,
    stream_kbps: Option<i64>,
    publish_us: Option<i64>,
    fps: Option<i64>,
    dirty: Option<i64>,
    game: Option<GameState>,
}

#[derive(Debug, Deserialize)]
struct PlayerStatus {
    name: Option<String>,
    connected: Option<bool>,
    r#type: Option<String>,
    pps: Option<i64>,
    cps: Option<i64>,
}

#[derive(Debug, Deserialize)]
struct GameState {
    in_match: Option<bool>,
    timer: Option<i64>,
    stage: Option<i64>,
    p1_chars: Option<Vec<i64>>,
    p2_chars: Option<Vec<i64>>,
    p1_hp: Option<Vec<i64>>,
    p2_hp: Option<Vec<i64>>,
    p1_combo: Option<i64>,
    p2_combo: Option<i64>,
    p1_meter: Option<i64>,
    p2_meter: Option<i64>,
}

#[derive(Debug, Deserialize)]
struct MatchEndMsg {
    r#type: Option<String>,
    winner: Option<i64>,
    winner_name: Option<String>,
    loser_name: Option<String>,
}

/// MVC2 Character Names (index = char_id from memory map +0x001)
const MVC2_CHARS: &[&str] = &[
    "Ryu","Zangief","Guile","Morrigan","Anakaris","Strider","Cyclops","Wolverine",
    "Psylocke","Iceman","Rogue","Captain America","Spider-Man","Hulk","Venom","Dr. Doom",
    "Tron Bonne","Jill","Hayato","Ruby Heart","SonSon","Amingo","Marrow","Cable",
    "Abyss1","Abyss2","Abyss3","Chun-Li","Megaman","Roll","Akuma","B.B. Hood",
    "Felicia","Charlie","Sakura","Dan","Cammy","Dhalsim","M. Bison","Ken",
    "Gambit","Juggernaut","Storm","Sabretooth","Magneto","Shuma-Gorath","War Machine","Silver Samurai",
    "Omega Red","Spiral","Colossus","Iron Man","Sentinel","Blackheart","Thanos","Jin",
];

fn char_name(id: i64) -> &'static str {
    MVC2_CHARS.get(id as usize).copied().unwrap_or("Unknown")
}

fn rank_tier(rating: i64) -> &'static str {
    match rating {
        0..500 => "Rookie",
        500..1000 => "Fighter",
        1000..1500 => "Warrior",
        1500..2000 => "Champion",
        2000..2500 => "Master",
        2500..3000 => "Grand Master",
        _ => "Legend",
    }
}

/// ELO rating change calculation
fn elo_change(winner_rating: i64, loser_rating: i64) -> i64 {
    let k = 32.0_f64;
    let expected = 1.0 / (1.0 + 10.0_f64.powf((loser_rating - winner_rating) as f64 / 400.0));
    (k * (1.0 - expected)).round() as i64
}

/// Match tracking state — accumulates during match, flushes on match end
struct MatchTracker {
    active: bool,
    start_time: std::time::Instant,

    // Players
    p1_name: String,
    p2_name: String,

    // Teams
    p1_chars: Vec<i64>,
    p2_chars: Vec<i64>,
    stage: i64,

    // Health tracking
    p1_initial_hp: Vec<i64>,
    p2_initial_hp: Vec<i64>,
    p1_hp: Vec<i64>,
    p2_hp: Vec<i64>,

    // Combo tracking
    p1_max_combo: i64,
    p2_max_combo: i64,

    // Input tracking
    p1_total_inputs: i64,
    p2_total_inputs: i64,

    // Meter tracking
    p1_peak_meter: i64,
    p2_peak_meter: i64,

    // Match quality detection
    p1_chars_alive: i64,
    p2_chars_alive: i64,
    first_blood_slot: Option<i64>,  // who dealt damage first
    timer_at_end: i64,
}

impl Default for MatchTracker {
    fn default() -> Self {
        Self {
            active: false,
            start_time: std::time::Instant::now(),
            p1_name: String::new(),
            p2_name: String::new(),
            p1_chars: vec![],
            p2_chars: vec![],
            stage: 0,
            p1_initial_hp: vec![],
            p2_initial_hp: vec![],
            p1_hp: vec![144, 144, 144],
            p2_hp: vec![144, 144, 144],
            p1_max_combo: 0,
            p2_max_combo: 0,
            p1_total_inputs: 0,
            p2_total_inputs: 0,
            p1_peak_meter: 0,
            p2_peak_meter: 0,
            p1_chars_alive: 3,
            p2_chars_alive: 3,
            first_blood_slot: None,
            timer_at_end: 99,
        }
    }
}

impl MatchTracker {
    fn reset(&mut self) {
        *self = Self::default();
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("[collector] MapleCast Data Collector (Rust) starting...");

    let ws_url = std::env::var("MAPLECAST_WS").unwrap_or_else(|_| DEFAULT_WS_URL.to_string());
    let db_url = std::env::var("MAPLECAST_DB").unwrap_or_else(|_| DEFAULT_DB_URL.to_string());

    // Open SurrealDB — local rocksdb OR remote ws://
    let db: Surreal<Any> = any::connect(&db_url).await?;
    println!("[collector] SurrealDB connected: {db_url}");

    // Sign in if remote
    if db_url.starts_with("ws://") || db_url.starts_with("wss://") || db_url.starts_with("http") {
        let user = std::env::var("MAPLECAST_DB_USER").unwrap_or_else(|_| "root".to_string());
        let pass = std::env::var("MAPLECAST_DB_PASS").unwrap_or_else(|_| "root".to_string());
        db.signin(Root { username: user.clone(), password: pass.clone() }).await?;
        println!("[collector] Authenticated as {user}");
    }

    db.use_ns("maplecast").use_db("arcade").await?;

    // Apply schema (idempotent — DEFINE statements are safe to re-run)
    let schema = include_str!("../../schema.surql");
    if let Err(e) = db.query(schema).await {
        eprintln!("[collector] Schema apply note: {e}");
    }
    println!("[collector] Schema ready");

    let mut tracker = MatchTracker::default();

    // Reconnect loop
    loop {
        println!("[collector] Connecting to {ws_url}...");
        match connect_async(&ws_url).await {
            Ok((ws, _)) => {
                println!("[collector] Connected to flycast WS");
                let (_, mut read) = ws.split();

                while let Some(Ok(msg)) = read.next().await {
                    if !msg.is_text() {
                        continue;
                    }
                    let text = msg.into_text().unwrap_or_default();
                    if let Err(e) = handle_message(&db, &text, &mut tracker).await {
                        eprintln!("[collector] Error: {e}");
                    }
                }
                println!("[collector] WS disconnected");
            }
            Err(e) => {
                eprintln!("[collector] Connection failed: {e}");
            }
        }
        println!("[collector] Reconnecting in 2s...");
        tokio::time::sleep(std::time::Duration::from_secs(2)).await;
    }
}

async fn handle_message(
    db: &Surreal<Any>,
    text: &str,
    tracker: &mut MatchTracker,
) -> Result<(), Box<dyn std::error::Error>> {
    let raw: serde_json::Value = serde_json::from_str(text)?;
    let msg_type = raw.get("type").and_then(|t| t.as_str()).unwrap_or("");

    match msg_type {
        "status" => {
            let status: StatusMsg = serde_json::from_str(text)?;
            handle_status(db, &status, tracker).await?;
        }
        "match_end" => {
            let end: MatchEndMsg = serde_json::from_str(text)?;
            handle_match_end(db, &end, tracker).await?;
        }
        _ => {} // Ignore other message types
    }

    Ok(())
}

async fn handle_status(
    db: &Surreal<Any>,
    msg: &StatusMsg,
    tracker: &mut MatchTracker,
) -> Result<(), Box<dyn std::error::Error>> {
    let frame = msg.frame.unwrap_or(0);

    // Only collect data during active matches
    if let Some(game) = &msg.game {
        let in_match = game.in_match.unwrap_or(false);

        // === MATCH START DETECTION ===
        if in_match && !tracker.active {
            tracker.reset();
            tracker.active = true;
            tracker.start_time = std::time::Instant::now();
            tracker.p1_name = msg.p1.as_ref().and_then(|p| p.name.clone()).unwrap_or_default();
            tracker.p2_name = msg.p2.as_ref().and_then(|p| p.name.clone()).unwrap_or_default();
            tracker.p1_chars = game.p1_chars.clone().unwrap_or_default();
            tracker.p2_chars = game.p2_chars.clone().unwrap_or_default();
            tracker.stage = game.stage.unwrap_or(0);
            tracker.p1_initial_hp = game.p1_hp.clone().unwrap_or(vec![144, 144, 144]);
            tracker.p2_initial_hp = game.p2_hp.clone().unwrap_or(vec![144, 144, 144]);
            tracker.p1_hp = tracker.p1_initial_hp.clone();
            tracker.p2_hp = tracker.p2_initial_hp.clone();

            let p1_team: Vec<&str> = tracker.p1_chars.iter().map(|c| char_name(*c)).collect();
            let p2_team: Vec<&str> = tracker.p2_chars.iter().map(|c| char_name(*c)).collect();

            db.query("CREATE game_event SET kind='match_start', data=$d")
                .bind(("d", serde_json::json!({
                    "frame": frame,
                    "p1_name": tracker.p1_name,
                    "p2_name": tracker.p2_name,
                    "p1_chars": tracker.p1_chars,
                    "p2_chars": tracker.p2_chars,
                    "p1_team": p1_team,
                    "p2_team": p2_team,
                    "stage": tracker.stage,
                })))
                .await?;

            println!("[collector] FIGHT! {} ({}) vs {} ({})",
                tracker.p1_name, p1_team.join("/"),
                tracker.p2_name, p2_team.join("/"));
        }

        // === IN-MATCH TRACKING (memory only, no DB writes) ===
        if in_match && tracker.active {
            // Combos
            tracker.p1_max_combo = tracker.p1_max_combo.max(game.p1_combo.unwrap_or(0));
            tracker.p2_max_combo = tracker.p2_max_combo.max(game.p2_combo.unwrap_or(0));

            // Health
            tracker.p1_hp = game.p1_hp.clone().unwrap_or(tracker.p1_hp.clone());
            tracker.p2_hp = game.p2_hp.clone().unwrap_or(tracker.p2_hp.clone());

            // Characters alive
            tracker.p1_chars_alive = tracker.p1_hp.iter().filter(|h| **h > 0).count() as i64;
            tracker.p2_chars_alive = tracker.p2_hp.iter().filter(|h| **h > 0).count() as i64;

            // First blood detection
            if tracker.first_blood_slot.is_none() {
                let p2_took_damage = tracker.p2_hp.iter().zip(&tracker.p2_initial_hp)
                    .any(|(cur, init)| cur < init);
                let p1_took_damage = tracker.p1_hp.iter().zip(&tracker.p1_initial_hp)
                    .any(|(cur, init)| cur < init);
                if p2_took_damage { tracker.first_blood_slot = Some(0); }  // P1 drew first blood
                else if p1_took_damage { tracker.first_blood_slot = Some(1); }
            }

            // Meter
            tracker.p1_peak_meter = tracker.p1_peak_meter.max(game.p1_meter.unwrap_or(0));
            tracker.p2_peak_meter = tracker.p2_peak_meter.max(game.p2_meter.unwrap_or(0));

            // Timer
            tracker.timer_at_end = game.timer.unwrap_or(99);

            // Input accumulation
            for (slot, player) in [(0i64, &msg.p1), (1i64, &msg.p2)] {
                if let Some(p) = player {
                    if p.connected.unwrap_or(false) {
                        let cps = p.cps.unwrap_or(0);
                        if slot == 0 { tracker.p1_total_inputs += cps; }
                        else { tracker.p2_total_inputs += cps; }
                    }
                }
            }
        }

        // === MATCH END DETECTION (state-based fallback) ===
        if !in_match && tracker.active {
            tracker.active = false;
            let duration = tracker.start_time.elapsed().as_secs_f64();
            println!(
                "[collector] K.O.! {:.0}s — P1 combo:{} P2 combo:{} — P1 alive:{} P2 alive:{}",
                duration, tracker.p1_max_combo, tracker.p2_max_combo,
                tracker.p1_chars_alive, tracker.p2_chars_alive
            );
        }
    }

    Ok(())
}

async fn handle_match_end(
    db: &Surreal<Any>,
    msg: &MatchEndMsg,
    tracker: &mut MatchTracker,
) -> Result<(), Box<dyn std::error::Error>> {
    let winner_slot = msg.winner.unwrap_or(-1);
    let winner_name = msg.winner_name.clone().unwrap_or_default();
    let loser_name = msg.loser_name.clone().unwrap_or_default();
    let duration = tracker.start_time.elapsed().as_secs_f64();

    // Determine match quality flags
    let winner_hp = if winner_slot == 0 { &tracker.p1_hp } else { &tracker.p2_hp };
    let loser_hp = if winner_slot == 0 { &tracker.p2_hp } else { &tracker.p1_hp };
    let winner_initial = if winner_slot == 0 { &tracker.p1_initial_hp } else { &tracker.p2_initial_hp };
    let winner_alive = if winner_slot == 0 { tracker.p1_chars_alive } else { tracker.p2_chars_alive };
    let loser_alive = if winner_slot == 0 { tracker.p2_chars_alive } else { tracker.p1_chars_alive };

    let was_perfect = winner_hp.iter().zip(winner_initial.iter()).all(|(cur, init)| cur == init);
    let was_ocv = winner_alive == 3;  // all 3 chars still alive
    let was_comeback = winner_alive == 1 && loser_alive == 0;  // last char standing
    let was_clutch = tracker.timer_at_end < 10;
    let was_timeout = loser_hp.iter().any(|h| *h > 0);  // loser still had health = timeout

    let finish_type = if was_timeout { "timeout" } else { "ko" };

    // === WRITE MATCH RECORD ===
    db.query("CREATE match SET \
        winner_slot=$ws, duration_s=$dur, stage=$stg, finish_type=$ft, \
        p1_name=$p1n, p1_chars=$p1c, p1_hp_final=$p1hp, p1_max_combo=$p1mc, \
        p1_inputs=$p1i, p1_chars_alive=$p1a, \
        p2_name=$p2n, p2_chars=$p2c, p2_hp_final=$p2hp, p2_max_combo=$p2mc, \
        p2_inputs=$p2i, p2_chars_alive=$p2a, \
        was_perfect=$perf, was_ocv=$ocv, was_comeback=$cb, was_clutch=$cl, was_timeout=$to")
        .bind(("ws", winner_slot))
        .bind(("dur", duration))
        .bind(("stg", tracker.stage))
        .bind(("ft", finish_type))
        .bind(("p1n", tracker.p1_name.clone()))
        .bind(("p1c", tracker.p1_chars.clone()))
        .bind(("p1hp", tracker.p1_hp.clone()))
        .bind(("p1mc", tracker.p1_max_combo))
        .bind(("p1i", tracker.p1_total_inputs))
        .bind(("p1a", tracker.p1_chars_alive))
        .bind(("p2n", tracker.p2_name.clone()))
        .bind(("p2c", tracker.p2_chars.clone()))
        .bind(("p2hp", tracker.p2_hp.clone()))
        .bind(("p2mc", tracker.p2_max_combo))
        .bind(("p2i", tracker.p2_total_inputs))
        .bind(("p2a", tracker.p2_chars_alive))
        .bind(("perf", was_perfect))
        .bind(("ocv", was_ocv))
        .bind(("cb", was_comeback))
        .bind(("cl", was_clutch))
        .bind(("to", was_timeout))
        .await?;

    // === UPDATE PLAYER STATS + RATING ===
    // Upsert both players, update W/L, streak, rating, badges
    for (name, won, max_combo, chars) in [
        (&winner_name, true, if winner_slot == 0 { tracker.p1_max_combo } else { tracker.p2_max_combo },
         if winner_slot == 0 { &tracker.p1_chars } else { &tracker.p2_chars }),
        (&loser_name, false, if winner_slot == 0 { tracker.p2_max_combo } else { tracker.p1_max_combo },
         if winner_slot == 0 { &tracker.p2_chars } else { &tracker.p1_chars }),
    ] {
        if name.is_empty() { continue; }
        let name_lower = name.to_lowercase();

        // Skip anonymous players — no stats for unregistered users
        let anon_prefixes = ["wanderer_","drifter_","nomad_","ronin_","ghost_",
            "shadow_","vagrant_","stranger_","outlaw_","rogue_","exile_","phantom_",
            "unknown_","nameless_","faceless_"];
        if anon_prefixes.iter().any(|p| name_lower.starts_with(p)) { continue; }

        // Upsert player
        db.query("INSERT INTO player (username, last_seen, total_matches) VALUES ($n, time::now(), 1) \
            ON DUPLICATE KEY UPDATE last_seen=time::now(), total_matches+=1")
            .bind(("n", name_lower.clone()))
            .await?;

        // Update W/L + streak
        if won {
            db.query("UPDATE player SET wins+=1, \
                streak = IF streak >= 0 THEN streak + 1 ELSE 1 END, \
                best_streak = IF streak + 1 > best_streak THEN streak + 1 ELSE best_streak END \
                WHERE username=$n")
                .bind(("n", name_lower.clone()))
                .await?;
        } else {
            db.query("UPDATE player SET losses+=1, \
                streak = IF streak <= 0 THEN streak - 1 ELSE -1 END, \
                worst_streak = IF streak - 1 < worst_streak THEN streak - 1 ELSE worst_streak END \
                WHERE username=$n")
                .bind(("n", name_lower.clone()))
                .await?;
        }

        // Update best combo
        if max_combo > 0 {
            db.query("UPDATE player SET \
                best_combo = IF $mc > best_combo THEN $mc ELSE best_combo END \
                WHERE username=$n")
                .bind(("n", name_lower.clone()))
                .bind(("mc", max_combo))
                .await?;
        }

        // Match quality stats (winner only)
        if won {
            if was_perfect {
                db.query("UPDATE player SET perfects+=1 WHERE username=$n")
                    .bind(("n", name_lower.clone())).await?;
            }
            if was_ocv {
                db.query("UPDATE player SET ocvs+=1 WHERE username=$n")
                    .bind(("n", name_lower.clone())).await?;
            }
            if was_comeback {
                db.query("UPDATE player SET comebacks+=1 WHERE username=$n")
                    .bind(("n", name_lower.clone())).await?;
            }
            if was_clutch {
                db.query("UPDATE player SET clutch_wins+=1 WHERE username=$n")
                    .bind(("n", name_lower.clone())).await?;
            }
            if tracker.first_blood_slot == Some(winner_slot) {
                db.query("UPDATE player SET first_bloods+=1 WHERE username=$n")
                    .bind(("n", name_lower.clone())).await?;
            }
        }

        // === CHARACTER STATS ===
        for char_id in chars.iter() {
            let cname = char_name(*char_id).to_string();
            db.query("INSERT INTO char_stats (username, char_id, char_name, games, wins, losses) \
                VALUES ($u, $cid, $cn, 1, $w, $l) \
                ON DUPLICATE KEY UPDATE games+=1, wins+=$w, losses+=$l")
                .bind(("u", name_lower.clone()))
                .bind(("cid", *char_id))
                .bind(("cn", cname))
                .bind(("w", if won { 1i64 } else { 0i64 }))
                .bind(("l", if won { 0i64 } else { 1i64 }))
                .await?;
        }

        // === TEAM STATS ===
        if chars.len() == 3 {
            let mut sorted = chars.clone();
            sorted.sort();
            let team_key = format!("{}_{}_{}", sorted[0], sorted[1], sorted[2]);
            let names: Vec<String> = chars.iter().map(|c| char_name(*c).to_string()).collect();

            db.query("INSERT INTO team_stats (username, team_key, char1, char2, char3, \
                char1_name, char2_name, char3_name, games, wins, losses) \
                VALUES ($u, $tk, $c1, $c2, $c3, $n1, $n2, $n3, 1, $w, $l) \
                ON DUPLICATE KEY UPDATE games+=1, wins+=$w, losses+=$l")
                .bind(("u", name_lower.clone()))
                .bind(("tk", team_key))
                .bind(("c1", chars[0]))
                .bind(("c2", chars[1]))
                .bind(("c3", chars[2]))
                .bind(("n1", names[0].clone()))
                .bind(("n2", names[1].clone()))
                .bind(("n3", names[2].clone()))
                .bind(("w", if won { 1i64 } else { 0i64 }))
                .bind(("l", if won { 0i64 } else { 1i64 }))
                .await?;
        }
    }

    // === HEAD TO HEAD ===
    if !winner_name.is_empty() && !loser_name.is_empty() {
        let (p1, p2) = if winner_name < loser_name {
            (winner_name.to_lowercase(), loser_name.to_lowercase())
        } else {
            (loser_name.to_lowercase(), winner_name.to_lowercase())
        };
        let p1_won = winner_name.to_lowercase() == p1;
        db.query("INSERT INTO h2h (player1, player2, p1_wins, p2_wins, last_match) \
            VALUES ($p1, $p2, $w1, $w2, time::now()) \
            ON DUPLICATE KEY UPDATE p1_wins+=$w1, p2_wins+=$w2, last_match=time::now()")
            .bind(("p1", p1))
            .bind(("p2", p2))
            .bind(("w1", if p1_won { 1i64 } else { 0i64 }))
            .bind(("w2", if p1_won { 0i64 } else { 1i64 }))
            .await?;
    }

    // === GAME EVENT ===
    let p1_team: Vec<&str> = tracker.p1_chars.iter().map(|c| char_name(*c)).collect();
    let p2_team: Vec<&str> = tracker.p2_chars.iter().map(|c| char_name(*c)).collect();
    db.query("CREATE game_event SET kind='match_end', data=$d")
        .bind(("d", serde_json::json!({
            "winner": &winner_name,
            "loser": &loser_name,
            "winner_slot": winner_slot,
            "duration_s": duration,
            "p1_team": p1_team,
            "p2_team": p2_team,
            "p1_max_combo": tracker.p1_max_combo,
            "p2_max_combo": tracker.p2_max_combo,
            "perfect": was_perfect,
            "ocv": was_ocv,
            "comeback": was_comeback,
            "clutch": was_clutch,
            "timeout": was_timeout,
        })))
        .await?;

    // Print results
    let mut tags = vec![];
    if was_perfect { tags.push("PERFECT"); }
    if was_ocv { tags.push("OCV"); }
    if was_comeback { tags.push("COMEBACK"); }
    if was_clutch { tags.push("CLUTCH"); }
    if was_timeout { tags.push("TIMEOUT"); }
    let tag_str = if tags.is_empty() { String::new() } else { format!(" [{}]", tags.join(", ")) };

    println!("[collector] {} WINS!{} ({:.0}s) — combo P1:{} P2:{}",
        winner_name, tag_str, duration, tracker.p1_max_combo, tracker.p2_max_combo);

    tracker.reset();
    Ok(())
}
