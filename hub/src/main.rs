// ============================================================================
// MAPLECAST HUB — Distributed Node Registry + Matchmaker
//
// The hub is the COLD PATH — discovery and matchmaking only.
// It is NEVER in the gameplay hot path. Once a match starts,
// browsers talk directly to the assigned node.
//
// Endpoints:
//   POST   /hub/api/nodes/register        — node registers itself
//   POST   /hub/api/nodes/:id/heartbeat   — node heartbeat + metrics
//   DELETE /hub/api/nodes/:id             — node deregisters
//   GET    /hub/api/nodes                 — list active nodes (public)
//   GET    /hub/api/nodes/nearby          — geographic pre-filter
//   POST   /hub/api/matchmake            — player submits ping results
//   POST   /hub/api/matchmake/select     — collector picks optimal node
//   GET    /hub/api/dashboard/stats       — aggregate stats
//   GET    /hub/api/dashboard/nodes       — full node list for map
//
// OVERKILL IS NECESSARY.
// ============================================================================

mod api;
mod geo;
mod matchmaker;
mod types;

use axum::{Router, routing::{get, post, delete}};
use clap::Parser;
use std::net::SocketAddr;
use tower_http::cors::CorsLayer;
use tracing::info;

use types::Operator;

#[derive(Parser, Debug)]
#[command(name = "maplecast-hub", about = "Distributed node registry + matchmaker — OVERKILL IS NECESSARY")]
struct Args {
    /// HTTP listen address
    #[arg(long, default_value = "127.0.0.1:7220")]
    listen: String,

    /// Bootstrap operator name (creates a default operator for development)
    #[arg(long, env = "MAPLECAST_HUB_BOOTSTRAP_OPERATOR")]
    bootstrap_operator: Option<String>,

    /// Bootstrap operator token (plain-text, used with bootstrap_operator)
    #[arg(long, env = "MAPLECAST_HUB_BOOTSTRAP_TOKEN")]
    bootstrap_token: Option<String>,
}

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "maplecast_hub=info".parse().unwrap()),
        )
        .init();

    let args = Args::parse();

    info!("╔══════════════════════════════════════════════╗");
    info!("║   MAPLECAST HUB — DISTRIBUTED NODE REGISTRY   ║");
    info!("║   OVERKILL IS NECESSARY                        ║");
    info!("╚══════════════════════════════════════════════╝");

    let store = types::new_store();

    // Bootstrap a default operator for development/testing
    if let (Some(name), Some(token)) = (&args.bootstrap_operator, &args.bootstrap_token) {
        let mut s = store.write().await;
        s.operators.insert(
            name.clone(),
            Operator {
                name: name.clone(),
                token_hash: token.clone(), // plain-text for bootstrap; production uses argon2
                approved: true,
                max_nodes: 100,
                created_at: chrono::Utc::now(),
            },
        );
        info!("Bootstrap operator created: {}", name);
    }

    // Spawn stale node sweeper
    let sweeper_store = store.clone();
    tokio::spawn(async move {
        api::stale_sweeper(sweeper_store).await;
    });

    // Build router
    let app = Router::new()
        // Node management
        .route("/hub/api/nodes/register", post(api::register_node))
        .route("/hub/api/nodes/{id}/heartbeat", post(api::heartbeat))
        .route("/hub/api/nodes/{id}", delete(api::deregister_node))
        // Public node listing
        .route("/hub/api/nodes", get(api::list_nodes))
        .route("/hub/api/nodes/nearby", get(api::nearby_nodes))
        // Matchmaking
        .route("/hub/api/matchmake", post(api::matchmake))
        .route("/hub/api/matchmake/select", post(api::matchmake_select))
        // Dashboard
        .route("/hub/api/dashboard/stats", get(api::dashboard_stats))
        .route("/hub/api/dashboard/nodes", get(api::dashboard_nodes))
        // CORS — the dashboard and browser clients live on different origins
        .layer(CorsLayer::permissive())
        .with_state(store);

    let addr: SocketAddr = args.listen.parse().expect("invalid listen address");
    info!("Hub listening on {}", addr);

    let listener = tokio::net::TcpListener::bind(addr).await.expect("bind failed");
    axum::serve(
        listener,
        app.into_make_service_with_connect_info::<SocketAddr>(),
    )
    .await
    .expect("server crashed");
}
