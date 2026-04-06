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

/// Lightweight HTTP server: only serves GET /turn-cred.
/// Listens on a separate port from the relay WS so nginx can proxy it cleanly.
pub async fn http_listener(
    addr: SocketAddr,
    secret: String,
    turn_host: String,
) -> std::io::Result<()> {
    let listener = TcpListener::bind(addr).await?;
    info!("HTTP turn-cred endpoint ready on {}", addr);

    loop {
        let (stream, peer) = listener.accept().await?;
        let secret = secret.clone();
        let turn_host = turn_host.clone();
        tokio::spawn(async move {
            if let Err(e) = handle_http(stream, peer, &secret, &turn_host).await {
                warn!("HTTP {} error: {}", peer, e);
            }
        });
    }
}

async fn handle_http(
    mut stream: TcpStream,
    peer: SocketAddr,
    secret: &str,
    turn_host: &str,
) -> std::io::Result<()> {
    let mut buf = [0u8; 1024];
    let n = stream.read(&mut buf).await?;
    if n == 0 {
        return Ok(());
    }

    let req = String::from_utf8_lossy(&buf[..n]);
    let first_line = req.lines().next().unwrap_or("");

    let response = if first_line.starts_with("GET /turn-cred") {
        // Use peer IP as user ID (no auth — anonymous, time-limited only)
        let user_id = format!("anon-{}", peer.ip());
        let (username, credential) = generate_credentials(secret, &user_id);
        let body = build_ice_config_json(turn_host, &username, &credential);
        format!(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n{}",
            body.len(),
            body
        )
    } else {
        let body = "not found";
        format!(
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            body.len(),
            body
        )
    };

    stream.write_all(response.as_bytes()).await?;
    stream.shutdown().await.ok();
    Ok(())
}
