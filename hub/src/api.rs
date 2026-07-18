use axum::{
    Json,
    extract::{Path, Query, State, ConnectInfo},
    http::StatusCode,
    response::IntoResponse,
    response::sse::{Event, KeepAlive, Sse},
};
use chrono::Utc;
use serde::Deserialize;
use std::convert::Infallible;
use std::net::SocketAddr;
use std::time::Duration;
use tokio_stream::{wrappers::BroadcastStream, Stream, StreamExt};
use tracing::{info, warn};

use crate::geo;
use crate::matchmaker;
use crate::types::*;

// ============================================================================
// Auth helper — validate operator token (plain-text comparison for now,
// upgrade to argon2 hash check when operators table is in SurrealDB)
// ============================================================================

// Short stable pseudonymous operator id derived from the (secret) token — just a
// display label for the map, not cryptographic.
fn short_id(token: &str) -> String {
    use std::hash::{Hash, Hasher};
    let mut h = std::collections::hash_map::DefaultHasher::new();
    token.hash(&mut h);
    format!("{:08x}", h.finish() as u32)
}

// OPEN-registration anti-spam: at most LIMIT NEW nodes per source IP per hour.
// Returns true if the caller is over the limit and should be rejected.
fn reg_rate_limited(store: &mut HubStore, ip: std::net::IpAddr, now: chrono::DateTime<chrono::Utc>) -> bool {
    const LIMIT: usize = 20;
    let cutoff = now.timestamp() - 3600;
    let v = store.recent_regs.entry(ip).or_default();
    v.retain(|&t| t > cutoff);
    if v.len() >= LIMIT {
        return true;
    }
    v.push(now.timestamp());
    false
}

// Ownership: only the holder of a node's self-issued token may update/remove it.
fn token_owns_node(store: &HubStore, token: &str, node_id: &str) -> bool {
    match store.nodes.get(node_id) {
        Some(node) => store
            .operators
            .get(&node.operator_name)
            .map(|op| op.token_hash == token)
            .unwrap_or(false),
        None => false,
    }
}

// ============================================================================
// POST /hub/api/nodes/register — register a new game server node
// ============================================================================

pub async fn register_node(
    State(store): State<SharedStore>,
    State(events): State<SharedEvents>,
    ConnectInfo(addr): ConnectInfo<SocketAddr>,
    Json(req): Json<RegisterRequest>,
) -> impl IntoResponse {
    let mut s = store.write().await;
    let now = Utc::now();

    // OPEN registration: identity IS the operator_token (a self-issued secret the
    // node persists, like its node_id). Reuse an existing operator if the token
    // matches, else auto-create one — no approval gate. Anti-spam: cap NEW nodes
    // per source IP per hour. The client-side ROM-hash badge flags non-canonical
    // ROMs; hosting is casual-open (ranked stays on trusted nodes).
    let is_new_node = !s.nodes.contains_key(&req.node_id);
    let operator_name = match s
        .operators
        .values()
        .find(|op| op.token_hash == req.operator_token)
        .map(|op| op.name.clone())
    {
        Some(name) => name,
        None => {
            if is_new_node && reg_rate_limited(&mut s, addr.ip(), now) {
                warn!("Registration rate-limited for {}", addr);
                return (
                    StatusCode::TOO_MANY_REQUESTS,
                    Json(serde_json::json!({"ok": false, "error": "too many new nodes from your address — try again later"})),
                );
            }
            let name = format!("op-{}", short_id(&req.operator_token));
            s.operators.insert(
                name.clone(),
                Operator {
                    name: name.clone(),
                    token_hash: req.operator_token.clone(),
                    approved: true,
                    max_nodes: 25,
                    created_at: now,
                },
            );
            info!("New operator self-registered: {} from {}", name, addr);
            name
        }
    };

    // Per-operator node cap (excludes this node_id so re-registration is free).
    let operator_node_count = s
        .nodes
        .values()
        .filter(|n| n.operator_name == operator_name && n.status != "offline" && n.node_id != req.node_id)
        .count() as u32;
    let max_nodes = s
        .operators
        .get(&operator_name)
        .map(|o| o.max_nodes)
        .unwrap_or(25);
    if operator_node_count >= max_nodes {
        return (
            StatusCode::BAD_REQUEST,
            Json(serde_json::json!({"ok": false, "error": "node limit reached for this operator"})),
        );
    }

    // Determine public host — use provided, or fall back to request IP
    let public_host = req
        .public_host
        .unwrap_or_else(|| addr.ip().to_string());

    // GeoIP lookup (async, non-blocking — node registers even if this fails)
    let geo_ip = public_host.clone();
    drop(s); // release lock during network call
    let geo = geo::lookup_ip(&geo_ip).await;
    let mut s = store.write().await;

    let node = Node {
        node_id: req.node_id.clone(),
        operator_name: operator_name.clone(),
        name: req.name,
        region: req.region,
        public_host,
        ports: req.ports,
        tls: req.tls,
        capacity: req.capacity,
        rom_hash: req.rom_hash,
        version: req.version,
        status: "ready".to_string(),
        geo: geo.clone(),
        metrics: None,
        stats: NodeStats {
            total_matches: 0,
            total_frames: 0,
            uptime_s: 0,
        },
        last_heartbeat: now,
        registered_at: now,
        stale_count: 0,
        public_relay_url: req.public_relay_url,
        public_control_url: req.public_control_url,
        public_audio_url: req.public_audio_url,
        game: None,
    };

    info!(
        "Node registered: {} ({}) by {} at {}",
        node.name, req.node_id, operator_name,
        geo.as_ref().map(|g| format!("{}, {}", g.city, g.country)).unwrap_or_else(|| "unknown".into())
    );

    s.nodes.insert(req.node_id.clone(), node);

    // Live feed: a node just joined the mesh — push it to every open map.
    let _ = events.send(NodeEvent { kind: "joined", node: node_to_public(&s.nodes[&req.node_id]) });

    (
        StatusCode::OK,
        Json(serde_json::json!({
            "ok": true,
            "node_id": req.node_id,
            "geo": geo,
        })),
    )
}

// ============================================================================
// POST /hub/api/nodes/:id/heartbeat — update node status + metrics
// ============================================================================

#[derive(Debug, Deserialize)]
pub struct HeartbeatAuth {
    pub operator_token: String,
}

pub async fn heartbeat(
    State(store): State<SharedStore>,
    State(events): State<SharedEvents>,
    Path(node_id): Path<String>,
    Json(req): Json<HeartbeatPayload>,
) -> impl IntoResponse {
    let mut s = store.write().await;

    if !s.nodes.contains_key(&node_id) {
        return (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({"ok": false, "error": "node not found"})),
        );
    }
    if !token_owns_node(&s, &req.operator_token, &node_id) {
        return (
            StatusCode::FORBIDDEN,
            Json(serde_json::json!({"ok": false, "error": "token does not own this node"})),
        );
    }
    let node: &mut Node = s.nodes.get_mut(&node_id).unwrap();

    node.status = req.status;
    node.metrics = Some(req.metrics);
    node.stats = req.stats;
    node.game = req.game;
    node.last_heartbeat = Utc::now();
    // Reset stale count on successful heartbeat
    if node.status != "offline" {
        node.stale_count = 0;
    }

    // Live feed: refreshed status + metrics (spectators, frame flow) for this node.
    let _ = events.send(NodeEvent { kind: "updated", node: node_to_public(node) });

    (
        StatusCode::OK,
        Json(serde_json::json!({"ok": true})),
    )
}

#[derive(Debug, Deserialize)]
pub struct HeartbeatPayload {
    pub operator_token: String,
    pub status: String,
    pub metrics: NodeMetrics,
    pub stats: NodeStats,
    /// Live in-match state (flycast getStatus.game), forwarded verbatim. Absent when idle.
    #[serde(default)]
    pub game: Option<serde_json::Value>,
}

// ============================================================================
// DELETE /hub/api/nodes/:id — deregister a node (graceful shutdown)
// ============================================================================

#[derive(Debug, Deserialize)]
pub struct DeleteQuery {
    pub operator_token: String,
}

pub async fn deregister_node(
    State(store): State<SharedStore>,
    State(events): State<SharedEvents>,
    Path(node_id): Path<String>,
    Query(q): Query<DeleteQuery>,
) -> impl IntoResponse {
    let mut s = store.write().await;

    if !s.nodes.contains_key(&node_id) {
        return (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({"ok": false, "error": "node not found"})),
        );
    }
    if !token_owns_node(&s, &q.operator_token, &node_id) {
        return (
            StatusCode::FORBIDDEN,
            Json(serde_json::json!({"ok": false, "error": "token does not own this node"})),
        );
    }
    let node = s.nodes.get_mut(&node_id).unwrap();
    info!("Node deregistered: {} ({})", node.name, node_id);
    node.status = "offline".to_string();
    // Live feed: node left the mesh — maps drop its pin.
    let _ = events.send(NodeEvent { kind: "left", node: node_to_public(node) });
    (StatusCode::OK, Json(serde_json::json!({"ok": true})))
}

// ============================================================================
// GET /hub/api/nodes — list all active nodes (public, for browsers)
// ============================================================================

pub async fn list_nodes(State(store): State<SharedStore>) -> impl IntoResponse {
    let s = store.read().await;

    let nodes: Vec<NodePublic> = s
        .nodes
        .values()
        .filter(|n| n.status != "offline")
        .map(|n| node_to_public(n))
        .collect();

    Json(serde_json::json!({ "nodes": nodes }))
}

// ============================================================================
// GET /hub/api/nodes/nearby?lat=X&lng=Y&limit=5 — geographic pre-filter
// ============================================================================

#[derive(Debug, Deserialize)]
pub struct NearbyParams {
    pub lat: Option<f64>,
    pub lng: Option<f64>,
    pub limit: Option<usize>,
}

pub async fn nearby_nodes(
    State(store): State<SharedStore>,
    ConnectInfo(addr): ConnectInfo<SocketAddr>,
    Query(params): Query<NearbyParams>,
) -> impl IntoResponse {
    let s = store.read().await;
    let limit = params.limit.unwrap_or(5).min(20);

    // If lat/lng provided, use those. Otherwise, try to GeoIP the request IP.
    let (lat, lng) = match (params.lat, params.lng) {
        (Some(lat), Some(lng)) => (lat, lng),
        _ => {
            // Fall back: do a quick GeoIP on the requester
            drop(s);
            let geo = geo::lookup_ip(&addr.ip().to_string()).await;
            let s2 = store.read().await;
            match geo {
                Some(g) => (g.lat, g.lng),
                None => {
                    // Can't determine location — return all ready nodes
                    let nodes: Vec<NodePublic> = s2
                        .nodes
                        .values()
                        .filter(|n| n.status == "ready")
                        .take(limit)
                        .map(|n| node_to_public(n))
                        .collect();
                    return Json(serde_json::json!({ "nodes": nodes }));
                }
            }
        }
    };

    let s = store.read().await;
    let mut candidates: Vec<(&Node, f64)> = s
        .nodes
        .values()
        .filter(|n| n.status == "ready")
        .filter_map(|n| {
            n.geo
                .as_ref()
                .map(|g| (n, geo::haversine_km(lat, lng, g.lat, g.lng)))
        })
        .collect();

    candidates.sort_by(|a, b| a.1.partial_cmp(&b.1).unwrap());
    candidates.truncate(limit);

    let nodes: Vec<NodePublic> = candidates.iter().map(|(n, _)| node_to_public(n)).collect();

    Json(serde_json::json!({
        "nodes": nodes,
        "your_location": { "lat": lat, "lng": lng },
    }))
}

// ============================================================================
// POST /hub/api/matchmake — submit ping results, get optimal node
// ============================================================================

pub async fn matchmake(
    State(store): State<SharedStore>,
    Json(req): Json<MatchmakeRequest>,
) -> impl IntoResponse {
    let mut s = store.write().await;

    // Store this player's ping results
    s.pending_pings.insert(
        req.player_id.clone(),
        PendingPings {
            player_id: req.player_id.clone(),
            session_id: req.session_id.clone(),
            pings: req.ping_results,
            submitted_at: Utc::now(),
        },
    );

    info!(
        "Ping results received from player {} ({} nodes probed)",
        req.player_id,
        s.pending_pings[&req.player_id].pings.len()
    );

    // Check if there's another player with pending pings (simple 2-player matchmaking)
    // In production this would integrate with the SurrealDB queue system.
    // For now, return "pending" — the actual matching happens when the queue
    // system queries the hub for the optimal node.
    let resp = MatchmakeResponse {
        node_id: None,
        node_urls: None,
        status: "pending".to_string(),
    };

    (StatusCode::OK, Json(resp))
}

// ============================================================================
// POST /hub/api/matchmake/select — called by collector when two players are
// ready. Provide both player IDs, get back the optimal node.
// ============================================================================

#[derive(Debug, Deserialize)]
pub struct SelectRequest {
    pub player1_id: String,
    pub player2_id: String,
}

pub async fn matchmake_select(
    State(store): State<SharedStore>,
    Json(req): Json<SelectRequest>,
) -> impl IntoResponse {
    let s = store.read().await;

    let p1_pings = match s.pending_pings.get(&req.player1_id) {
        Some(p) => &p.pings,
        None => {
            return (
                StatusCode::BAD_REQUEST,
                Json(serde_json::json!({
                    "ok": false,
                    "error": "no ping results for player1"
                })),
            );
        }
    };

    let p2_pings = match s.pending_pings.get(&req.player2_id) {
        Some(p) => &p.pings,
        None => {
            return (
                StatusCode::BAD_REQUEST,
                Json(serde_json::json!({
                    "ok": false,
                    "error": "no ping results for player2"
                })),
            );
        }
    };

    let ready_nodes: Vec<&Node> = s
        .nodes
        .values()
        .filter(|n| n.status == "ready")
        .collect();

    match matchmaker::select_node(p1_pings, p2_pings, &ready_nodes) {
        Some((node_id, urls, worst_rtt)) => {
            info!(
                "Matchmaker: {} vs {} → node {} (worst RTT {:.1}ms)",
                req.player1_id, req.player2_id, node_id, worst_rtt
            );
            (
                StatusCode::OK,
                Json(serde_json::json!({
                    "ok": true,
                    "node_id": node_id,
                    "node_urls": urls,
                    "worst_rtt_ms": worst_rtt,
                })),
            )
        }
        None => {
            info!(
                "Matchmaker: no suitable node for {} vs {} — use origin server",
                req.player1_id, req.player2_id
            );
            (
                StatusCode::OK,
                Json(serde_json::json!({
                    "ok": true,
                    "node_id": null,
                    "node_urls": null,
                    "fallback": "origin",
                })),
            )
        }
    }
}

// ============================================================================
// GET /hub/api/dashboard/stats — aggregate stats for the dashboard
// ============================================================================

pub async fn dashboard_stats(State(store): State<SharedStore>) -> impl IntoResponse {
    let s = store.read().await;

    let active_nodes: Vec<&Node> = s.nodes.values().filter(|n| n.status != "offline").collect();

    let stats = DashboardStats {
        total_nodes_online: active_nodes.len() as u64,
        total_matches_active: active_nodes
            .iter()
            .filter(|n| n.status == "in_match")
            .count() as u64,
        total_spectators: active_nodes
            .iter()
            .filter_map(|n| n.metrics.as_ref())
            .map(|m| m.clients)
            .sum(),
        total_matches_played: active_nodes.iter().map(|n| n.stats.total_matches).sum(),
    };

    Json(stats)
}

// ============================================================================
// GET /hub/api/dashboard/nodes — full node list with geo for map rendering
// ============================================================================

pub async fn dashboard_nodes(State(store): State<SharedStore>) -> impl IntoResponse {
    let s = store.read().await;

    let nodes: Vec<NodePublic> = s
        .nodes
        .values()
        .filter(|n| n.status != "offline")
        .map(|n| node_to_public(n))
        .collect();

    Json(serde_json::json!({ "nodes": nodes }))
}

// ============================================================================
// GET /hub/api/events — Server-Sent Events: live node join/update/leave feed.
// The map subscribes here (EventSource) so pins pop in instantly instead of
// waiting for the 5s poll; the poll stays as a reconcile/fallback. Each SSE
// message is a JSON NodeEvent {kind, node}. A subscriber that lags past the
// channel buffer just drops events and re-syncs on its next poll.
// ============================================================================

pub async fn events(
    State(bus): State<SharedEvents>,
) -> Sse<impl Stream<Item = Result<Event, Infallible>>> {
    let rx = bus.subscribe();
    let stream = BroadcastStream::new(rx).filter_map(|r| {
        let evt = r.ok()?; // skip a lagged receiver's error frame
        let e = Event::default().json_data(&evt).ok()?;
        Some(Ok::<Event, Infallible>(e))
    });
    Sse::new(stream).keep_alive(KeepAlive::new().interval(Duration::from_secs(15)))
}

// ============================================================================
// Helpers
// ============================================================================

fn node_to_public(n: &Node) -> NodePublic {
    NodePublic {
        node_id: n.node_id.clone(),
        name: n.name.clone(),
        operator: n.operator_name.clone(),
        region: n.region.clone(),
        public_host: n.public_host.clone(),
        ports: n.ports.clone(),
        tls: n.tls,
        status: n.status.clone(),
        geo: n.geo.clone(),
        spectators: n.metrics.as_ref().map(|m| m.clients).unwrap_or(0),
        stats: n.stats.clone(),
        uptime_s: n.stats.uptime_s,
        rom_hash: n.rom_hash.clone(),
        version: n.version.clone(),
        relay_url: n.relay_url(),
        metrics: n.metrics.clone(),
        game: n.game.clone(),
    }
}

// ============================================================================
// GET /hub/api/matches/active — list input servers currently in a match.
// Used by spectator mode to discover what's live.
// ============================================================================

pub async fn active_matches(State(store): State<SharedStore>) -> impl IntoResponse {
    let s = store.read().await;

    // Every input server that reports status="in_match" OR has
    // frames_received > 0 (i.e., actively streaming) is a candidate for
    // spectating. status="ready" but idle servers are skipped.
    let matches: Vec<_> = s
        .nodes
        .values()
        .filter(|n| n.status == "in_match" || {
            // Fallback: consider any node streaming frames as having an
            // active match (until we have proper match-begin/end signals
            // from the flycast process)
            n.metrics.as_ref().map(|m| m.frames_received > 0).unwrap_or(false)
        })
        .map(|n| serde_json::json!({
            "server_id": n.node_id,
            "server_name": n.name,
            "region": n.region,
            "geo": n.geo,
            "relay_url": n.relay_url(),
            "spectators": n.metrics.as_ref().map(|m| m.clients).unwrap_or(0),
            "frames": n.metrics.as_ref().map(|m| m.frames_received).unwrap_or(0),
            "game": n.game,
        }))
        .collect();

    Json(serde_json::json!({ "matches": matches }))
}

// ============================================================================
// Stale node sweeper — runs as a background tokio task
// ============================================================================

pub async fn stale_sweeper(store: SharedStore, events: SharedEvents) {
    let mut interval = tokio::time::interval(std::time::Duration::from_secs(10));

    loop {
        interval.tick().await;
        let now = Utc::now();
        let mut s = store.write().await;
        let mut transitions: Vec<NodeEvent> = Vec::new();

        for node in s.nodes.values_mut() {
            if node.status == "offline" {
                continue;
            }

            let age = now
                .signed_duration_since(node.last_heartbeat)
                .num_seconds();

            if age > 60 {
                if node.status != "offline" {
                    warn!(
                        "Node {} ({}) → offline (no heartbeat for {}s)",
                        node.name, node.node_id, age
                    );
                    node.status = "offline".to_string();
                    node.stale_count += 1;
                    // Live feed: node went dark — maps drop its pin.
                    transitions.push(NodeEvent { kind: "left", node: node_to_public(node) });
                }
            } else if age > 30 {
                if node.status != "stale" {
                    warn!(
                        "Node {} ({}) → stale (no heartbeat for {}s)",
                        node.name, node.node_id, age
                    );
                    node.status = "stale".to_string();
                    transitions.push(NodeEvent { kind: "updated", node: node_to_public(node) });
                }
            }
        }

        // Clean up old pending pings (older than 5 minutes)
        s.pending_pings.retain(|_, p| {
            now.signed_duration_since(p.submitted_at).num_seconds() < 300
        });

        drop(s);
        for ev in transitions {
            let _ = events.send(ev);
        }
    }
}
