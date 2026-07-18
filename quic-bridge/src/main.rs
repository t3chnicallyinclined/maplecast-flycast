//! MapleCast TDW → QUIC bridge (roadmap D1, Phase 1).
//!
//! WS-subscribes flycast's TDW players wire on :7200 and re-serves it over QUIC:
//!   - TDW1 frames that fit a datagram  -> UNRELIABLE datagram (no head-of-line
//!     blocking — a lost frame is skipped, the next full snapshot arrives on
//!     time; the whole reason we're leaving TCP).
//!   - SYNC / TDWS / any oversized frame -> RELIABLE per-message uni-stream
//!     (must arrive complete; each on its own stream so one stalled frame never
//!     blocks the others — the opposite of TCP's single ordered byte-stream).
//!
//! One QUIC client == one upstream WS subscription (its own view of the shared
//! game). Input stays on the client's existing raw-UDP :7100 path — untouched.
//!
//! Env: MC_BRIDGE_LISTEN (default 0.0.0.0:7300), MC_FLYCAST_WS
//! (default ws://127.0.0.1:7200).

use anyhow::{Context, Result};
use bytes::Bytes;
use futures_util::{SinkExt, StreamExt};
use std::sync::Arc;
use tokio_tungstenite::tungstenite::Message;

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let rt = tokio::runtime::Runtime::new()?;
    rt.block_on(run())
}

async fn run() -> Result<()> {
    rustls::crypto::ring::default_provider()
        .install_default()
        .ok();

    let listen: std::net::SocketAddr = std::env::var("MC_BRIDGE_LISTEN")
        .unwrap_or_else(|_| "0.0.0.0:7300".into())
        .parse()
        .context("MC_BRIDGE_LISTEN")?;
    let flycast_ws =
        std::env::var("MC_FLYCAST_WS").unwrap_or_else(|_| "ws://127.0.0.1:7200".into());

    // Self-signed cert. Phase-1 measurement rig: the client skips verification.
    // Productionization swaps in the real play.nobd.net cert (same as the relay).
    let cert = rcgen::generate_simple_self_signed(vec!["maplecast-bridge".into()])?;
    let cert_der = rustls::pki_types::CertificateDer::from(cert.cert);
    let key_der =
        rustls::pki_types::PrivatePkcs8KeyDer::from(cert.key_pair.serialize_der());

    let mut server_crypto = rustls::ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(vec![cert_der], key_der.into())?;
    server_crypto.alpn_protocols = vec![b"mc-tdw".to_vec()];
    let mut server_config = quinn::ServerConfig::with_crypto(Arc::new(
        quinn::crypto::rustls::QuicServerConfig::try_from(server_crypto)?,
    ));
    // Keep the connection warm; a game stream is continuous.
    let transport = Arc::get_mut(&mut server_config.transport).unwrap();
    transport.max_idle_timeout(Some(std::time::Duration::from_secs(30).try_into()?));
    transport.keep_alive_interval(Some(std::time::Duration::from_secs(5)));

    let endpoint = quinn::Endpoint::server(server_config, listen)?;
    log::info!("[bridge] QUIC listening on {listen}  (upstream {flycast_ws})");

    while let Some(incoming) = endpoint.accept().await {
        let ws = flycast_ws.clone();
        tokio::spawn(async move {
            match incoming.await {
                Ok(conn) => {
                    let peer = conn.remote_address();
                    log::info!("[bridge] QUIC client {peer} connected");
                    if let Err(e) = serve_client(conn, &ws).await {
                        log::warn!("[bridge] client {peer} ended: {e}");
                    }
                }
                Err(e) => log::warn!("[bridge] handshake failed: {e}"),
            }
        });
    }
    Ok(())
}

/// One QUIC client: open a fresh WS to flycast, subscribe TDW, and pump every
/// wire frame out over QUIC with the datagram/stream split.
async fn serve_client(conn: quinn::Connection, flycast_ws: &str) -> Result<()> {
    let (ws_stream, _) = tokio_tungstenite::connect_async(flycast_ws)
        .await
        .context("connect flycast WS")?;
    let (mut ws_tx, mut ws_rx) = ws_stream.split();
    // Seed (onOpen SYNC is pushed automatically) + go TDW-only.
    ws_tx.send(Message::Text("{\"type\":\"request_sync\"}".into())).await?;
    ws_tx
        .send(Message::Text("{\"type\":\"subscribe\",\"mode\":\"tdw\"}".into()))
        .await?;

    let mut dgrams: u64 = 0;
    let mut streams: u64 = 0;
    let mut log_t = std::time::Instant::now();

    while let Some(msg) = ws_rx.next().await {
        let data = match msg {
            Ok(Message::Binary(b)) => b,
            Ok(Message::Close(_)) => break,
            Ok(_) => continue, // text lobby JSON — not needed on the QUIC leg
            Err(e) => {
                log::warn!("[bridge] upstream WS error: {e}");
                break;
            }
        };
        if conn.close_reason().is_some() {
            break;
        }

        // A frame that fits a datagram AND isn't a big keyframe -> datagram.
        // TDW1 warm frames (~700B) ride here. Everything oversized (SYNC/TDWS/
        // page-heavy super frames) -> its own reliable uni-stream.
        let max_dg = conn.max_datagram_size().unwrap_or(0);
        let is_tdw1 = data.len() >= 4 && &data[0..4] == b"TDW1";
        if is_tdw1 && data.len() <= max_dg {
            match conn.send_datagram(Bytes::copy_from_slice(&data)) {
                Ok(()) => dgrams += 1,
                Err(_) => {
                    send_stream(&conn, &data).await?;
                    streams += 1;
                }
            }
        } else {
            send_stream(&conn, &data).await?;
            streams += 1;
        }

        if log_t.elapsed().as_secs() >= 10 {
            log::info!(
                "[bridge] out: {dgrams} datagrams, {streams} streams (max_dg={max_dg}B)"
            );
            log_t = std::time::Instant::now();
        }
    }
    Ok(())
}

/// Reliable per-message uni-stream: open, write the whole message, finish.
/// The stream boundary IS the message boundary (client reads to end).
async fn send_stream(conn: &quinn::Connection, data: &[u8]) -> Result<()> {
    let mut s = conn.open_uni().await?;
    s.write_all(data).await?;
    s.finish()?;
    Ok(())
}
