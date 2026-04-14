// ============================================================================
// MAPLECAST RELAY — Zero-Copy TA Stream Relay
//
// Two upstream modes:
//   --ws-upstream ws://your-box:7200  (DEFAULT — connects to flycast WS)
//   --tcp-listen 0.0.0.0:7300        (FUTURE — flycast pushes raw TCP)
//
// Downstream:
//   WebSocket on --ws-listen (browsers/players connect here)
//
// Hot path (TCP mode): splice() + tee() — frame data never touches userspace.
// WS mode: still fast — tokio async, broadcast channel, zero-copy Bytes.
//
// OVERKILL IS NECESSARY.
// ============================================================================

mod splice;
mod protocol;
mod fanout;
mod signaling;
mod turn;
mod auth_api;
mod admin_api;
mod client_telemetry;
mod webtransport;
mod hub_client;

use clap::Parser;
use std::net::SocketAddr;
use tracing::{info, error};

#[derive(Parser, Debug)]
#[command(name = "maplecast-relay", about = "Zero-copy TA stream relay — OVERKILL IS NECESSARY")]
struct Args {
    /// Flycast WebSocket URL to connect to as upstream (ws://host:port)
    #[arg(long)]
    ws_upstream: Option<String>,

    /// Raw TCP listen address for flycast to push frames to us (future)
    #[arg(long, default_value = "0.0.0.0:7300")]
    tcp_listen: String,

    /// WebSocket listen address (browsers/players connect here)
    #[arg(long, default_value = "0.0.0.0:7201")]
    ws_listen: String,

    /// Max spectator clients
    #[arg(long, default_value_t = 500)]
    max_clients: usize,

    /// HTTP listen address for /turn-cred endpoint
    #[arg(long, default_value = "127.0.0.1:7202")]
    http_listen: String,

    /// TURN shared secret (env: TURN_SECRET). Required for /turn-cred.
    #[arg(long, env = "TURN_SECRET")]
    turn_secret: Option<String>,

    /// TURN server hostname (used in ICE config response)
    #[arg(long, default_value = "nobd.net")]
    turn_host: String,

    /// WebTransport (QUIC/HTTP3) listen address (UDP)
    #[arg(long, default_value = "0.0.0.0:443")]
    wt_listen: String,

    /// TLS certificate path (fullchain.pem) for QUIC
    #[arg(long, default_value = "/etc/letsencrypt/live/nobd.net/fullchain.pem")]
    tls_cert: String,

    /// TLS private key path (privkey.pem) for QUIC
    #[arg(long, default_value = "/etc/letsencrypt/live/nobd.net/privkey.pem")]
    tls_key: String,

    /// Disable WebTransport listener (run WS-only mode)
    #[arg(long)]
    no_webtransport: bool,

    // ── Hub registration (distributed node network) ──────────────

    /// Register this node with the MapleCast hub for distributed matchmaking
    #[arg(long)]
    hub_register: bool,

    /// Hub API URL (e.g. https://nobd.net/hub/api). Setting this env var
    /// auto-enables registration even without --hub-register.
    #[arg(long, env = "MAPLECAST_HUB_URL")]
    hub_url: Option<String>,

    /// Operator token for hub authentication
    #[arg(long, env = "MAPLECAST_HUB_TOKEN")]
    hub_token: Option<String>,

    /// Human-readable node name shown on the dashboard
    #[arg(long, env = "MAPLECAST_NODE_NAME", default_value = "unnamed")]
    node_name: String,

    /// Region identifier (us-east, eu-west, ap-northeast, etc.)
    #[arg(long, env = "MAPLECAST_NODE_REGION", default_value = "auto")]
    node_region: String,

    /// Public hostname or IP (auto-detected via ifconfig.me if omitted)
    #[arg(long, env = "MAPLECAST_PUBLIC_HOST")]
    public_host: Option<String>,

    /// Override the public relay WS URL (e.g. wss://nobd.net/ws when nginx
    /// terminates TLS). Defaults to ws://{public_host}:{ws_port}/ws.
    #[arg(long, env = "MAPLECAST_PUBLIC_RELAY_URL")]
    public_relay_url: Option<String>,

    /// Override the public control WS URL (e.g. wss://nobd.net/play)
    #[arg(long, env = "MAPLECAST_PUBLIC_CONTROL_URL")]
    public_control_url: Option<String>,

    /// Override the public audio WS URL (e.g. wss://nobd.net/audio)
    #[arg(long, env = "MAPLECAST_PUBLIC_AUDIO_URL")]
    public_audio_url: Option<String>,
}

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "maplecast_relay=info".parse().unwrap()),
        )
        .init();

    // Environment variable aliases: "input server" is the user-facing canonical
    // name, but historically clap reads MAPLECAST_NODE_*. Promote the alias
    // values to the canonical names BEFORE clap parses. Canonical wins if both
    // are set.
    for (alias, canonical) in [
        ("MAPLECAST_INPUT_SERVER_NAME", "MAPLECAST_NODE_NAME"),
        ("MAPLECAST_INPUT_SERVER_REGION", "MAPLECAST_NODE_REGION"),
    ] {
        if std::env::var_os(canonical).is_none() {
            if let Ok(v) = std::env::var(alias) {
                // SAFETY: single-threaded main, before any tasks spawn
                unsafe { std::env::set_var(canonical, v); }
            }
        }
    }

    let args = Args::parse();

    info!("╔══════════════════════════════════════════════╗");
    info!("║   MAPLECAST RELAY — ZERO COPY FANOUT          ║");
    info!("║   OVERKILL IS NECESSARY                        ║");
    info!("╚══════════════════════════════════════════════╝");

    let upstream_mode = if let Some(ref url) = args.ws_upstream {
        info!("Upstream: WebSocket → {}", url);
        "ws"
    } else {
        info!("Upstream: TCP listen on {}", args.tcp_listen);
        "tcp"
    };
    info!("Downstream: WebSocket on {}", args.ws_listen);
    if !args.no_webtransport {
        info!("Downstream: WebTransport (QUIC) on UDP {}", args.wt_listen);
    }
    info!("Max clients: {}", args.max_clients);

    let state = fanout::RelayState::new(args.max_clients);
    let telemetry = client_telemetry::ClientTelemetry::new();
    let ws_addr: SocketAddr = args.ws_listen.parse().expect("invalid ws_listen address");
    let http_addr: SocketAddr = args.http_listen.parse().expect("invalid http_listen address");

    // HTTP endpoint always runs — serves /metrics and /health unconditionally,
    // /turn-cred only if TURN_SECRET is set.
    let secret = args.turn_secret.clone();
    let host = args.turn_host.clone();
    let state_http = state.clone();
    let telemetry_http = telemetry.clone();
    info!(
        "HTTP endpoint: {} (/metrics /health /api{})",
        http_addr,
        if secret.is_some() { " /turn-cred" } else { "" }
    );
    let http_task = Some(tokio::spawn(async move {
        if let Err(e) = turn::http_listener(http_addr, secret, host, state_http, telemetry_http).await {
            error!("HTTP listener exited: {:?}", e);
        }
    }));

    // Spawn WebTransport listener if enabled
    let wt_task = if !args.no_webtransport {
        let wt_addr: SocketAddr = args.wt_listen.parse().expect("invalid wt_listen address");
        let state_wt = state.clone();
        let cert = args.tls_cert.clone();
        let key = args.tls_key.clone();
        Some(tokio::spawn(async move {
            if let Err(e) = webtransport::wt_client_listener(wt_addr, cert, key, state_wt).await {
                error!("WebTransport listener exited: {:?}", e);
            }
        }))
    } else {
        info!("WebTransport: disabled (--no-webtransport)");
        None
    };

    // Spawn the queue sweeper — drops stale 'waiting' / 'promoted' rows
    // whose last_seen_at heartbeat hasn't fired in 30s. The browser
    // heartbeats every 10s while in queue (queue.mjs startQueueHeartbeat),
    // so 30s grace = 3 missed beats. Closing the tab → row sweeps inside
    // the grace window. Refreshing within 30s → reclaim path picks it back
    // up. Also reaps legacy 'expired' / 'left' rows that have no other
    // owner. Sweep interval is 15s — strictly less than grace so a row that
    // just crossed the line gets cleaned within one cycle.
    let cfg_sweep = auth_api::DbConfig::from_env();
    let _queue_sweeper = tokio::spawn(async move {
        let interval = std::time::Duration::from_secs(15);
        let sql = "DELETE queue WHERE \
            (status IN ['waiting', 'promoted'] AND last_seen_at < time::now() - 30s) \
            OR status IN ['expired', 'left'];";
        loop {
            tokio::time::sleep(interval).await;
            if let Err(e) = auth_api::sql_query_as_admin(&cfg_sweep, sql).await {
                tracing::warn!("[queue-sweeper] sweep failed: {}", e);
            }
        }
    });

    // Spawn hub client if registration is enabled (flag or env var)
    let _hub_task = if args.hub_register || args.hub_url.is_some() {
        match (&args.hub_url, &args.hub_token) {
            (Some(url), Some(token)) => {
                let ws_port: u16 = args.ws_listen.split(':').last()
                    .and_then(|p| p.parse().ok())
                    .unwrap_or(7201);
                let input_port: u16 = std::env::var("MAPLECAST_PORT")
                    .ok()
                    .and_then(|p| p.parse().ok())
                    .unwrap_or(7100);

                let config = hub_client::HubConfig {
                    hub_url: url.clone(),
                    hub_token: token.clone(),
                    node_name: args.node_name.clone(),
                    node_region: args.node_region.clone(),
                    public_host: args.public_host.clone(),
                    ws_listen_port: ws_port,
                    input_port,
                    public_relay_url: args.public_relay_url.clone(),
                    public_control_url: args.public_control_url.clone(),
                    public_audio_url: args.public_audio_url.clone(),
                };
                let hub_state = state.clone();
                info!("Hub registration enabled → {}", url);
                Some(tokio::spawn(async move {
                    hub_client::run(config, hub_state).await;
                }))
            }
            _ => {
                error!("Hub registration requires both --hub-url and --hub-token");
                None
            }
        }
    } else {
        None
    };

    match upstream_mode {
        "ws" => {
            // WebSocket upstream mode — connect to flycast as a WS client
            let url = args.ws_upstream.unwrap();
            let state_up = state.clone();
            let state_ws = state.clone();

            tokio::select! {
                r = fanout::ws_upstream_connector(url, state_up) => {
                    error!("WS upstream connector exited: {:?}", r);
                }
                r = fanout::ws_client_listener(ws_addr, state_ws) => {
                    error!("WS client listener exited: {:?}", r);
                }
            }
        }
        _ => {
            // TCP upstream mode — flycast pushes frames to us
            let tcp_addr: SocketAddr = args.tcp_listen.parse().expect("invalid tcp_listen address");
            let state_tcp = state.clone();
            let state_ws = state.clone();

            tokio::select! {
                r = fanout::tcp_upstream_listener(tcp_addr, state_tcp) => {
                    error!("TCP upstream listener exited: {:?}", r);
                }
                r = fanout::ws_client_listener(ws_addr, state_ws) => {
                    error!("WS client listener exited: {:?}", r);
                }
            }
        }
    }

    if let Some(h) = wt_task {
        h.abort();
    }

    if let Some(h) = http_task {
        h.abort();
    }
}
