// ============================================================================
// AUTH_API — Browser-facing player registration and signin endpoints
//
// The browser cannot directly write to SurrealDB (we run it with a viewer-only
// role for safety). These endpoints proxy registration/signin requests through
// the relay, which uses the admin SurrealDB credentials (kept in env vars).
//
// Endpoints:
//   POST /api/register  body: {username, password}                → {ok|error}
//   POST /api/signin    body: {username, password}                → {ok|error, profile}
//   POST /api/leave     body: {} header: Authorization: Bearer    → {ok|error}
//     — clears any slot or queue row held by the authenticated
//       player. Used by the browser DISCONNECT button when the
//       browser can't UPDATE slot directly (collector-only table).
//
// All endpoints normalize username to lowercase. /api/leave authenticates by
// verifying the JWT against the DB and pulling the player record from `$auth`.
// ============================================================================

use serde::{Deserialize, Serialize};
use serde_json::Value as Json;
use tracing::warn;

#[derive(Debug, Deserialize)]
pub struct AuthRequest {
    pub username: String,
    pub password: String,
}

#[derive(Debug, Default, Serialize)]
pub struct AuthResponse {
    pub ok: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub profile: Option<Json>,
    /// SurrealDB JWT scoped to the `browser` record access. Browsers include
    /// this as `Authorization: Bearer …` on both REST (`surreal.mjs`) and
    /// WS (`surreal-live.mjs`) so SurrealDB enforces per-table permissions
    /// instead of running everything under the old hardcoded viewer role.
    ///
    /// Absent for legacy accounts without a pass_hash (they can still sign
    /// in but can't mint a scoped token — browser falls back to the
    /// anonymous viewer role for those).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub db_token: Option<String>,
}

impl AuthResponse {
    /// Failure response with an error string. The default for a short-circuit
    /// like "username invalid" or "wrong password".
    fn err(msg: impl Into<String>) -> Self {
        Self { ok: false, error: Some(msg.into()), ..Default::default() }
    }

    /// Success response with a scrubbed profile and (optionally) a scoped JWT.
    fn ok(profile: Json, db_token: Option<String>) -> Self {
        Self { ok: true, profile: Some(profile), db_token, ..Default::default() }
    }
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
/// pub-visible so admin_api can reuse it for the /overlord/api/* admin
/// operations (kick player, promote queue row, etc) — same admin creds,
/// same shape responses, no point duplicating the wrapper.
pub async fn sql_query_as_admin(cfg: &DbConfig, query: &str) -> Result<Json, String> {
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
        Err(e) => return AuthResponse::err(format!("bad json: {e}")),
    };

    let name = match normalize_username(&req.username) {
        Some(n) => n,
        None => return AuthResponse::err("username must be 4-32 alphanumeric chars"),
    };

    if req.password.len() < 4 || req.password.len() > 256 {
        return AuthResponse::err("password must be 4-256 chars");
    }

    let cfg = DbConfig::from_env();

    // Check if username taken
    let check_q = format!("SELECT id FROM player WHERE username = '{}';", esc(&name));
    match sql_query_as_admin(&cfg, &check_q).await {
        Ok(v) => {
            if let Some(arr) = v.get(0).and_then(|r| r.get("result")).and_then(|r| r.as_array()) {
                if !arr.is_empty() {
                    return AuthResponse::err("name already taken");
                }
            }
        }
        Err(e) => {
            warn!("register check failed: {}", e);
            return AuthResponse::err("database unavailable");
        }
    }

    // Create player with hashed password
    let create_q = format!(
        "CREATE player SET username = '{}', pass_hash = crypto::argon2::generate('{}'), created_at = time::now(), last_seen = time::now();",
        esc(&name), esc(&req.password)
    );
    match sql_query_as_admin(&cfg, &create_q).await {
        Ok(v) => {
            let profile = v.get(0)
                .and_then(|r| r.get("result"))
                .and_then(|r| r.as_array())
                .and_then(|a| a.first())
                .cloned();
            let Some(profile) = profile else {
                return AuthResponse::err("create returned empty");
            };
            // Fresh player has the password we just set — mint a browser JWT
            // so the client can write chat/queue rows under its own scope.
            let db_token = mint_browser_token(&cfg, &name, &req.password).await;
            AuthResponse::ok(scrub_profile(profile), db_token)
        }
        Err(e) => {
            warn!("register create failed: {}", e);
            AuthResponse::err(format!("create failed: {e}"))
        }
    }
}

pub async fn handle_signin(body: &str) -> AuthResponse {
    let req: AuthRequest = match serde_json::from_str(body) {
        Ok(r) => r,
        Err(e) => return AuthResponse::err(format!("bad json: {e}")),
    };

    let name = match normalize_username(&req.username) {
        Some(n) => n,
        None => return AuthResponse::err("invalid username"),
    };

    let cfg = DbConfig::from_env();

    // Fetch player + verify password in one query
    let q = format!(
        "SELECT *, crypto::argon2::compare(pass_hash, '{}') AS pass_ok FROM player WHERE username = '{}';",
        esc(&req.password), esc(&name)
    );
    let v = match sql_query_as_admin(&cfg, &q).await {
        Ok(v) => v,
        Err(e) => {
            warn!("signin query failed: {}", e);
            return AuthResponse::err("database unavailable");
        }
    };

    let row = v.get(0)
        .and_then(|r| r.get("result"))
        .and_then(|r| r.as_array())
        .and_then(|a| a.first());

    let row = match row {
        Some(r) => r,
        None => return AuthResponse::err("fighter not found"),
    };

    // pass_hash may be missing for legacy accounts — let them through but
    // we won't be able to mint a browser JWT for them (no argon2 compare).
    let has_hash = row.get("pass_hash").map(|v| !v.is_null()).unwrap_or(false);
    if has_hash {
        let pass_ok = row.get("pass_ok").and_then(|v| v.as_bool()).unwrap_or(false);
        if !pass_ok {
            return AuthResponse::err("wrong password");
        }
    }

    // Update last_seen (fire-and-forget)
    let update_q = format!("UPDATE player SET last_seen = time::now() WHERE username = '{}';", esc(&name));
    let _ = sql_query_as_admin(&cfg, &update_q).await;

    // Mint a browser-scoped JWT so subsequent writes go under the player's
    // own record access. Legacy accounts without pass_hash silently get
    // None here and the browser falls back to the anonymous viewer role.
    let db_token = if has_hash {
        mint_browser_token(&cfg, &name, &req.password).await
    } else {
        None
    };

    AuthResponse::ok(scrub_profile(row.clone()), db_token)
}

/// Sign in to SurrealDB via the `browser` record access with the user's
/// plaintext password. Returns the scoped JWT on success. Called from both
/// `handle_register` (right after the account is created) and
/// `handle_signin` (after the relay-side argon2 compare passes).
///
/// Failure returns `None` — the caller should proceed with the auth flow
/// without a token rather than failing outright, since the browser's
/// unauthenticated-viewer fallback can still render the page.
async fn mint_browser_token(cfg: &DbConfig, username: &str, password: &str) -> Option<String> {
    #[derive(Serialize)]
    struct SigninBody<'a> {
        ns: &'a str,
        db: &'a str,
        ac: &'a str,
        username: &'a str,
        password: &'a str,
    }

    let body = SigninBody {
        ns: &cfg.ns,
        db: &cfg.db,
        ac: "browser",
        username,
        password,
    };

    let client = reqwest::Client::new();
    let res = match client
        .post(format!("{}/signin", cfg.url))
        .header("Accept", "application/json")
        .json(&body)
        .send()
        .await
    {
        Ok(r) => r,
        Err(e) => {
            warn!("mint_browser_token network: {}", e);
            return None;
        }
    };

    if !res.status().is_success() {
        warn!("mint_browser_token http {}", res.status());
        return None;
    }

    // Response shape: { "code": 200, "details": "...", "token": "eyJ…" }
    let v: Json = res.json().await.ok()?;
    v.get("token")?.as_str().map(|s| s.to_string())
}

/// Strip pass_hash from profile before sending to browser.
fn scrub_profile(mut p: Json) -> Json {
    if let Some(obj) = p.as_object_mut() {
        obj.remove("pass_hash");
        obj.remove("pass_ok");
    }
    p
}

/// Verify a bearer JWT and return true iff the authenticated player has
/// `admin = true` in their record. This is the SOLE access gate for every
/// `/overlord/api/*` write endpoint. No fallback, no shared secret, no
/// emergency override — if you lose access, SSH into the VPS and flip the
/// bool directly in SurrealDB.
///
/// Returns false on ANY failure: missing token, invalid token, expired,
/// network down, schema mismatch, admin field absent, admin field false.
/// Defense in depth: false-by-default for everything that's not an
/// explicit yes.
///
/// Two-step query, mirrors `handle_leave`:
///   1. With the browser's own token: `RETURN $auth;`
///      Validates the JWT signature/expiry server-side and returns the
///      player record id (e.g. "player:35c7pk94...").
///   2. With ADMIN creds: `SELECT admin FROM <id>;`
///      Reads the field. We can't use the browser token for this because
///      the player table is locked down at the table-permissions level
///      (record-access reads return empty post-auth). The admin lookup is
///      safe because we already verified the JWT in step 1.
///
/// See docs/WORKSTREAM-OVERLORD.md Phase B for the design rationale.
pub async fn check_admin(authorization: Option<&str>) -> bool {
    let token = match authorization.and_then(|h| h.strip_prefix("Bearer ")) {
        Some(t) if !t.is_empty() => t,
        _ => return false,
    };
    let cfg = DbConfig::from_env();
    let client = reqwest::Client::new();

    // Step 1: validate the token + extract the record id via $auth.
    let who_res = match client
        .post(format!("{}/sql", cfg.url))
        .bearer_auth(token)
        .header("Surreal-NS", &cfg.ns)
        .header("Surreal-DB", &cfg.db)
        .header("Accept", "application/json")
        .body("RETURN $auth;")
        .send()
        .await
    {
        Ok(r) => r,
        Err(e) => {
            warn!("check_admin: token validate network: {}", e);
            return false;
        }
    };
    if !who_res.status().is_success() {
        // Bad token, expired, etc. — silently deny.
        return false;
    }
    let parsed: Json = match who_res.json().await {
        Ok(v) => v,
        Err(e) => {
            warn!("check_admin: token validate parse: {}", e);
            return false;
        }
    };
    // Response shape: [{"result": "player:xxx", "status": "OK", ...}]
    let record_id = parsed
        .as_array()
        .and_then(|a| a.first())
        .and_then(|stmt| stmt.get("result"))
        .and_then(|v| v.as_str())
        .map(|s| s.to_string());
    let record_id = match record_id {
        Some(r) if r.starts_with("player:") => r,
        _ => return false,
    };

    // Step 2: read the admin field with admin creds. The id came from
    // a verified JWT, so this is not user-controlled — no SQL injection
    // surface (record ids are alphanumeric).
    let q = format!("SELECT admin FROM {};", record_id);
    let v = match sql_query_as_admin(&cfg, &q).await {
        Ok(v) => v,
        Err(e) => {
            warn!("check_admin: admin lookup failed: {}", e);
            return false;
        }
    };
    // Response shape: [{"result": [{"admin": true}], "status": "OK", ...}]
    v.as_array()
        .and_then(|a| a.first())
        .and_then(|stmt| stmt.get("result"))
        .and_then(|r| r.as_array())
        .and_then(|arr| arr.first())
        .and_then(|obj| obj.get("admin"))
        .and_then(|v| v.as_bool())
        .unwrap_or(false)
}

/// POST /api/leave — authenticated slot + queue clear.
///
/// The browser sends the scoped JWT it got from /api/signin in the
/// `Authorization: Bearer` header. We call SurrealDB's `/key/player/<id>`
/// endpoint with that token to verify it's valid and extract the player's
/// username, then run admin-privileged UPDATE/DELETE to free any slot row
/// holding that username and delete any waiting queue row for it.
///
/// Why admin-privileged here: the slot table is PERMISSIONS NONE for the
/// `browser` record access (collector-only). The browser can't clear its
/// own slot, so we proxy the write through this endpoint using the relay's
/// admin credentials. The JWT check is what makes it safe — no random POST
/// can nuke someone else's slot.
pub async fn handle_leave(authorization: Option<&str>) -> AuthResponse {
    let token = match authorization.and_then(|h| h.strip_prefix("Bearer ")) {
        Some(t) if !t.is_empty() => t,
        _ => return AuthResponse::err("missing bearer token"),
    };
    let cfg = DbConfig::from_env();

    // Resolve the authenticated player record id from the JWT.
    //
    // We call SurrealDB's /sql with the browser's own token and `RETURN $auth`.
    // This returns the record id of the player the token authenticates as,
    // e.g. `player:35c7pk94bmzvpu7bh2nm`. The call verifies the token is
    // valid in the process — if the signature is bad or the token expired,
    // SurrealDB returns a 401 and we bail.
    //
    // We DELIBERATELY don't try to `SELECT username FROM $auth` with the
    // browser token: the `player` table permissions deny record-access reads
    // post-auth, so that query comes back empty even for valid tokens. We
    // do the username lookup below using admin creds instead.
    let client = reqwest::Client::new();
    let who_res = match client
        .post(format!("{}/sql", cfg.url))
        .bearer_auth(token)
        .header("Surreal-NS", &cfg.ns)
        .header("Surreal-DB", &cfg.db)
        .header("Accept", "application/json")
        .body("RETURN $auth;")
        .send()
        .await
    {
        Ok(r) => r,
        Err(e) => return AuthResponse::err(format!("db network: {e}")),
    };
    if !who_res.status().is_success() {
        return AuthResponse::err("token rejected");
    }
    let parsed: Json = match who_res.json().await {
        Ok(v) => v,
        Err(e) => return AuthResponse::err(format!("bad db response: {e}")),
    };
    // Response shape: [{"result": "player:xxx", "status": "OK", ...}]
    let record_id = parsed
        .as_array()
        .and_then(|a| a.first())
        .and_then(|stmt| stmt.get("result"))
        .and_then(|v| v.as_str())
        .map(|s| s.to_string());
    let record_id = match record_id {
        Some(r) if r.starts_with("player:") => r,
        _ => return AuthResponse::err("no player on token"),
    };

    // Look up the username with admin creds (browser token can't SELECT
    // player rows due to table-level PERMISSIONS). Safe: we've already
    // verified the JWT resolves to a real player record via $auth above.
    let lookup_sql = format!("SELECT VALUE username FROM {};", record_id);
    let lookup = match sql_query_as_admin(&cfg, &lookup_sql).await {
        Ok(v) => v,
        Err(e) => return AuthResponse::err(format!("username lookup: {e}")),
    };
    let username = lookup
        .as_array()
        .and_then(|a| a.first())
        .and_then(|stmt| stmt.get("result"))
        .and_then(|r| r.as_array())
        .and_then(|arr| arr.first())
        .and_then(|v| v.as_str())
        .map(|s| s.to_string());
    let username = match username {
        Some(u) if !u.is_empty() => u,
        _ => return AuthResponse::err("username not found"),
    };

    // Now do the privileged clear using the admin SQL path. Both the slot
    // clear and the queue delete are safe to run unconditionally — idempotent
    // no-ops if the user isn't actually in either place.
    let display = username.to_uppercase();
    let display_esc = esc(&display);
    let uname_esc = esc(&username);
    let sql = format!(
        "UPDATE slot SET \
            occupant_name = NONE, \
            player_record = NONE, \
            device = NONE, \
            session_id = NONE, \
            claimed_at = NONE, \
            last_input_at = NONE, \
            grace_expires_at = NONE \
         WHERE occupant_name = '{display_esc}'; \
         DELETE queue WHERE username = '{display_esc}' OR username = '{uname_esc}';"
    );
    match sql_query_as_admin(&cfg, &sql).await {
        Ok(_) => AuthResponse { ok: true, ..Default::default() },
        Err(e) => AuthResponse::err(format!("clear failed: {e}")),
    }
}
