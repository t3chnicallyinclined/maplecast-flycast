// ============================================================================
// WEBTRANSPORT — QUIC/HTTP3 client listener
//
// Runs alongside the WebSocket listener. Browsers that support WebTransport
// (Chrome 97+, Edge) connect here for zero-HOL-blocking frame delivery.
//
// Frame delivery:
//   Delta frames → unreliable datagrams (no retransmit = lowest latency)
//   SYNC packets → reliable unidirectional stream (must arrive complete)
//
// The browser's transport.mjs tries WebTransport first, falls back to WS.
// OVERKILL IS NECESSARY.
// ============================================================================

use crate::fanout::RelayState;
use crate::protocol;
use bytes::Bytes;
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;
use tokio::sync::broadcast;
use tracing::{info, warn, error, debug};
use wtransport::tls;

/// Start the WebTransport/QUIC listener.
/// Binds UDP on `addr` (typically 0.0.0.0:443).
pub async fn wt_client_listener(
    addr: SocketAddr,
    cert_path: String,
    key_path: String,
    state: RelayState,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let identity = load_identity(&cert_path, &key_path)?;

    let config = wtransport::ServerConfig::builder()
        .with_bind_address(addr)
        .with_identity(identity)
        .keep_alive_interval(Some(Duration::from_secs(3)))
        .max_idle_timeout(Some(Duration::from_secs(30)))?
        .build();

    let endpoint = wtransport::Endpoint::server(config)?;
    info!("WebTransport (QUIC/HTTP3) listener ready on UDP {}", addr);

    loop {
        let incoming = endpoint.accept().await;
        let state = state.clone();

        tokio::spawn(async move {
            match handle_incoming(incoming, state).await {
                Ok(_) => debug!("WT session ended cleanly"),
                Err(e) => debug!("WT session error: {}", e),
            }
        });
    }
}

/// Load TLS certificate + private key from Let's Encrypt PEM files.
fn load_identity(
    cert_path: &str,
    key_path: &str,
) -> Result<tls::Identity, Box<dyn std::error::Error + Send + Sync>> {
    use std::io::BufReader;

    let cert_file = std::fs::File::open(cert_path)
        .map_err(|e| format!("Failed to open cert {}: {}", cert_path, e))?;
    let key_file = std::fs::File::open(key_path)
        .map_err(|e| format!("Failed to open key {}: {}", key_path, e))?;

    // Parse PEM certificates
    let certs: Vec<tls::Certificate> = rustls_pemfile::certs(&mut BufReader::new(cert_file))
        .filter_map(|r| r.ok())
        .map(|der| tls::Certificate::from_der(der.to_vec()))
        .collect::<Result<Vec<_>, _>>()
        .map_err(|e| format!("Invalid certificate: {:?}", e))?;

    if certs.is_empty() {
        return Err("No certificates found in PEM file".into());
    }

    // Parse PEM private key
    let key_der = rustls_pemfile::private_key(&mut BufReader::new(key_file))
        .map_err(|e| format!("Failed to parse key: {}", e))?
        .ok_or("No private key found in PEM file")?;

    let private_key = tls::PrivateKey::from_der_pkcs8(key_der.secret_der().to_vec());

    let chain = tls::CertificateChain::new(certs);
    let identity = tls::Identity::new(chain, private_key);

    info!("Loaded TLS identity from {} + {}", cert_path, key_path);
    Ok(identity)
}

async fn handle_incoming(
    incoming: wtransport::endpoint::IncomingSession,
    state: RelayState,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    // IncomingSession → SessionRequest
    let session_request = incoming.await?;

    let path = session_request.path().to_string();
    let authority = session_request.authority().to_string();
    info!("WT session request: authority={} path={}", authority, path);

    // Accept the session → Connection
    let conn = session_request.accept().await?;
    let conn = Arc::new(conn);
    info!("WT session established");

    if !state.add_client().await {
        warn!("WT client rejected — max clients reached");
        return Ok(());
    }

    let count = state.client_count().await;
    info!("WT client connected (total: {})", count);

    let result = run_session(conn.clone(), state.clone()).await;

    state.remove_client().await;
    let count = state.client_count().await;
    info!("WT client disconnected (total: {})", count);

    result
}

async fn run_session(
    conn: Arc<wtransport::Connection>,
    state: RelayState,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    // Step 1: Send cached SYNC via reliable unidirectional stream
    if let Some(sync_data) = state.get_sync().await {
        info!("Sending cached SYNC via WT uni-stream ({:.1}MB)", sync_data.len() as f64 / 1024.0 / 1024.0);
        let mut uni = conn.open_uni().await?.await?;
        use tokio::io::AsyncWriteExt;
        uni.write_all(&sync_data).await?;
        uni.shutdown().await?;
    }

    // Step 2: Subscribe to frame broadcast + forward
    let mut frame_rx = state.subscribe_frames();
    let mut frames_sent: u64 = 0;
    let mut dgram_sent: u64 = 0;
    let mut stream_sent: u64 = 0;
    let mut frames_dropped: u64 = 0;

    loop {
        tokio::select! {
            frame = frame_rx.recv() => {
                match frame {
                    Ok(data) => {
                        if protocol::is_sync_or_compressed_sync(&data) {
                            // SYNC — reliable uni-stream
                            match send_reliable(&conn, &data).await {
                                Ok(_) => stream_sent += 1,
                                Err(_) => break,
                            }
                        } else {
                            // Delta/audio — unreliable datagram (fast path)
                            match conn.send_datagram(Bytes::copy_from_slice(&data)) {
                                Ok(_) => dgram_sent += 1,
                                Err(wtransport::error::SendDatagramError::TooLarge) => {
                                    // Exceeds max datagram — fall back to uni-stream
                                    debug!("WT datagram too large ({}B), using uni-stream", data.len());
                                    match send_reliable(&conn, &data).await {
                                        Ok(_) => stream_sent += 1,
                                        Err(_) => break,
                                    }
                                }
                                Err(_) => break,
                            }
                        }
                        frames_sent += 1;
                    }
                    Err(broadcast::error::RecvError::Lagged(n)) => {
                        frames_dropped += n;
                        debug!("WT client lagged {} frames (dropped: {})", n, frames_dropped);
                    }
                    Err(broadcast::error::RecvError::Closed) => break,
                }
            }

            _ = conn.closed() => {
                debug!("WT session closed by peer");
                break;
            }
        }
    }

    if frames_sent > 0 {
        info!(
            "WT client final: sent={} (dgram={} stream={}) dropped={}",
            frames_sent, dgram_sent, stream_sent, frames_dropped
        );
    }

    Ok(())
}

/// Send data via a reliable unidirectional stream (for SYNC and oversized frames).
async fn send_reliable(
    conn: &wtransport::Connection,
    data: &[u8],
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let mut uni = conn.open_uni().await?.await?;
    use tokio::io::AsyncWriteExt;
    uni.write_all(data).await?;
    uni.shutdown().await?;
    Ok(())
}
