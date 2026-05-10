// hub/src/queue.rs — Phase 1 matchmaking queue.
//
// Pairs two players FIFO, picks the best server via the existing
// matchmaker::select_node algorithm, and parks both entries with
// `Matched { server, slot, partner }` so play.html's status-poll
// returns the answer.
//
// See docs/MATCHMAKING.md for the full Phase 1 design.

use crate::matchmaker::select_node;
use crate::types::{PingResult, SharedStore};
use axum::{Json, extract::{Query, State}, http::StatusCode, response::IntoResponse};
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, VecDeque};
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::sync::Mutex;
use tracing::info;
use uuid::Uuid;

// ── State ─────────────────────────────────────────────────────────────

#[derive(Debug, Clone, Serialize)]
#[serde(tag = "state", rename_all = "lowercase")]
pub enum QueueState {
    Queueing { position: usize, queue_size: usize },
    Matched {
        server: MatchedServer,
        slot: u8,
        partner: String,
    },
    Cancelled,
    Expired,
}

#[derive(Debug, Clone, Serialize)]
pub struct MatchedServer {
    pub node_id: String,
    pub name: String,
    pub region: String,
    pub host: String,            // public_host, used by native client URL
    pub flycast_ws_port: u16,    // relay_ws port — native client connects here
    pub relay_url: String,       // wss://...:port/ws — browser-facing
    pub control_url: String,     // wss://...:port/play — browser direct-flycast
    pub audio_url: String,       // wss://...:port/audio
}

#[derive(Debug, Clone)]
struct QueueEntry {
    name: String,
    rtts: HashMap<String, f64>,  // node_id -> avg_rtt_ms (browser-side probes)
    joined_at: Instant,
    state: QueueState,
}

pub struct QueueStore {
    fifo: VecDeque<Uuid>,
    entries: HashMap<Uuid, QueueEntry>,
}

impl QueueStore {
    pub fn new() -> Self {
        Self { fifo: VecDeque::new(), entries: HashMap::new() }
    }
    fn position_of(&self, token: &Uuid) -> Option<usize> {
        self.fifo.iter().position(|t| t == token).map(|i| i + 1)
    }
}

pub type SharedQueue = Arc<Mutex<QueueStore>>;

// ── Request / response shapes ─────────────────────────────────────────

#[derive(Debug, Deserialize)]
pub struct JoinRequest {
    pub name: String,
    /// node_id -> avg_rtt_ms. Optional. Fed into the min-max
    /// matchmaker; if both players omit it, we fall back to picking
    /// the first ready node.
    #[serde(default)]
    pub rtts: HashMap<String, f64>,
}

#[derive(Debug, Serialize)]
pub struct JoinResponse {
    pub token: String,
    pub position: usize,
    pub queue_size: usize,
}

#[derive(Debug, Deserialize)]
pub struct StatusQuery {
    pub token: String,
}

#[derive(Debug, Deserialize)]
pub struct LeaveRequest {
    pub token: String,
}

// ── Helpers ───────────────────────────────────────────────────────────

fn rtts_to_pings(rtts: &HashMap<String, f64>) -> Vec<PingResult> {
    rtts.iter().map(|(id, ms)| PingResult {
        node_id:    id.clone(),
        avg_rtt_ms: *ms,
        p95_rtt_ms: *ms,  // Phase 1 doesn't separate p95
    }).collect()
}

// ── Handlers ──────────────────────────────────────────────────────────

pub async fn join(
    State(queue): State<SharedQueue>,
    Json(req): Json<JoinRequest>,
) -> impl IntoResponse {
    let name = req.name.trim();
    if name.is_empty() || name.len() > 32 {
        return (StatusCode::BAD_REQUEST,
                Json(serde_json::json!({"error": "name must be 1-32 chars"})))
                .into_response();
    }
    let token = Uuid::new_v4();
    let mut q = queue.lock().await;
    let position = q.fifo.len() + 1;
    let queue_size = position;
    q.fifo.push_back(token);
    q.entries.insert(token, QueueEntry {
        name: name.to_string(),
        rtts: req.rtts,
        joined_at: Instant::now(),
        state: QueueState::Queueing { position, queue_size },
    });
    info!("queue: join name='{}' token={} pos={}", name, token, position);
    (StatusCode::OK, Json(JoinResponse {
        token: token.to_string(),
        position,
        queue_size,
    })).into_response()
}

pub async fn status(
    State(queue): State<SharedQueue>,
    Query(q): Query<StatusQuery>,
) -> impl IntoResponse {
    let token = match Uuid::parse_str(&q.token) {
        Ok(t) => t,
        Err(_) => return (StatusCode::BAD_REQUEST,
                          Json(serde_json::json!({"error": "invalid token"})))
                          .into_response(),
    };
    let store = queue.lock().await;
    let Some(entry) = store.entries.get(&token) else {
        return (StatusCode::OK,
                Json(serde_json::json!({"state": "expired"})))
                .into_response();
    };
    let state = match &entry.state {
        QueueState::Queueing { .. } => {
            let pos = store.position_of(&token).unwrap_or(0);
            QueueState::Queueing { position: pos, queue_size: store.fifo.len() }
        }
        other => other.clone(),
    };
    (StatusCode::OK, Json(state)).into_response()
}

pub async fn leave(
    State(queue): State<SharedQueue>,
    Json(req): Json<LeaveRequest>,
) -> impl IntoResponse {
    let token = match Uuid::parse_str(&req.token) {
        Ok(t) => t,
        Err(_) => return StatusCode::BAD_REQUEST.into_response(),
    };
    let mut q = queue.lock().await;
    q.fifo.retain(|t| *t != token);
    if let Some(e) = q.entries.get_mut(&token) {
        e.state = QueueState::Cancelled;
    }
    info!("queue: leave token={}", token);
    (StatusCode::OK, Json(serde_json::json!({"ok": true}))).into_response()
}

// ── Pair-and-notify task ──────────────────────────────────────────────

pub async fn pair_and_notify(queue: SharedQueue, store: SharedStore) {
    let match_ttl = Duration::from_secs(120);
    let queue_ttl = Duration::from_secs(300);
    let mut tick = tokio::time::interval(Duration::from_secs(1));
    loop {
        tick.tick().await;

        // Step 1: GC stale entries.
        {
            let mut q = queue.lock().await;
            let now = Instant::now();
            let stale: Vec<Uuid> = q.entries.iter().filter_map(|(k, v)| {
                let age = now.duration_since(v.joined_at);
                let drop = match v.state {
                    QueueState::Matched { .. } | QueueState::Cancelled
                    | QueueState::Expired => age > match_ttl,
                    QueueState::Queueing { .. } => age > queue_ttl,
                };
                if drop { Some(*k) } else { None }
            }).collect();
            for k in stale {
                q.fifo.retain(|t| *t != k);
                q.entries.remove(&k);
            }
            if q.fifo.len() < 2 { continue; }
        }

        // Step 2: pop two queue heads under lock.
        let (a_token, b_token, a_name, b_name, a_rtts, b_rtts) = {
            let mut q = queue.lock().await;
            if q.fifo.len() < 2 { continue; }
            let a_token = q.fifo.pop_front().unwrap();
            let b_token = q.fifo.pop_front().unwrap();
            let (a_name, a_rtts) = match q.entries.get(&a_token) {
                Some(e) => (e.name.clone(), e.rtts.clone()),
                None => continue,
            };
            let (b_name, b_rtts) = match q.entries.get(&b_token) {
                Some(e) => (e.name.clone(), e.rtts.clone()),
                None => {
                    // a was valid; restore it so it doesn't get lost
                    q.fifo.push_front(a_token);
                    continue;
                }
            };
            (a_token, b_token, a_name, b_name, a_rtts, b_rtts)
        };

        // Step 3: read server registry.
        let nodes = {
            let s = store.read().await;
            s.nodes.values().cloned().collect::<Vec<_>>()
        };
        if nodes.is_empty() {
            // Re-queue at front; nothing to match against.
            let mut q = queue.lock().await;
            q.fifo.push_front(b_token);
            q.fifo.push_front(a_token);
            continue;
        }

        // Step 4: pick the best server.
        let p1_pings = rtts_to_pings(&a_rtts);
        let p2_pings = rtts_to_pings(&b_rtts);
        let nodes_ref: Vec<&_> = nodes.iter().collect();
        let chosen = select_node(&p1_pings, &p2_pings, &nodes_ref);
        let chosen_node = match chosen {
            Some((node_id, _urls, _worst)) => {
                nodes.iter().find(|n| n.node_id == node_id).cloned()
            }
            None => {
                // No common-pinged node. Fall back to the first ready
                // node so Phase 1 still works without client probes.
                nodes.iter().find(|n| n.status == "ready").cloned()
                    .or_else(|| nodes.first().cloned())
            }
        };
        let Some(node) = chosen_node else {
            let mut q = queue.lock().await;
            q.fifo.push_front(b_token);
            q.fifo.push_front(a_token);
            continue;
        };

        let server = MatchedServer {
            node_id:         node.node_id.clone(),
            name:            node.name.clone(),
            region:          node.region.clone(),
            host:            node.public_host.clone(),
            flycast_ws_port: node.ports.relay_ws,
            relay_url:       node.relay_url(),
            control_url:     node.control_url(),
            audio_url:       node.audio_url(),
        };

        // Step 5: write Matched into both entries.
        let mut q = queue.lock().await;
        if let Some(e) = q.entries.get_mut(&a_token) {
            e.state = QueueState::Matched {
                server: server.clone(),
                slot: 0,
                partner: b_name.clone(),
            };
        }
        if let Some(e) = q.entries.get_mut(&b_token) {
            e.state = QueueState::Matched {
                server: server.clone(),
                slot: 1,
                partner: a_name.clone(),
            };
        }
        info!("queue: paired {} <-> {} -> {}", a_name, b_name, server.name);
    }
}
