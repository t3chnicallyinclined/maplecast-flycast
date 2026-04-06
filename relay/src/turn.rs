// ============================================================================
// TURN — Time-limited credential generator for coturn (REST API spec)
//
// Implements the standard coturn shared-secret REST auth:
//   username = "<unix_expiry>:<userId>"
//   password = base64(HMAC-SHA1(secret, username))
//
// Browsers fetch /turn-cred → get short-lived credentials → use with TURN server.
// coturn validates the HMAC using the SAME secret on its end. Stateless.
// ============================================================================

use crate::auth_api;
use crate::fanout::RelayState;
use base64::Engine;
use hmac::{Hmac, Mac};
use sha1::Sha1;
use std::net::SocketAddr;
use std::time::{SystemTime, UNIX_EPOCH};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tracing::{info, warn};

type HmacSha1 = Hmac<Sha1>;

const CRED_LIFETIME_SECS: u64 = 3600; // 1 hour

/// Generate time-limited TURN credentials using shared secret.
pub fn generate_credentials(secret: &str, user_id: &str) -> (String, String) {
    let expiry = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs()
        + CRED_LIFETIME_SECS;
    let username = format!("{}:{}", expiry, user_id);
    let mut mac = HmacSha1::new_from_slice(secret.as_bytes()).expect("hmac key");
    mac.update(username.as_bytes());
    let signature = mac.finalize().into_bytes();
    let password = base64::engine::general_purpose::STANDARD.encode(signature);
    (username, password)
}

/// Build the JSON response for browser ICE config.
pub fn build_ice_config_json(turn_host: &str, username: &str, credential: &str) -> String {
    format!(
        r#"{{"iceServers":[{{"urls":["stun:{host}:3478"]}},{{"urls":["turn:{host}:3478?transport=udp","turn:{host}:3478?transport=tcp","turns:{host}:5349?transport=tcp"],"username":"{user}","credential":"{cred}"}}],"ttl":{ttl}}}"#,
        host = turn_host,
        user = username,
        cred = credential,
        ttl = CRED_LIFETIME_SECS
    )
}

/// Lightweight HTTP server: serves GET /turn-cred and GET /metrics.
/// Listens on a separate port from the relay WS so nginx can proxy it cleanly.
pub async fn http_listener(
    addr: SocketAddr,
    secret: Option<String>,
    turn_host: String,
    state: RelayState,
) -> std::io::Result<()> {
    let listener = TcpListener::bind(addr).await?;
    info!("HTTP endpoint ready on {} (/turn-cred /metrics /health)", addr);

    loop {
        let (stream, peer) = listener.accept().await?;
        let secret = secret.clone();
        let turn_host = turn_host.clone();
        let state = state.clone();
        tokio::spawn(async move {
            if let Err(e) = handle_http(stream, peer, secret.as_deref(), &turn_host, state).await {
                warn!("HTTP {} error: {}", peer, e);
            }
        });
    }
}

async fn handle_http(
    mut stream: TcpStream,
    peer: SocketAddr,
    secret: Option<&str>,
    turn_host: &str,
    state: RelayState,
) -> std::io::Result<()> {
    // Read the full request: headers + body. Cap at 64KB to prevent abuse.
    let mut buf = Vec::with_capacity(8192);
    let mut tmp = [0u8; 4096];
    loop {
        let n = stream.read(&mut tmp).await?;
        if n == 0 { break; }
        buf.extend_from_slice(&tmp[..n]);
        if buf.len() > 65536 { break; }
        // Stop once we have headers + declared content-length
        if let Some(headers_end) = find_double_crlf(&buf) {
            let headers = &buf[..headers_end];
            let content_length = parse_content_length(headers);
            let body_start = headers_end + 4;
            let body_have = buf.len() - body_start;
            if body_have >= content_length {
                break;
            }
        }
    }

    if buf.is_empty() {
        return Ok(());
    }

    let req = String::from_utf8_lossy(&buf);
    let first_line = req.lines().next().unwrap_or("");
    let body = req.split("\r\n\r\n").nth(1).unwrap_or("").to_string();

    let response = if first_line.starts_with("GET /turn-cred") {
        match secret {
            Some(s) => {
                let user_id = format!("anon-{}", peer.ip());
                let (username, credential) = generate_credentials(s, &user_id);
                let body = build_ice_config_json(turn_host, &username, &credential);
                format!(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n{}",
                    body.len(), body
                )
            }
            None => {
                let body = "{\"error\":\"TURN secret not configured\"}";
                format!(
                    "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                    body.len(), body
                )
            }
        }
    } else if first_line.starts_with("GET /metrics") {
        let snap = state.metrics().await;
        let body = render_prometheus(&snap);
        format!(
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            body.len(), body
        )
    } else if first_line.starts_with("GET /health") {
        let snap = state.metrics().await;
        let healthy = snap.upstream_connected;
        let code = if healthy { 200 } else { 503 };
        let body = format!(
            "{{\"healthy\":{},\"upstream\":{},\"clients\":{},\"frames\":{}}}",
            healthy, snap.upstream_connected, snap.clients, snap.frames_received
        );
        format!(
            "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            code, if healthy { "OK" } else { "Service Unavailable" }, body.len(), body
        )
    } else if first_line.starts_with("POST /api/register") {
        let resp = auth_api::handle_register(&body).await;
        let json = serde_json::to_string(&resp).unwrap_or_else(|_| "{}".to_string());
        let code = if resp.ok { 200 } else { 400 };
        format!(
            "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            code, if resp.ok { "OK" } else { "Bad Request" }, json.len(), json
        )
    } else if first_line.starts_with("POST /api/signin") {
        let resp = auth_api::handle_signin(&body).await;
        let json = serde_json::to_string(&resp).unwrap_or_else(|_| "{}".to_string());
        let code = if resp.ok { 200 } else { 401 };
        format!(
            "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            code, if resp.ok { "OK" } else { "Unauthorized" }, json.len(), json
        )
    } else {
        let body = "not found";
        format!(
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            body.len(), body
        )
    };

    stream.write_all(response.as_bytes()).await?;
    stream.shutdown().await.ok();
    Ok(())
}

/// Find the position of \r\n\r\n (end of HTTP headers).
fn find_double_crlf(buf: &[u8]) -> Option<usize> {
    buf.windows(4).position(|w| w == b"\r\n\r\n")
}

/// Parse Content-Length header from request bytes (case-insensitive).
fn parse_content_length(headers: &[u8]) -> usize {
    let s = String::from_utf8_lossy(headers);
    for line in s.lines() {
        if let Some((k, v)) = line.split_once(':') {
            if k.trim().eq_ignore_ascii_case("content-length") {
                return v.trim().parse().unwrap_or(0);
            }
        }
    }
    0
}

/// Render Prometheus 0.0.4 text format
fn render_prometheus(snap: &crate::fanout::MetricsSnapshot) -> String {
    let mut s = String::with_capacity(2048);

    // Connection state
    s.push_str("# HELP nobd_relay_upstream_connected Whether upstream flycast is connected (1=yes)\n");
    s.push_str("# TYPE nobd_relay_upstream_connected gauge\n");
    s.push_str(&format!("nobd_relay_upstream_connected {}\n", if snap.upstream_connected { 1 } else { 0 }));

    s.push_str("# HELP nobd_relay_clients Currently connected WebSocket clients\n");
    s.push_str("# TYPE nobd_relay_clients gauge\n");
    s.push_str(&format!("nobd_relay_clients {}\n", snap.clients));

    // Frame counters
    s.push_str("# HELP nobd_relay_frames_received_total Total frames received from upstream\n");
    s.push_str("# TYPE nobd_relay_frames_received_total counter\n");
    s.push_str(&format!("nobd_relay_frames_received_total {}\n", snap.frames_received));

    s.push_str("# HELP nobd_relay_frames_broadcast_total Total client frames sent (frames * recipients)\n");
    s.push_str("# TYPE nobd_relay_frames_broadcast_total counter\n");
    s.push_str(&format!("nobd_relay_frames_broadcast_total {}\n", snap.frames_broadcast));

    // Bytes
    s.push_str("# HELP nobd_relay_bytes_received_total Total bytes received from upstream\n");
    s.push_str("# TYPE nobd_relay_bytes_received_total counter\n");
    s.push_str(&format!("nobd_relay_bytes_received_total {}\n", snap.bytes_received));

    s.push_str("# HELP nobd_relay_bytes_broadcast_total Total bytes sent to all clients\n");
    s.push_str("# TYPE nobd_relay_bytes_broadcast_total counter\n");
    s.push_str(&format!("nobd_relay_bytes_broadcast_total {}\n", snap.bytes_broadcast));

    s.push_str("# HELP nobd_relay_last_frame_bytes Size of the most recent frame in bytes\n");
    s.push_str("# TYPE nobd_relay_last_frame_bytes gauge\n");
    s.push_str(&format!("nobd_relay_last_frame_bytes {}\n", snap.last_frame_size_bytes));

    // SYNC
    s.push_str("# HELP nobd_relay_sync_count_total Total SYNC frames received\n");
    s.push_str("# TYPE nobd_relay_sync_count_total counter\n");
    s.push_str(&format!("nobd_relay_sync_count_total {}\n", snap.sync_count));

    s.push_str("# HELP nobd_relay_sync_cache_bytes Size of cached SYNC frame\n");
    s.push_str("# TYPE nobd_relay_sync_cache_bytes gauge\n");
    s.push_str(&format!("nobd_relay_sync_cache_bytes {}\n", snap.sync_cache_bytes));

    // Latency / jitter
    s.push_str("# HELP nobd_relay_frame_interval_avg_us Average wall-clock interval between received frames\n");
    s.push_str("# TYPE nobd_relay_frame_interval_avg_us gauge\n");
    s.push_str(&format!("nobd_relay_frame_interval_avg_us {}\n", snap.avg_frame_interval_us));

    s.push_str("# HELP nobd_relay_frame_interval_max_us Worst-case interval between frames (jitter spike)\n");
    s.push_str("# TYPE nobd_relay_frame_interval_max_us gauge\n");
    s.push_str(&format!("nobd_relay_frame_interval_max_us {}\n", snap.max_frame_interval_us));

    s
}
