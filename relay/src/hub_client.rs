// ============================================================================
// HUB CLIENT — Node registration + heartbeat to the MapleCast Hub
//
// The relay registers itself as a game server node in the distributed
// network. Heartbeats every 10 seconds with metrics pulled from
// RelayState::metrics(). The hub is NEVER in the gameplay hot path.
//
// Activation: --hub-register flag or MAPLECAST_HUB_URL env var.
// ============================================================================

use crate::fanout::RelayState;
use serde::{Deserialize, Serialize};
use std::path::PathBuf;
use std::time::Duration;
use tracing::{info, warn, error};

/// Configuration for hub registration, derived from CLI args
#[derive(Debug, Clone)]
pub struct HubConfig {
    pub hub_url: String,
    pub hub_token: String,
    pub node_name: String,
    pub node_region: String,
    pub public_host: Option<String>,
    pub ws_listen_port: u16,
    pub input_port: u16,
    /// Optional override for the public wss:// URL the browser uses for the
    /// TA stream. Set this when the relay sits behind nginx TLS termination
    /// (e.g. wss://nobd.net/ws). Defaults to ws://{public_host}:{port}/ws.
    pub public_relay_url: Option<String>,
    pub public_control_url: Option<String>,
    pub public_audio_url: Option<String>,
}

// ============================================================================
// Node ID — persisted UUID in ~/.maplecast/node_id
// ============================================================================

fn node_id_path() -> PathBuf {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/opt/maplecast".into());
    PathBuf::from(home).join(".maplecast").join("node_id")
}

fn load_or_create_node_id() -> String {
    let path = node_id_path();
    if let Ok(id) = std::fs::read_to_string(&path) {
        let id = id.trim().to_string();
        if !id.is_empty() {
            info!("Loaded node_id: {}", id);
            return id;
        }
    }

    let id = uuid::Uuid::new_v4().to_string();
    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    if let Err(e) = std::fs::write(&path, &id) {
        warn!("Could not persist node_id to {:?}: {}", path, e);
    }
    info!("Generated new node_id: {}", id);
    id
}

// ============================================================================
// Public IP detection — simple HTTP GET to ifconfig.me
// ============================================================================

async fn detect_public_ip() -> Option<String> {
    match reqwest::get("https://ifconfig.me/ip").await {
        Ok(resp) => match resp.text().await {
            Ok(ip) => {
                let ip = ip.trim().to_string();
                info!("Detected public IP: {}", ip);
                Some(ip)
            }
            Err(e) => {
                warn!("Could not read public IP response: {}", e);
                None
            }
        },
        Err(e) => {
            warn!("Public IP detection failed: {}", e);
            None
        }
    }
}

// ============================================================================
// Registration payload (matches hub API expectations)
// ============================================================================

#[derive(Serialize)]
struct RegisterPayload {
    node_id: String,
    operator_token: String,
    name: String,
    region: String,
    public_host: Option<String>,
    ports: Ports,
    tls: bool,
    capacity: Capacity,
    rom_hash: String,
    version: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    public_relay_url: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    public_control_url: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    public_audio_url: Option<String>,
}

#[derive(Serialize)]
struct Ports {
    relay_ws: u16,
    input_udp: u16,
    control_ws: u16,
    http: u16,
}

#[derive(Serialize)]
struct Capacity {
    max_matches: u32,
    max_spectators: u32,
}

// ============================================================================
// Heartbeat payload
// ============================================================================

#[derive(Serialize)]
struct HeartbeatPayload {
    operator_token: String,
    status: String,
    metrics: MetricsPayload,
    stats: StatsPayload,
}

#[derive(Serialize)]
struct MetricsPayload {
    upstream_connected: bool,
    frames_received: u64,
    avg_frame_interval_us: u64,
    max_frame_interval_us: u64,
    clients: u64,
    bytes_broadcast: u64,
}

#[derive(Serialize)]
struct StatsPayload {
    total_matches: u64,
    total_frames: u64,
    uptime_s: u64,
}

#[derive(Deserialize)]
struct RegisterResponse {
    ok: bool,
    #[serde(default)]
    error: Option<String>,
}

// ============================================================================
// Main hub client task — spawned from main.rs
// ============================================================================

pub async fn run(config: HubConfig, state: RelayState) {
    let node_id = load_or_create_node_id();
    let start_time = std::time::Instant::now();

    // Determine public host
    let public_host = match &config.public_host {
        Some(h) => h.clone(),
        None => match detect_public_ip().await {
            Some(ip) => ip,
            None => {
                error!("Cannot determine public host — set --public-host or MAPLECAST_PUBLIC_HOST");
                return;
            }
        },
    };

    let client = reqwest::Client::new();

    // Register with hub
    let register_url = format!("{}/nodes/register", config.hub_url);
    let payload = RegisterPayload {
        node_id: node_id.clone(),
        operator_token: config.hub_token.clone(),
        name: config.node_name.clone(),
        region: config.node_region.clone(),
        public_host: Some(public_host),
        ports: Ports {
            relay_ws: config.ws_listen_port,
            input_udp: config.input_port,
            control_ws: config.ws_listen_port, // relay handles /play routing
            http: config.ws_listen_port + 1,   // HTTP is typically relay_ws + 1
        },
        tls: false, // operator configures this externally
        capacity: Capacity {
            max_matches: 1,
            max_spectators: 500,
        },
        rom_hash: "unknown".to_string(), // TODO: read from flycast status
        version: env!("CARGO_PKG_VERSION").to_string(),
        public_relay_url: config.public_relay_url.clone(),
        public_control_url: config.public_control_url.clone(),
        public_audio_url: config.public_audio_url.clone(),
    };

    info!("Registering input server with hub at {}", register_url);

    match client.post(&register_url).json(&payload).send().await {
        Ok(resp) => {
            let status = resp.status();
            match resp.json::<RegisterResponse>().await {
                Ok(r) if r.ok => {
                    info!("Hub registration successful — input server {} is live", node_id);
                }
                Ok(r) => {
                    error!(
                        "Hub registration rejected ({}): {}",
                        status,
                        r.error.unwrap_or_else(|| "unknown".into())
                    );
                    return;
                }
                Err(e) => {
                    error!("Hub registration response parse error: {}", e);
                    return;
                }
            }
        }
        Err(e) => {
            error!("Hub registration failed: {} — will retry on heartbeat", e);
            // Don't return — keep trying via heartbeat
        }
    }

    // Heartbeat loop — every 10 seconds
    let mut interval = tokio::time::interval(Duration::from_secs(10));
    let mut consecutive_failures: u32 = 0;

    loop {
        interval.tick().await;

        let metrics = state.metrics().await;
        let uptime = start_time.elapsed().as_secs();

        let heartbeat_url = format!("{}/nodes/{}/heartbeat", config.hub_url, node_id);
        let hb = HeartbeatPayload {
            operator_token: config.hub_token.clone(),
            status: if metrics.upstream_connected {
                "ready".to_string()
            } else {
                "draining".to_string()
            },
            metrics: MetricsPayload {
                upstream_connected: metrics.upstream_connected,
                frames_received: metrics.frames_received,
                avg_frame_interval_us: metrics.avg_frame_interval_us,
                max_frame_interval_us: metrics.max_frame_interval_us,
                clients: metrics.clients,
                bytes_broadcast: metrics.bytes_broadcast,
            },
            stats: StatsPayload {
                total_matches: 0, // TODO: track from flycast status
                total_frames: metrics.frames_received,
                uptime_s: uptime,
            },
        };

        match client.post(&heartbeat_url).json(&hb).send().await {
            Ok(resp) if resp.status().is_success() => {
                if consecutive_failures > 0 {
                    info!("Hub heartbeat recovered after {} failures", consecutive_failures);
                }
                consecutive_failures = 0;
            }
            Ok(resp) if resp.status().as_u16() == 404 => {
                // Hub forgot us (likely restarted with empty in-memory store).
                // Re-register without restarting the relay process.
                warn!("Hub returned 404 — re-registering node {}", node_id);
                let _ = client.post(&register_url).json(&payload).send().await;
            }
            Ok(resp) => {
                consecutive_failures += 1;
                warn!(
                    "Hub heartbeat rejected (HTTP {}), failure #{}",
                    resp.status(),
                    consecutive_failures
                );
            }
            Err(e) => {
                consecutive_failures += 1;
                if consecutive_failures <= 3 {
                    warn!("Hub heartbeat failed: {} (failure #{})", e, consecutive_failures);
                } else if consecutive_failures == 4 {
                    error!(
                        "Hub unreachable after {} attempts — node continues operating independently",
                        consecutive_failures
                    );
                }
                // Keep retrying silently — the node works fine without the hub
            }
        }
    }
}

/// Graceful deregister — called on SIGTERM
pub async fn deregister(hub_url: &str, hub_token: &str) {
    let node_id = load_or_create_node_id();
    let url = format!(
        "{}/nodes/{}?operator_token={}",
        hub_url, node_id, hub_token
    );

    info!("Deregistering node {} from hub", node_id);
    let client = reqwest::Client::new();
    match client.delete(&url).send().await {
        Ok(resp) if resp.status().is_success() => {
            info!("Node deregistered successfully");
        }
        Ok(resp) => {
            warn!("Deregistration got HTTP {}", resp.status());
        }
        Err(e) => {
            warn!("Deregistration failed: {}", e);
        }
    }
}
