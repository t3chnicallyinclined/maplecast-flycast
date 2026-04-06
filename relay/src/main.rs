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
}

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "maplecast_relay=info".parse().unwrap()),
        )
        .init();

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
    info!("Max clients: {}", args.max_clients);

    let state = fanout::RelayState::new(args.max_clients);
    let ws_addr: SocketAddr = args.ws_listen.parse().expect("invalid ws_listen address");
    let http_addr: SocketAddr = args.http_listen.parse().expect("invalid http_listen address");

    // HTTP endpoint always runs — serves /metrics and /health unconditionally,
    // /turn-cred only if TURN_SECRET is set.
    let secret = args.turn_secret.clone();
    let host = args.turn_host.clone();
    let state_http = state.clone();
    info!(
        "HTTP endpoint: {} (/metrics /health{})",
        http_addr,
        if secret.is_some() { " /turn-cred" } else { "" }
    );
    let http_task = Some(tokio::spawn(async move {
        if let Err(e) = turn::http_listener(http_addr, secret, host, state_http).await {
            error!("HTTP listener exited: {:?}", e);
        }
    }));

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

    if let Some(h) = http_task {
        h.abort();
    }
}
