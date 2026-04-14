use axum::{
    Json,
    extract::{Path, Query, State, ConnectInfo},
    http::StatusCode,
    response::IntoResponse,
};
use chrono::Utc;
use serde::Deserialize;
use std::net::SocketAddr;
use tracing::{info, warn};

use crate::geo;
use crate::matchmaker;
use crate::types::*;

// ============================================================================
// Auth helper — validate operator token (plain-text comparison for now,
// upgrade to argon2 hash check when operators table is in SurrealDB)
// ============================================================================

fn validate_operator(store: &HubStore, token: &str) -> Option<String> {
    store
        .operators
        .iter()
        .find(|(_, op)| op.approved && op.token_hash == token)
        .map(|(_, op)| op.name.clone())
}

// ============================================================================
// POST /hub/api/nodes/register — register a new game server node
// ============================================================================

pub async fn register_node(
    State(store): State<SharedStore>,
    ConnectInfo(addr): ConnectInfo<SocketAddr>,
    Json(req): Json<RegisterRequest>,
) -> impl IntoResponse {
    let s = store.write().await;

    // Validate operator token
    let operator_name = match validate_operator(&s, &req.operator_token) {
        Some(name) => name,
        None => {
            warn!("Registration rejected: invalid operator token from {}", addr);
            return (
                StatusCode::FORBIDDEN,
                Json(serde_json::json!({"ok": false, "error": "invalid operator token"})),
            );
        }
    };

    // Check if operator has hit their node limit
    let operator_node_count = s
        .nodes
        .values()
        .filter(|n| n.operator_name == operator_name && n.status != "offline")
        .count() as u32;
    let max_nodes = s
        .operators
        .get(&operator_name)
        .map(|o| o.max_nodes)
        .unwrap_or(5);
    if operator_node_count >= max_nodes {
        return (
            StatusCode::BAD_REQUEST,
            Json(serde_json::json!({"ok": false, "error": "node limit reached"})),
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

    let now = Utc::now();
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
    };

    info!(
        "Node registered: {} ({}) by {} at {}",
        node.name, req.node_id, operator_name,
        geo.as_ref().map(|g| format!("{}, {}", g.city, g.country)).unwrap_or_else(|| "unknown".into())
    );

    s.nodes.insert(req.node_id.clone(), node);

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
    Path(node_id): Path<String>,
    Json(req): Json<HeartbeatPayload>,
) -> impl IntoResponse {
    let mut s = store.write().await;

    // Validate operator token
    if validate_operator(&s, &req.operator_token).is_none() {
        return (
            StatusCode::FORBIDDEN,
            Json(serde_json::json!({"ok": false, "error": "invalid token"})),
        );
    }

    let node: &mut Node = match s.nodes.get_mut(&node_id) {
        Some(n) => n,
        None => {
            return (
                StatusCode::NOT_FOUND,
                Json(serde_json::json!({"ok": false, "error": "node not found"})),
            );
        }
    };

    node.status = req.status;
    node.metrics = Some(req.metrics);
    node.stats = req.stats;
    node.last_heartbeat = Utc::now();
    // Reset stale count on successful heartbeat
    if node.status != "offline" {
        node.stale_count = 0;
    }

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
    Path(node_id): Path<String>,
    Query(q): Query<DeleteQuery>,
) -> impl IntoResponse {
    let mut s = store.write().await;

    if validate_operator(&s, &q.operator_token).is_none() {
        return (
            StatusCode::FORBIDDEN,
            Json(serde_json::json!({"ok": false, "error": "invalid token"})),
        );
    }

    if let Some(node) = s.nodes.get_mut(&node_id) {
        info!("Node deregistered: {} ({})", node.name, node_id);
        node.status = "offline".to_string();
        (StatusCode::OK, Json(serde_json::json!({"ok": true})))
    } else {
        (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({"ok": false, "error": "node not found"})),
        )
    }
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
        relay_url: n.relay_url(),
    }
}

// ============================================================================
// Stale node sweeper — runs as a background tokio task
// ============================================================================

pub async fn stale_sweeper(store: SharedStore) {
    let mut interval = tokio::time::interval(std::time::Duration::from_secs(10));

    loop {
        interval.tick().await;
        let now = Utc::now();
        let mut s = store.write().await;

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
                }
            } else if age > 30 {
                if node.status != "stale" {
                    warn!(
                        "Node {} ({}) → stale (no heartbeat for {}s)",
                        node.name, node.node_id, age
                    );
                    node.status = "stale".to_string();
                }
            }
        }

        // Clean up old pending pings (older than 5 minutes)
        s.pending_pings.retain(|_, p| {
            now.signed_duration_since(p.submitted_at).num_seconds() < 300
        });
    }
}
