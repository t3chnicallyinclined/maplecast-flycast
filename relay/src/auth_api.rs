// ============================================================================
// AUTH_API — Browser-facing player registration and signin endpoints
//
// The browser cannot directly write to SurrealDB (we run it with a viewer-only
// role for safety). These endpoints proxy registration/signin requests through
// the relay, which uses the admin SurrealDB credentials (kept in env vars).
//
// Endpoints:
//   POST /api/register  body: {username, password}     → {ok|error}
//   POST /api/signin    body: {username, password}     → {ok|error, profile}
//
// Both endpoints normalize username to lowercase and reject duplicates.
// Passwords are hashed server-side via SurrealDB's crypto::argon2::generate.
// ============================================================================

use serde::{Deserialize, Serialize};
use serde_json::Value as Json;
use tracing::warn;

#[derive(Debug, Deserialize)]
pub struct AuthRequest {
    pub username: String,
    pub password: String,
}

#[derive(Debug, Serialize)]
pub struct AuthResponse {
    pub ok: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub profile: Option<Json>,
}

pub struct DbConfig {
    pub url: String,         // e.g. http://127.0.0.1:8000
    pub ns: String,          // namespace
    pub db: String,          // database
    pub user: String,        // admin username
    pub pass: String,        // admin password
}

impl DbConfig {
    pub fn from_env() -> Self {
        Self {
            url: std::env::var("NOBD_DB_URL").unwrap_or_else(|_| "http://127.0.0.1:8000".to_string()),
            ns: std::env::var("NOBD_DB_NS").unwrap_or_else(|_| "maplecast".to_string()),
            db: std::env::var("NOBD_DB_DATABASE").unwrap_or_else(|_| "arcade".to_string()),
            user: std::env::var("NOBD_DB_USER").unwrap_or_else(|_| "root".to_string()),
            pass: std::env::var("NOBD_DB_PASS").unwrap_or_else(|_| "root".to_string()),
        }
    }
}

/// Send a SurrealDB SQL query as the admin user. Returns parsed JSON.
async fn sql_query(cfg: &DbConfig, query: &str) -> Result<Json, String> {
    let client = reqwest::Client::new();
    let res = client
        .post(format!("{}/sql", cfg.url))
        .basic_auth(&cfg.user, Some(&cfg.pass))
        .header("Surreal-NS", &cfg.ns)
        .header("Surreal-DB", &cfg.db)
        .header("Accept", "application/json")
        .body(query.to_string())
        .send()
        .await
        .map_err(|e| format!("network: {e}"))?;

    if !res.status().is_success() {
        return Err(format!("http {}", res.status()));
    }

    res.json::<Json>().await.map_err(|e| format!("json: {e}"))
}

/// Sanitize username — lowercase, alphanumeric/underscore only, 4-32 chars.
fn normalize_username(raw: &str) -> Option<String> {
    let lower = raw.trim().to_lowercase();
    if lower.len() < 4 || lower.len() > 32 {
        return None;
    }
    if !lower.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') {
        return None;
    }
    Some(lower)
}

/// SurrealDB string literal escape: replace `'` with `\'`.
fn esc(s: &str) -> String {
    s.replace('\\', "\\\\").replace('\'', "\\'")
}

pub async fn handle_register(body: &str) -> AuthResponse {
    let req: AuthRequest = match serde_json::from_str(body) {
        Ok(r) => r,
        Err(e) => return AuthResponse { ok: false, error: Some(format!("bad json: {e}")), profile: None },
    };

    let name = match normalize_username(&req.username) {
        Some(n) => n,
        None => return AuthResponse { ok: false, error: Some("username must be 4-32 alphanumeric chars".into()), profile: None },
    };

    if req.password.len() < 4 || req.password.len() > 256 {
        return AuthResponse { ok: false, error: Some("password must be 4-256 chars".into()), profile: None };
    }

    let cfg = DbConfig::from_env();

    // Check if username taken
    let check_q = format!("SELECT id FROM player WHERE username = '{}';", esc(&name));
    match sql_query(&cfg, &check_q).await {
        Ok(v) => {
            if let Some(arr) = v.get(0).and_then(|r| r.get("result")).and_then(|r| r.as_array()) {
                if !arr.is_empty() {
                    return AuthResponse { ok: false, error: Some("name already taken".into()), profile: None };
                }
            }
        }
        Err(e) => {
            warn!("register check failed: {}", e);
            return AuthResponse { ok: false, error: Some("database unavailable".into()), profile: None };
        }
    }

    // Create player with hashed password
    let create_q = format!(
        "CREATE player SET username = '{}', pass_hash = crypto::argon2::generate('{}'), created_at = time::now(), last_seen = time::now();",
        esc(&name), esc(&req.password)
    );
    match sql_query(&cfg, &create_q).await {
        Ok(v) => {
            if let Some(arr) = v.get(0).and_then(|r| r.get("result")).and_then(|r| r.as_array()) {
                if let Some(profile) = arr.first() {
                    return AuthResponse { ok: true, error: None, profile: Some(scrub_profile(profile.clone())) };
                }
            }
            AuthResponse { ok: false, error: Some("create returned empty".into()), profile: None }
        }
        Err(e) => {
            warn!("register create failed: {}", e);
            AuthResponse { ok: false, error: Some(format!("create failed: {e}")), profile: None }
        }
    }
}

pub async fn handle_signin(body: &str) -> AuthResponse {
    let req: AuthRequest = match serde_json::from_str(body) {
        Ok(r) => r,
        Err(e) => return AuthResponse { ok: false, error: Some(format!("bad json: {e}")), profile: None },
    };

    let name = match normalize_username(&req.username) {
        Some(n) => n,
        None => return AuthResponse { ok: false, error: Some("invalid username".into()), profile: None },
    };

    let cfg = DbConfig::from_env();

    // Fetch player + verify password in one query
    let q = format!(
        "SELECT *, crypto::argon2::compare(pass_hash, '{}') AS pass_ok FROM player WHERE username = '{}';",
        esc(&req.password), esc(&name)
    );
    let v = match sql_query(&cfg, &q).await {
        Ok(v) => v,
        Err(e) => {
            warn!("signin query failed: {}", e);
            return AuthResponse { ok: false, error: Some("database unavailable".into()), profile: None };
        }
    };

    let row = v.get(0)
        .and_then(|r| r.get("result"))
        .and_then(|r| r.as_array())
        .and_then(|a| a.first());

    let row = match row {
        Some(r) => r,
        None => return AuthResponse { ok: false, error: Some("fighter not found".into()), profile: None },
    };

    // pass_hash may be missing for legacy accounts — let them through
    let has_hash = row.get("pass_hash").map(|v| !v.is_null()).unwrap_or(false);
    if has_hash {
        let pass_ok = row.get("pass_ok").and_then(|v| v.as_bool()).unwrap_or(false);
        if !pass_ok {
            return AuthResponse { ok: false, error: Some("wrong password".into()), profile: None };
        }
    }

    // Update last_seen (fire-and-forget)
    let update_q = format!("UPDATE player SET last_seen = time::now() WHERE username = '{}';", esc(&name));
    let _ = sql_query(&cfg, &update_q).await;

    AuthResponse { ok: true, error: None, profile: Some(scrub_profile(row.clone())) }
}

/// Strip pass_hash from profile before sending to browser.
fn scrub_profile(mut p: Json) -> Json {
    if let Some(obj) = p.as_object_mut() {
        obj.remove("pass_hash");
        obj.remove("pass_ok");
    }
    p
}
