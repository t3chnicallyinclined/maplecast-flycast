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

use crate::admin_api;
use crate::auth_api;
use crate::client_telemetry::{ClientReport, ClientTelemetry};
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

/// Lightweight HTTP server: serves GET /turn-cred, /metrics, /health,
/// and POST /api/register, /api/signin, /api/telemetry.
/// Listens on a separate port from the relay WS so nginx can proxy it cleanly.
pub async fn http_listener(
    addr: SocketAddr,
    secret: Option<String>,
    turn_host: String,
    state: RelayState,
    telemetry: ClientTelemetry,
) -> std::io::Result<()> {
    let listener = TcpListener::bind(addr).await?;
    info!("HTTP endpoint ready on {} (/turn-cred /metrics /health /api/*)", addr);

    loop {
        let (stream, peer) = listener.accept().await?;
        let secret = secret.clone();
        let turn_host = turn_host.clone();
        let state = state.clone();
        let telemetry = telemetry.clone();
        tokio::spawn(async move {
            if let Err(e) = handle_http(stream, peer, secret.as_deref(), &turn_host, state, telemetry).await {
                warn!("HTTP {} error: {}", peer, e);
            }
        });
    }
}

// ============================================================================
// Helpers for /overlord/api/* responses. The admin handlers return their
// own JSON envelope, so these wrap an already-built JSON string in the
// HTTP response. CORS headers included so the admin SPA at /overlord/
// can call these from a (slightly) different origin if needed.
// ============================================================================
fn ok_json(json: &str) -> String {
    format!(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{}",
        json.len(), json
    )
}

fn forbidden_json() -> String {
    let body = r#"{"ok":false,"error":"forbidden — admin role required"}"#;
    format!(
        "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\nContent-Length: {}\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{}",
        body.len(), body
    )
}

fn err_json_with_code(code: u16, msg: &str) -> String {
    let body = format!(r#"{{"ok":false,"error":"{}"}}"#, msg.replace('"', "\\\""));
    let reason = match code {
        400 => "Bad Request",
        404 => "Not Found",
        500 => "Internal Server Error",
        _ => "Error",
    };
    format!(
        "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{}",
        code, reason, body.len(), body
    )
}

async fn handle_http(
    mut stream: TcpStream,
    peer: SocketAddr,
    secret: Option<&str>,
    turn_host: &str,
    state: RelayState,
    telemetry: ClientTelemetry,
) -> std::io::Result<()> {
    // Read the full request: headers + body.
    //
    // Body cap is normally 64 KB to prevent abuse. The savestate upload
    // endpoint (/overlord/api/savestates/upload) is the only route that
    // accepts files >64 KB — we bump its cap to 64 MB. Per-route detection
    // happens after we have enough data to parse the request line.
    const SMALL_BODY_CAP: usize = 64 * 1024;
    const LARGE_BODY_CAP: usize = 64 * 1024 * 1024;

    let mut buf = Vec::with_capacity(8192);
    let mut tmp = [0u8; 4096];
    let mut body_cap = SMALL_BODY_CAP;
    let mut headers_parsed = false;
    loop {
        let n = stream.read(&mut tmp).await?;
        if n == 0 { break; }
        buf.extend_from_slice(&tmp[..n]);

        if !headers_parsed {
            // Look at the request line as soon as we can to bump the cap
            // for the upload route.
            if let Some(headers_end) = find_double_crlf(&buf) {
                headers_parsed = true;
                let headers_lossy = String::from_utf8_lossy(&buf[..headers_end]);
                let first_line = headers_lossy.lines().next().unwrap_or("");
                if first_line.starts_with("POST /overlord/api/savestates/upload") {
                    body_cap = LARGE_BODY_CAP;
                }
            }
        }

        if buf.len() > body_cap { break; }
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
    // For binary endpoints that need raw bytes (multipart upload), we use
    // body_bytes which preserves non-UTF8 content.
    let headers_end = find_double_crlf(&buf).unwrap_or(0);
    let body_bytes_start = if headers_end > 0 { headers_end + 4 } else { 0 };
    let body_bytes: &[u8] = &buf[body_bytes_start..];

    // Helper: extract `Authorization: Bearer <token>` from request lines.
    let auth_header: Option<String> = req
        .lines()
        .find(|l| l.to_ascii_lowercase().starts_with("authorization:"))
        .and_then(|l| l.split_once(':'))
        .map(|(_, v)| v.trim().to_string());

    // Helper: parse query string from request line ("GET /foo?a=1&b=2 HTTP/1.1" → "a=1&b=2")
    let query_string: String = first_line
        .split_whitespace()
        .nth(1)
        .and_then(|p| p.split_once('?'))
        .map(|(_, q)| q.to_string())
        .unwrap_or_default();

    // Helper: extract Content-Type header
    let content_type: String = req
        .lines()
        .find(|l| l.to_ascii_lowercase().starts_with("content-type:"))
        .and_then(|l| l.split_once(':'))
        .map(|(_, v)| v.trim().to_string())
        .unwrap_or_default();

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
        let mut body = render_prometheus(&snap);
        // Append per-client telemetry aggregates
        let agg = telemetry.snapshot().await;
        agg.render_prometheus(&mut body);
        format!(
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            body.len(), body
        )
    } else if first_line.starts_with("POST /api/telemetry") {
        // Browser pushes client RTT/FPS/jitter — fire and forget, always 200
        match serde_json::from_str::<ClientReport>(&body) {
            Ok(report) => telemetry.ingest(report).await,
            Err(_) => {} // bad payload, just drop it
        }
        let body = "{\"ok\":true}";
        format!(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
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
    } else if first_line.starts_with("POST /api/join") {
        let resp = auth_api::handle_join(auth_header.as_deref(), &body).await;
        let json = serde_json::to_string(&resp).unwrap_or_else(|_| "{}".to_string());
        let code = if resp.ok { 200 } else { 401 };
        format!(
            "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{}",
            code, if resp.ok { "OK" } else { "Unauthorized" }, json.len(), json
        )
    } else if first_line.starts_with("POST /api/leave") {
        let resp = auth_api::handle_leave(auth_header.as_deref()).await;
        let json = serde_json::to_string(&resp).unwrap_or_else(|_| "{}".to_string());
        let code = if resp.ok { 200 } else { 401 };
        format!(
            "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{}",
            code, if resp.ok { "OK" } else { "Unauthorized" }, json.len(), json
        )

    // ====================================================================
    // /overlord/* — admin panel routes
    // ====================================================================
    //
    // Every write endpoint is gated by auth_api::check_admin() against the
    // bearer JWT. Read endpoints (status, list savestates, read config,
    // tail logs) are also gated — there's no anonymous /overlord access.
    //
    // POST /overlord/api/signin — special: this is the login flow itself,
    // so it can't pre-check admin. Instead, it runs the normal signin and
    // THEN checks admin against the just-issued token. If admin is false,
    // returns 403 even though the credentials were valid.
    } else if first_line.starts_with("POST /overlord/api/signin") {
        let signin_resp = auth_api::handle_signin(&body).await;
        if !signin_resp.ok {
            let json = serde_json::to_string(&signin_resp).unwrap_or_else(|_| "{}".to_string());
            format!(
                "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: {}\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{}",
                json.len(), json
            )
        } else {
            // Use the just-issued db_token to verify admin role.
            let token = signin_resp.db_token.as_deref();
            let bearer = token.map(|t| format!("Bearer {}", t));
            let is_admin = auth_api::check_admin(bearer.as_deref()).await;
            if is_admin {
                let json = serde_json::to_string(&signin_resp).unwrap_or_else(|_| "{}".to_string());
                format!(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{}",
                    json.len(), json
                )
            } else {
                let body = r#"{"ok":false,"error":"not an admin"}"#;
                format!(
                    "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\nContent-Length: {}\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{}",
                    body.len(), body
                )
            }
        }
    } else if first_line.starts_with("GET /overlord/api/status") {
        if !auth_api::check_admin(auth_header.as_deref()).await {
            forbidden_json()
        } else {
            ok_json(&admin_api::handle_status(&state).await.to_json_string())
        }
    } else if first_line.starts_with("GET /overlord/api/savestates/download/") {
        if !auth_api::check_admin(auth_header.as_deref()).await {
            forbidden_json()
        } else {
            // Path: /overlord/api/savestates/download/<slot>
            let slot_str = first_line
                .split_whitespace()
                .nth(1)
                .and_then(|p| p.strip_prefix("/overlord/api/savestates/download/"))
                .map(|s| s.split('?').next().unwrap_or(s))
                .unwrap_or("");
            let slot = slot_str.parse::<i32>().unwrap_or(-1);
            match admin_api::handle_savestate_download(slot).await {
                Ok(bytes) => {
                    let fname = if slot == 0 {
                        format!("{}.state", admin_api::rom_basename())
                    } else {
                        format!("{}_{}.state", admin_api::rom_basename(), slot)
                    };
                    // Build the response headers separately so we can ship raw bytes.
                    let header = format!(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Disposition: attachment; filename=\"{}\"\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                        fname, bytes.len()
                    );
                    stream.write_all(header.as_bytes()).await?;
                    stream.write_all(&bytes).await?;
                    stream.shutdown().await.ok();
                    return Ok(());
                }
                Err(e) => err_json_with_code(500, &e),
            }
        }
    } else if first_line.starts_with("GET /overlord/api/savestates") {
        if !auth_api::check_admin(auth_header.as_deref()).await {
            forbidden_json()
        } else {
            ok_json(&admin_api::handle_list_savestates().await.to_json_string())
        }
    } else if first_line.starts_with("POST /overlord/api/savestates/save") {
        if !auth_api::check_admin(auth_header.as_deref()).await {
            forbidden_json()
        } else {
            ok_json(&admin_api::handle_savestate_save(&body).await.to_json_string())
        }
    } else if first_line.starts_with("POST /overlord/api/savestates/load") {
        if !auth_api::check_admin(auth_header.as_deref()).await {
            forbidden_json()
        } else {
            ok_json(&admin_api::handle_savestate_load(&body).await.to_json_string())
        }
    } else if first_line.starts_with("POST /overlord/api/savestates/set-autoload") {
        if !auth_api::check_admin(auth_header.as_deref()).await {
            forbidden_json()
        } else {
            ok_json(&admin_api::handle_set_autoload_slot(&body).await.to_json_string())
        }
    } else if first_line.starts_with("GET /overlord/api/players") {
        if !auth_api::check_admin(auth_header.as_deref()).await {
            forbidden_json()
        } else {
            ok_json(&admin_api::handle_players().await.to_json_string())
        }
    } else if first_line.starts_with("POST /overlord/api/players/kick") {
        if !auth_api::check_admin(auth_header.as_deref()).await {
            forbidden_json()
        } else {
            ok_json(&admin_api::handle_kick_player(&body).await.to_json_string())
        }
    } else if first_line.starts_with("POST /overlord/api/queue/kick") {
        if !auth_api::check_admin(auth_header.as_deref()).await {
            forbidden_json()
        } else {
            ok_json(&admin_api::handle_kick_queue(&body).await.to_json_string())
        }
    } else if first_line.starts_with("POST /overlord/api/queue/promote") {
        if !auth_api::check_admin(auth_header.as_deref()).await {
            forbidden_json()
        } else {
            ok_json(&admin_api::handle_promote_queue(&body).await.to_json_string())
        }
    } else if first_line.starts_with("POST /overlord/api/savestates/upload") {
        if !auth_api::check_admin(auth_header.as_deref()).await {
            forbidden_json()
        } else {
            ok_json(&admin_api::handle_savestate_upload(&query_string, &content_type, body_bytes).await.to_json_string())
        }
    } else if first_line.starts_with("GET /overlord/api/config") {
        if !auth_api::check_admin(auth_header.as_deref()).await {
            forbidden_json()
        } else {
            ok_json(&admin_api::handle_config_read().await.to_json_string())
        }
    } else if first_line.starts_with("POST /overlord/api/config") {
        if !auth_api::check_admin(auth_header.as_deref()).await {
            forbidden_json()
        } else {
            ok_json(&admin_api::handle_config_write(&body).await.to_json_string())
        }
    } else if first_line.starts_with("POST /overlord/api/service/restart") {
        if !auth_api::check_admin(auth_header.as_deref()).await {
            forbidden_json()
        } else {
            ok_json(&admin_api::handle_service_restart().await.to_json_string())
        }
    } else if first_line.starts_with("POST /overlord/api/reset") {
        if !auth_api::check_admin(auth_header.as_deref()).await {
            forbidden_json()
        } else {
            ok_json(&admin_api::handle_reset().await.to_json_string())
        }
    } else if first_line.starts_with("GET /overlord/api/logs/tail") {
        if !auth_api::check_admin(auth_header.as_deref()).await {
            forbidden_json()
        } else {
            ok_json(&admin_api::handle_logs_tail(&query_string).await.to_json_string())
        }
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

    // Audio (tracked separately so per-frame video metrics above aren't
    // polluted by ~86 audio packets/sec. Audio packets are 2052 bytes:
    // [0xAD][0x10][seqHi][seqLo][512 × int16 stereo PCM]).
    s.push_str("# HELP nobd_relay_audio_packets_received_total Total audio PCM packets from upstream\n");
    s.push_str("# TYPE nobd_relay_audio_packets_received_total counter\n");
    s.push_str(&format!("nobd_relay_audio_packets_received_total {}\n", snap.audio_packets_received));

    s.push_str("# HELP nobd_relay_audio_packets_broadcast_total Total audio packets sent to clients (packets * recipients)\n");
    s.push_str("# TYPE nobd_relay_audio_packets_broadcast_total counter\n");
    s.push_str(&format!("nobd_relay_audio_packets_broadcast_total {}\n", snap.audio_packets_broadcast));

    s.push_str("# HELP nobd_relay_audio_bytes_received_total Total audio bytes from upstream\n");
    s.push_str("# TYPE nobd_relay_audio_bytes_received_total counter\n");
    s.push_str(&format!("nobd_relay_audio_bytes_received_total {}\n", snap.audio_bytes_received));

    s.push_str("# HELP nobd_relay_audio_bytes_broadcast_total Total audio bytes sent to clients\n");
    s.push_str("# TYPE nobd_relay_audio_bytes_broadcast_total counter\n");
    s.push_str(&format!("nobd_relay_audio_bytes_broadcast_total {}\n", snap.audio_bytes_broadcast));

    s
}
