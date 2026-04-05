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
use surrealdb::engine::local::RocksDb;
use surrealdb::Surreal;
use tokio_tungstenite::connect_async;

const WS_URL: &str = "ws://localhost:7200";
const DB_PATH: &str = "maplecast.db";

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

/// Match tracking state
struct MatchTracker {
    active: bool,
    p1_max_combo: i64,
    p2_max_combo: i64,
    p1_total_inputs: i64,
    p2_total_inputs: i64,
}

impl Default for MatchTracker {
    fn default() -> Self {
        Self {
            active: false,
            p1_max_combo: 0,
            p2_max_combo: 0,
            p1_total_inputs: 0,
            p2_total_inputs: 0,
        }
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("[collector] MapleCast Data Collector (Rust) starting...");

    // Open embedded SurrealDB with RocksDB persistence
    let db = Surreal::new::<RocksDb>(DB_PATH).await?;
    db.use_ns("maplecast").use_db("arcade").await?;
    println!("[collector] SurrealDB opened at {DB_PATH}");

    // Apply schema
    let schema = include_str!("../../schema.surql");
    if let Err(e) = db.query(schema).await {
        // Schema already exists on subsequent runs — that's fine
        eprintln!("[collector] Schema apply note: {e}");
    }
    println!("[collector] Schema ready");

    let mut tracker = MatchTracker::default();

    // Reconnect loop
    loop {
        println!("[collector] Connecting to {WS_URL}...");
        match connect_async(WS_URL).await {
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
    db: &Surreal<surrealdb::engine::local::Db>,
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
    db: &Surreal<surrealdb::engine::local::Db>,
    msg: &StatusMsg,
    tracker: &mut MatchTracker,
) -> Result<(), Box<dyn std::error::Error>> {
    let frame = msg.frame.unwrap_or(0);

    // Stream telemetry
    db.query("CREATE stream_snapshot SET fps=$fps, kbps=$kbps, publish_us=$pus, dirty_pages=$dirty, frame=$frame, spectators=$spec")
        .bind(("fps", msg.fps.unwrap_or(0)))
        .bind(("kbps", msg.stream_kbps.unwrap_or(0)))
        .bind(("pus", msg.publish_us.unwrap_or(0)))
        .bind(("dirty", msg.dirty.unwrap_or(0)))
        .bind(("frame", frame))
        .bind(("spec", msg.spectators.unwrap_or(0)))
        .await?;

    // Input snapshots
    for (slot, player) in [(0i64, &msg.p1), (1i64, &msg.p2)] {
        if let Some(p) = player {
            if p.connected.unwrap_or(false) {
                let pps = p.pps.unwrap_or(0);
                let cps = p.cps.unwrap_or(0);

                db.query("CREATE input_snapshot SET slot=$slot, player_name=$name, pps=$pps, cps=$cps, buttons=0, lt=0, rt=0, input_type=$itype")
                    .bind(("slot", slot))
                    .bind(("name", p.name.clone().unwrap_or_default()))
                    .bind(("pps", pps))
                    .bind(("cps", cps))
                    .bind(("itype", p.r#type.clone().unwrap_or_default()))
                    .await?;

                if slot == 0 {
                    tracker.p1_total_inputs += cps;
                } else {
                    tracker.p2_total_inputs += cps;
                }
            }
        }
    }

    // Game state
    if let Some(game) = &msg.game {
        let in_match = game.in_match.unwrap_or(false);

        db.query("CREATE game_snapshot SET frame=$frame, in_match=$im, timer=$t, stage=$s, p1_chars=$p1c, p2_chars=$p2c, p1_hp=$p1h, p2_hp=$p2h, p1_combo=$p1co, p2_combo=$p2co, p1_meter=$p1m, p2_meter=$p2m")
            .bind(("frame", frame))
            .bind(("im", in_match))
            .bind(("t", game.timer))
            .bind(("s", game.stage))
            .bind(("p1c", game.p1_chars.clone()))
            .bind(("p2c", game.p2_chars.clone()))
            .bind(("p1h", game.p1_hp.clone()))
            .bind(("p2h", game.p2_hp.clone()))
            .bind(("p1co", game.p1_combo))
            .bind(("p2co", game.p2_combo))
            .bind(("p1m", game.p1_meter))
            .bind(("p2m", game.p2_meter))
            .await?;

        tracker.p1_max_combo = tracker.p1_max_combo.max(game.p1_combo.unwrap_or(0));
        tracker.p2_max_combo = tracker.p2_max_combo.max(game.p2_combo.unwrap_or(0));

        // Match start detection
        if in_match && !tracker.active {
            tracker.active = true;
            tracker.p1_max_combo = 0;
            tracker.p2_max_combo = 0;
            tracker.p1_total_inputs = 0;
            tracker.p2_total_inputs = 0;

            let p1_name = msg.p1.as_ref().and_then(|p| p.name.clone()).unwrap_or_default();
            let p2_name = msg.p2.as_ref().and_then(|p| p.name.clone()).unwrap_or_default();

            db.query("CREATE game_event SET kind='match_start', data=$d")
                .bind(("d", serde_json::json!({
                    "frame": frame,
                    "p1_name": p1_name,
                    "p2_name": p2_name,
                    "p1_chars": game.p1_chars,
                    "p2_chars": game.p2_chars,
                    "stage": game.stage,
                })))
                .await?;

            println!("[collector] Match started: {p1_name} vs {p2_name}");
        }

        // Match end detection (state-based)
        if !in_match && tracker.active {
            tracker.active = false;
            println!(
                "[collector] Match ended (combos: P1={}, P2={})",
                tracker.p1_max_combo, tracker.p2_max_combo
            );
        }
    }

    Ok(())
}

async fn handle_match_end(
    db: &Surreal<surrealdb::engine::local::Db>,
    msg: &MatchEndMsg,
    tracker: &mut MatchTracker,
) -> Result<(), Box<dyn std::error::Error>> {
    let winner = msg.winner.unwrap_or(-1);
    let winner_name = msg.winner_name.clone().unwrap_or_default();
    let loser_name = msg.loser_name.clone().unwrap_or_default();

    let (p1_name, p2_name) = if winner == 0 {
        (winner_name.clone(), loser_name.clone())
    } else {
        (loser_name.clone(), winner_name.clone())
    };

    db.query("CREATE match SET winner_slot=$ws, p1_name=$p1, p2_name=$p2, p1_max_combo=$p1c, p2_max_combo=$p2c, p1_inputs=$p1i, p2_inputs=$p2i")
        .bind(("ws", winner))
        .bind(("p1", p1_name))
        .bind(("p2", p2_name))
        .bind(("p1c", tracker.p1_max_combo))
        .bind(("p2c", tracker.p2_max_combo))
        .bind(("p1i", tracker.p1_total_inputs))
        .bind(("p2i", tracker.p2_total_inputs))
        .await?;

    let event_data = serde_json::json!({
        "winner_slot": winner,
        "winner_name": &winner_name,
        "loser_name": &loser_name,
        "p1_max_combo": tracker.p1_max_combo,
        "p2_max_combo": tracker.p2_max_combo,
    });
    db.query("CREATE game_event SET kind='match_end', data=$d")
        .bind(("d", event_data))
        .await?;

    println!("[collector] Match recorded: {winner_name} wins!");

    // Reset
    tracker.p1_max_combo = 0;
    tracker.p2_max_combo = 0;
    tracker.p1_total_inputs = 0;
    tracker.p2_total_inputs = 0;

    Ok(())
}
