use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::sync::Arc;
use tokio::sync::RwLock;

// ============================================================================
// Node — a registered game server in the network
// ============================================================================

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NodePorts {
    pub relay_ws: u16,
    pub input_udp: u16,
    pub control_ws: u16,
    pub http: u16,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NodeGeo {
    pub lat: f64,
    pub lng: f64,
    pub city: String,
    pub country: String,
    pub region: String,
    pub isp: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NodeCapacity {
    pub max_matches: u32,
    pub max_spectators: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NodeMetrics {
    pub upstream_connected: bool,
    pub frames_received: u64,
    pub avg_frame_interval_us: u64,
    pub max_frame_interval_us: u64,
    pub clients: u64,
    pub bytes_broadcast: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NodeStats {
    pub total_matches: u64,
    pub total_frames: u64,
    pub uptime_s: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Node {
    pub node_id: String,
    pub operator_name: String,
    pub name: String,
    pub region: String,
    pub public_host: String,
    pub ports: NodePorts,
    pub tls: bool,
    pub capacity: NodeCapacity,
    pub rom_hash: String,
    pub version: String,
    pub status: String, // ready | in_match | draining | offline | stale
    pub geo: Option<NodeGeo>,
    pub metrics: Option<NodeMetrics>,
    pub stats: NodeStats,
    pub last_heartbeat: DateTime<Utc>,
    pub registered_at: DateTime<Utc>,
    pub stale_count: u32,
    /// Operator-supplied public URLs (e.g. behind nginx TLS termination).
    /// If set, used as-is; otherwise we construct ws://{host}:{port}/ws.
    pub public_relay_url: Option<String>,
    pub public_control_url: Option<String>,
    pub public_audio_url: Option<String>,
}

impl Node {
    /// Build the full WSS/WS URLs for browser connection
    pub fn relay_url(&self) -> String {
        if let Some(u) = &self.public_relay_url { return u.clone(); }
        let proto = if self.tls { "wss" } else { "ws" };
        format!("{}://{}:{}/ws", proto, self.public_host, self.ports.relay_ws)
    }

    pub fn control_url(&self) -> String {
        if let Some(u) = &self.public_control_url { return u.clone(); }
        let proto = if self.tls { "wss" } else { "ws" };
        format!("{}://{}:{}/play", proto, self.public_host, self.ports.control_ws)
    }

    pub fn audio_url(&self) -> String {
        if let Some(u) = &self.public_audio_url { return u.clone(); }
        let proto = if self.tls { "wss" } else { "ws" };
        // Audio goes through relay port (nginx proxies /audio on the node)
        format!("{}://{}:{}/audio", proto, self.public_host, self.ports.relay_ws)
    }
}

// ============================================================================
// Operator — approved entity that can register nodes
// ============================================================================

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Operator {
    pub name: String,
    pub token_hash: String,
    pub approved: bool,
    pub max_nodes: u32,
    pub created_at: DateTime<Utc>,
}

// ============================================================================
// API request/response types
// ============================================================================

#[derive(Debug, Deserialize)]
pub struct RegisterRequest {
    pub node_id: String,
    pub operator_token: String,
    pub name: String,
    pub region: String,
    pub public_host: Option<String>,
    pub ports: NodePorts,
    pub tls: bool,
    pub capacity: NodeCapacity,
    pub rom_hash: String,
    pub version: String,
    #[serde(default)]
    pub public_relay_url: Option<String>,
    #[serde(default)]
    pub public_control_url: Option<String>,
    #[serde(default)]
    pub public_audio_url: Option<String>,
}

#[derive(Debug, Deserialize)]
pub struct HeartbeatRequest {
    pub status: String,
    pub match_active: bool,
    pub spectator_count: u64,
    pub metrics: NodeMetrics,
    pub stats: NodeStats,
}

#[derive(Debug, Clone, Deserialize)]
pub struct PingResult {
    pub node_id: String,
    pub avg_rtt_ms: f64,
    pub p95_rtt_ms: f64,
}

#[derive(Debug, Deserialize)]
pub struct MatchmakeRequest {
    pub player_id: String,
    pub session_id: String,
    pub ping_results: Vec<PingResult>,
}

#[derive(Debug, Serialize)]
pub struct MatchmakeResponse {
    pub node_id: Option<String>,
    pub node_urls: Option<NodeUrls>,
    pub status: String, // "assigned" | "pending" (waiting for opponent pings)
}

#[derive(Debug, Clone, Serialize)]
pub struct NodeUrls {
    pub relay_url: String,
    pub control_url: String,
    pub audio_url: String,
    pub input_udp_host: String,
    pub input_udp_port: u16,
}

#[derive(Debug, Serialize)]
pub struct NodePublic {
    pub node_id: String,
    pub name: String,
    pub operator: String,
    pub region: String,
    pub public_host: String,
    pub ports: NodePorts,
    pub tls: bool,
    pub status: String,
    pub geo: Option<NodeGeo>,
    pub spectators: u64,
    pub stats: NodeStats,
    pub uptime_s: u64,
    pub relay_url: String,
}

#[derive(Debug, Serialize)]
pub struct DashboardStats {
    pub total_nodes_online: u64,
    pub total_matches_active: u64,
    pub total_spectators: u64,
    pub total_matches_played: u64,
}

#[derive(Debug, Serialize)]
pub struct NearbyQuery {
    pub lat: f64,
    pub lng: f64,
    pub limit: usize,
}

// ============================================================================
// In-memory store — fast, lockless reads for the hot dashboard path
// ============================================================================

#[derive(Debug, Clone, Default)]
pub struct PendingPings {
    pub player_id: String,
    pub session_id: String,
    pub pings: Vec<PingResult>,
    pub submitted_at: DateTime<Utc>,
}

#[derive(Debug, Default)]
pub struct HubStore {
    pub nodes: HashMap<String, Node>,
    pub operators: HashMap<String, Operator>,
    // Pending matchmake ping reports keyed by player_id
    pub pending_pings: HashMap<String, PendingPings>,
}

pub type SharedStore = Arc<RwLock<HubStore>>;

pub fn new_store() -> SharedStore {
    Arc::new(RwLock::new(HubStore::default()))
}
