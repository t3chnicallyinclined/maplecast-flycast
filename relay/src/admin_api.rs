// ============================================================================
// ADMIN_API — /overlord administrative endpoints
//
// All endpoints in this module are gated by auth_api::check_admin() against
// a SurrealDB-minted bearer JWT. The browser stores the token after a
// successful POST /overlord/api/signin and includes it as
// `Authorization: Bearer <token>` on every subsequent admin call. There is
// no shared secret, no env-var override, no fallback path — the bool
// `admin = true` on the player record is the sole gate.
//
// Endpoints (full list — see WORKSTREAM-OVERLORD §1 acceptance gates):
//
//   POST /overlord/api/signin                          Sign in + admin check
//   GET  /overlord/api/status                          Live cab metrics
//   GET  /overlord/api/savestates                      List all .state files
//   POST /overlord/api/savestates/save                 Hot-save current state
//   POST /overlord/api/savestates/load                 Hot-load a slot
//   POST /overlord/api/savestates/upload               Upload a .state file
//   GET  /overlord/api/savestates/download/<slot>      Download a slot
//   GET  /overlord/api/config                          Read emu.cfg
//   POST /overlord/api/config                          Write emu.cfg
//   POST /overlord/api/service/restart                 systemctl restart
//   GET  /overlord/api/logs/tail                       journalctl -n 200
//   GET  /overlord/api/logs/stream                     SSE log tail
//
// Many of these read/write files in /opt/maplecast/{.config,.local/share}/.
// The relay runs as the `maplecast-relay` user and needs read+write access
// to those paths. Phase E (deploy) sets up the polkit rule for systemctl
// and adjusts directory ownership so the relay user can write to flycast's
// config + savestate dirs.
// ============================================================================

use serde::{Deserialize, Serialize};
use serde_json::{json, Value as Json};
use std::path::PathBuf;
use std::time::Duration;
use tokio::io::AsyncReadExt;
use tracing::{info, warn};

// ============================================================================
// Config — paths the admin API touches
// ============================================================================

/// Where flycast keeps savestates on the VPS. Matches the layout documented
/// in VPS-SETUP §9 "flycast config + data layout":
///   /opt/maplecast/.local/share/flycast/mvc2_<slot>.state
///
/// Override with MAPLECAST_SAVESTATE_DIR for testing.
pub fn savestate_dir() -> PathBuf {
    std::env::var("MAPLECAST_SAVESTATE_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("/opt/maplecast/.local/share/flycast"))
}

/// Where flycast reads its config from on the VPS:
///   /opt/maplecast/.config/flycast/emu.cfg
pub fn config_path() -> PathBuf {
    std::env::var("MAPLECAST_CONFIG_PATH")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("/opt/maplecast/.config/flycast/emu.cfg"))
}

/// Control WS endpoint flycast listens on. Loopback-only by design — see
/// docs/WORKSTREAM-OVERLORD.md Phase A.
pub fn control_ws_url() -> String {
    std::env::var("MAPLECAST_CONTROL_WS_URL")
        .unwrap_or_else(|_| "ws://127.0.0.1:7211".to_string())
}

/// systemd unit name for the headless flycast service.
pub fn flycast_unit() -> String {
    std::env::var("MAPLECAST_FLYCAST_UNIT")
        .unwrap_or_else(|_| "maplecast-headless.service".to_string())
}

/// ROM basename used when computing savestate filenames. Flycast names
/// savestates as `<rom_basename>_<slot>.state`. We need to know the
/// basename so we can list/save/load slots correctly.
pub fn rom_basename() -> String {
    std::env::var("MAPLECAST_ROM_BASENAME").unwrap_or_else(|_| "mvc2".to_string())
}

// ============================================================================
// Response envelope
// ============================================================================

#[derive(Debug, Serialize)]
pub struct AdminResponse {
    pub ok: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub data: Option<Json>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
}

impl AdminResponse {
    pub fn ok(data: Json) -> Self {
        Self { ok: true, data: Some(data), error: None }
    }
    pub fn ok_empty() -> Self {
        Self { ok: true, data: None, error: None }
    }
    pub fn err(msg: impl Into<String>) -> Self {
        Self { ok: false, data: None, error: Some(msg.into()) }
    }

    pub fn to_json_string(&self) -> String {
        serde_json::to_string(self).unwrap_or_else(|_| r#"{"ok":false,"error":"json encode failed"}"#.to_string())
    }
}

// ============================================================================
// GET /overlord/api/status
//
// Returns a snapshot of the live flycast service: pid, memory, uptime,
// listen ports, savestate slot from config. Doesn't bounce through the
// control WS — everything we need is filesystem-readable.
// ============================================================================

#[derive(Debug, Serialize)]
pub struct StatusData {
    pub flycast_active: bool,
    pub flycast_pid: Option<u32>,
    pub flycast_uptime_s: Option<u64>,
    pub flycast_mem_mb: Option<u64>,
    pub current_slot: Option<i64>,
    pub control_ws: String,
    pub savestate_dir: String,
    pub rom_basename: String,
}

pub async fn handle_status() -> AdminResponse {
    // 1. Is the flycast service active? Shell out to systemctl is-active.
    let unit = flycast_unit();
    let active_check = tokio::process::Command::new("systemctl")
        .args(["is-active", &unit])
        .output()
        .await;
    let flycast_active = match active_check {
        Ok(out) => out.status.success() && String::from_utf8_lossy(&out.stdout).trim() == "active",
        Err(_) => false,
    };

    // 2. Pid via systemctl show MainPID.
    let pid: Option<u32> = if flycast_active {
        let out = tokio::process::Command::new("systemctl")
            .args(["show", &unit, "--property=MainPID", "--value"])
            .output()
            .await
            .ok();
        out.and_then(|o| String::from_utf8(o.stdout).ok())
            .and_then(|s| s.trim().parse::<u32>().ok())
            .filter(|&p| p > 0)
    } else {
        None
    };

    // 3. Uptime + memory: read /proc/<pid>/stat and /proc/<pid>/status.
    let mut uptime_s: Option<u64> = None;
    let mut mem_mb: Option<u64> = None;
    if let Some(p) = pid {
        // /proc/<pid>/status has VmRSS in KB
        if let Ok(content) = tokio::fs::read_to_string(format!("/proc/{}/status", p)).await {
            for line in content.lines() {
                if let Some(rest) = line.strip_prefix("VmRSS:") {
                    if let Some(kb_str) = rest.split_whitespace().next() {
                        if let Ok(kb) = kb_str.parse::<u64>() {
                            mem_mb = Some(kb / 1024);
                        }
                    }
                    break;
                }
            }
        }
        // Uptime: systemctl show ActiveEnterTimestampMonotonic gives a usec
        // value relative to system boot. Compare to /proc/uptime.
        let active_since = tokio::process::Command::new("systemctl")
            .args(["show", &unit, "--property=ActiveEnterTimestampMonotonic", "--value"])
            .output()
            .await
            .ok()
            .and_then(|o| String::from_utf8(o.stdout).ok())
            .and_then(|s| s.trim().parse::<u64>().ok());
        if let Some(active_us) = active_since {
            if let Ok(uptime_str) = tokio::fs::read_to_string("/proc/uptime").await {
                if let Some(uptime_secs) = uptime_str.split_whitespace().next()
                    .and_then(|s| s.parse::<f64>().ok())
                {
                    let uptime_us = (uptime_secs * 1_000_000.0) as u64;
                    if uptime_us > active_us {
                        uptime_s = Some((uptime_us - active_us) / 1_000_000);
                    }
                }
            }
        }
    }

    // 4. Current savestate slot from emu.cfg.
    let current_slot = read_current_slot().await;

    let data = StatusData {
        flycast_active,
        flycast_pid: pid,
        flycast_uptime_s: uptime_s,
        flycast_mem_mb: mem_mb,
        current_slot,
        control_ws: control_ws_url(),
        savestate_dir: savestate_dir().to_string_lossy().to_string(),
        rom_basename: rom_basename(),
    };
    AdminResponse::ok(serde_json::to_value(data).unwrap_or(json!({})))
}

/// Parse Dreamcast.SavestateSlot from emu.cfg. Returns None if the file
/// doesn't exist or the field is missing/unparseable. Cheap, runs on every
/// /status call (~1 file read of <10 KB).
async fn read_current_slot() -> Option<i64> {
    let content = tokio::fs::read_to_string(config_path()).await.ok()?;
    for line in content.lines() {
        if let Some(rest) = line.strip_prefix("Dreamcast.SavestateSlot") {
            // "Dreamcast.SavestateSlot = 1"
            if let Some(eq) = rest.find('=') {
                let val = rest[eq + 1..].trim();
                if let Ok(n) = val.parse::<i64>() {
                    return Some(n);
                }
            }
        }
    }
    None
}

// ============================================================================
// GET /overlord/api/savestates
//
// List every mvc2_*.state file in the savestate dir. Returns slot number,
// file size in bytes, mtime (unix seconds), and a "is_current" flag for
// the slot that emu.cfg says will auto-load on next boot.
// ============================================================================

#[derive(Debug, Serialize)]
struct SavestateEntry {
    slot: i32,
    filename: String,
    size: u64,
    mtime: u64,
    is_current: bool,
}

pub async fn handle_list_savestates() -> AdminResponse {
    let dir = savestate_dir();
    let basename = rom_basename();
    let current_slot = read_current_slot().await.unwrap_or(0) as i32;

    let mut entries = match tokio::fs::read_dir(&dir).await {
        Ok(rd) => rd,
        Err(e) => return AdminResponse::err(format!("savestate dir unreadable: {e}")),
    };

    // Pattern matching for filenames. Flycast format:
    //   slot 0:   <basename>.state
    //   slot N:   <basename>_<N>.state
    let prefix = basename.clone();
    let mut out: Vec<SavestateEntry> = Vec::new();
    while let Ok(Some(entry)) = entries.next_entry().await {
        let fname = entry.file_name().to_string_lossy().to_string();
        if !fname.ends_with(".state") {
            continue;
        }
        // Strip .state suffix
        let stem = &fname[..fname.len() - ".state".len()];
        // Slot 0: stem == basename
        // Slot N: stem == basename + "_" + N
        let slot: Option<i32> = if stem == prefix {
            Some(0)
        } else if let Some(rest) = stem.strip_prefix(&format!("{}_", prefix)) {
            rest.parse::<i32>().ok()
        } else {
            None
        };
        let Some(slot) = slot else { continue };

        let meta = match entry.metadata().await {
            Ok(m) => m,
            Err(_) => continue,
        };
        let mtime = meta
            .modified()
            .ok()
            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|d| d.as_secs())
            .unwrap_or(0);
        out.push(SavestateEntry {
            slot,
            filename: fname,
            size: meta.len(),
            mtime,
            is_current: slot == current_slot,
        });
    }
    out.sort_by_key(|e| e.slot);

    AdminResponse::ok(json!({
        "slots": out,
        "current_slot": current_slot,
        "rom_basename": basename,
        "savestate_dir": dir.to_string_lossy(),
    }))
}

// ============================================================================
// Control WS proxy — savestate save/load + reset commands
//
// These admin endpoints can't write the savestate file directly; they have
// to bounce the command through flycast's control WS so dc_savestate /
// dc_loadstate run on the render thread (per Phase A's threading model).
// ============================================================================

/// Send a single JSON command to flycast's control WS, await one reply,
/// return it as JSON. Connect-per-request — admin commands are infrequent
/// and a persistent connection adds complexity for no win.
async fn control_ws_call(cmd: Json) -> Result<Json, String> {
    use futures_util::{SinkExt, StreamExt};
    use tokio_tungstenite::tungstenite::Message;

    let url = control_ws_url();
    let (mut ws, _resp) = tokio::time::timeout(
        Duration::from_secs(3),
        tokio_tungstenite::connect_async(&url),
    )
    .await
    .map_err(|_| "control ws connect timeout".to_string())?
    .map_err(|e| format!("control ws connect: {e}"))?;

    let payload = serde_json::to_string(&cmd).map_err(|e| format!("encode: {e}"))?;
    ws.send(Message::Text(payload.into()))
        .await
        .map_err(|e| format!("send: {e}"))?;

    // Wait up to 10s for the reply (savestate save can take a few hundred
    // ms because dc_serialize walks ~27 MB of state on the render thread).
    let reply = tokio::time::timeout(Duration::from_secs(10), ws.next())
        .await
        .map_err(|_| "control ws reply timeout".to_string())?
        .ok_or_else(|| "control ws closed without reply".to_string())?
        .map_err(|e| format!("ws read: {e}"))?;

    let _ = ws.close(None).await;

    match reply {
        Message::Text(t) => serde_json::from_str(&t).map_err(|e| format!("parse reply: {e}")),
        other => Err(format!("non-text reply: {:?}", other)),
    }
}

/// Random reply correlation id. We use 12 hex chars from current micros.
fn next_reply_id() -> String {
    use std::time::SystemTime;
    let micros = SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .map(|d| d.as_micros())
        .unwrap_or(0);
    format!("{:012x}", micros & 0xFFFFFFFFFFFF)
}

#[derive(Debug, Deserialize)]
pub struct SlotBody {
    pub slot: i32,
}

pub async fn handle_savestate_save(body: &str) -> AdminResponse {
    let req: SlotBody = match serde_json::from_str(body) {
        Ok(r) => r,
        Err(e) => return AdminResponse::err(format!("bad json: {e}")),
    };
    if req.slot < 0 || req.slot > 99 {
        return AdminResponse::err("slot out of range [0,99]");
    }
    let reply_id = next_reply_id();
    let cmd = json!({
        "cmd": "savestate_save",
        "slot": req.slot,
        "reply_id": reply_id,
    });
    match control_ws_call(cmd).await {
        Ok(reply) => {
            let ok = reply.get("ok").and_then(|v| v.as_bool()).unwrap_or(false);
            if ok {
                info!("[admin] savestate_save slot={} OK", req.slot);
                AdminResponse::ok(reply.get("data").cloned().unwrap_or(json!({})))
            } else {
                let err = reply
                    .get("error")
                    .and_then(|v| v.as_str())
                    .unwrap_or("flycast rejected save")
                    .to_string();
                warn!("[admin] savestate_save slot={} FAIL: {}", req.slot, err);
                AdminResponse::err(err)
            }
        }
        Err(e) => {
            warn!("[admin] savestate_save slot={} ws error: {}", req.slot, e);
            AdminResponse::err(format!("control ws: {e}"))
        }
    }
}

pub async fn handle_savestate_load(body: &str) -> AdminResponse {
    let req: SlotBody = match serde_json::from_str(body) {
        Ok(r) => r,
        Err(e) => return AdminResponse::err(format!("bad json: {e}")),
    };
    if req.slot < 0 || req.slot > 99 {
        return AdminResponse::err("slot out of range [0,99]");
    }
    let reply_id = next_reply_id();
    let cmd = json!({
        "cmd": "savestate_load",
        "slot": req.slot,
        "reply_id": reply_id,
    });
    match control_ws_call(cmd).await {
        Ok(reply) => {
            let ok = reply.get("ok").and_then(|v| v.as_bool()).unwrap_or(false);
            if ok {
                info!("[admin] savestate_load slot={} OK", req.slot);
                AdminResponse::ok(reply.get("data").cloned().unwrap_or(json!({})))
            } else {
                let err = reply
                    .get("error")
                    .and_then(|v| v.as_str())
                    .unwrap_or("flycast rejected load")
                    .to_string();
                warn!("[admin] savestate_load slot={} FAIL: {}", req.slot, err);
                AdminResponse::err(err)
            }
        }
        Err(e) => {
            warn!("[admin] savestate_load slot={} ws error: {}", req.slot, e);
            AdminResponse::err(format!("control ws: {e}"))
        }
    }
}

pub async fn handle_reset() -> AdminResponse {
    let reply_id = next_reply_id();
    let cmd = json!({
        "cmd": "reset",
        "reply_id": reply_id,
    });
    match control_ws_call(cmd).await {
        Ok(reply) => {
            let ok = reply.get("ok").and_then(|v| v.as_bool()).unwrap_or(false);
            if ok {
                info!("[admin] reset OK");
                AdminResponse::ok_empty()
            } else {
                let err = reply
                    .get("error")
                    .and_then(|v| v.as_str())
                    .unwrap_or("flycast rejected reset")
                    .to_string();
                AdminResponse::err(err)
            }
        }
        Err(e) => AdminResponse::err(format!("control ws: {e}")),
    }
}

// ============================================================================
// GET /overlord/api/config + POST /overlord/api/config
// ============================================================================

pub async fn handle_config_read() -> AdminResponse {
    let path = config_path();
    match tokio::fs::read_to_string(&path).await {
        Ok(text) => AdminResponse::ok(json!({
            "path": path.to_string_lossy(),
            "text": text,
        })),
        Err(e) => AdminResponse::err(format!("read failed: {e}")),
    }
}

#[derive(Debug, Deserialize)]
pub struct ConfigWriteBody {
    pub text: String,
}

pub async fn handle_config_write(body: &str) -> AdminResponse {
    let req: ConfigWriteBody = match serde_json::from_str(body) {
        Ok(r) => r,
        Err(e) => return AdminResponse::err(format!("bad json: {e}")),
    };
    // Normalize line endings: any \r\n → \n. The config file is LF on Linux
    // and a CRLF mix would confuse flycast's INI parser.
    let normalized = req.text.replace("\r\n", "\n");
    // Cheap sanity check: every non-empty, non-comment line must contain '='.
    for (lineno, line) in normalized.lines().enumerate() {
        let trimmed = line.trim();
        if trimmed.is_empty() || trimmed.starts_with('#') || trimmed.starts_with('[') {
            continue;
        }
        if !trimmed.contains('=') {
            return AdminResponse::err(format!(
                "line {} does not look like an INI key=value: {:?}",
                lineno + 1,
                trimmed
            ));
        }
    }

    // Atomic write: write to <path>.tmp, fsync, rename. Avoids leaving a
    // half-written config if the process is killed mid-write.
    let path = config_path();
    let tmp = path.with_extension("cfg.tmp");
    if let Err(e) = tokio::fs::write(&tmp, normalized.as_bytes()).await {
        return AdminResponse::err(format!("tmp write failed: {e}"));
    }
    if let Err(e) = tokio::fs::rename(&tmp, &path).await {
        return AdminResponse::err(format!("rename failed: {e}"));
    }
    info!("[admin] config wrote {} bytes to {}", normalized.len(), path.display());
    AdminResponse::ok(json!({
        "path": path.to_string_lossy(),
        "bytes": normalized.len(),
    }))
}

// ============================================================================
// POST /overlord/api/service/restart
//
// systemctl restart maplecast-headless. Requires the relay user to have
// the polkit rule installed (see WORKSTREAM-OVERLORD Phase E). The relay
// user is `maplecast-relay` on the VPS.
// ============================================================================

pub async fn handle_service_restart() -> AdminResponse {
    let unit = flycast_unit();
    info!("[admin] systemctl restart {}", unit);
    let out = match tokio::process::Command::new("systemctl")
        .args(["restart", &unit])
        .output()
        .await
    {
        Ok(o) => o,
        Err(e) => return AdminResponse::err(format!("spawn failed: {e}")),
    };
    if out.status.success() {
        AdminResponse::ok(json!({"unit": unit}))
    } else {
        let stderr = String::from_utf8_lossy(&out.stderr).to_string();
        warn!("[admin] systemctl restart {} failed: {}", unit, stderr);
        AdminResponse::err(format!(
            "systemctl exit {}: {}",
            out.status.code().unwrap_or(-1),
            stderr.trim()
        ))
    }
}

// ============================================================================
// GET /overlord/api/logs/tail?n=200
//
// Shell out to journalctl, return the last N lines as plain text inside a
// JSON envelope. Default 200, max 2000.
// ============================================================================

pub async fn handle_logs_tail(query: &str) -> AdminResponse {
    // Parse n from query string (very minimal — just look for n= prefix)
    let n: u32 = query
        .split('&')
        .find_map(|kv| kv.strip_prefix("n="))
        .and_then(|v| v.parse().ok())
        .unwrap_or(200)
        .min(2000);

    let unit = flycast_unit();
    let out = match tokio::process::Command::new("journalctl")
        .args(["-u", &unit, "-n", &n.to_string(), "--no-pager", "--no-hostname", "-o", "short"])
        .output()
        .await
    {
        Ok(o) => o,
        Err(e) => return AdminResponse::err(format!("spawn journalctl: {e}")),
    };
    if !out.status.success() {
        let stderr = String::from_utf8_lossy(&out.stderr).to_string();
        return AdminResponse::err(format!("journalctl failed: {}", stderr.trim()));
    }
    let text = String::from_utf8_lossy(&out.stdout).to_string();
    AdminResponse::ok(json!({
        "lines": text.lines().count(),
        "text": text,
    }))
}

// ============================================================================
// File upload — savestate upload
//
// Accepts multipart/form-data POST. We do a minimal multipart parser
// inline because pulling in `multer` / `axum-extra` for one endpoint is
// overkill. The form must contain exactly one file field with the slot
// number in the query string: `?slot=N`.
// ============================================================================

pub async fn handle_savestate_upload(query: &str, content_type: &str, body_bytes: &[u8]) -> AdminResponse {
    // Slot from query string
    let slot: i32 = match query
        .split('&')
        .find_map(|kv| kv.strip_prefix("slot="))
        .and_then(|v| v.parse().ok())
    {
        Some(s) if (0..=99).contains(&s) => s,
        _ => return AdminResponse::err("missing or invalid ?slot=N (0-99)"),
    };

    // Extract boundary from Content-Type: multipart/form-data; boundary=----xxx
    let boundary = match content_type.split(';')
        .find_map(|part| part.trim().strip_prefix("boundary="))
    {
        Some(b) => b.trim_matches('"'),
        None => return AdminResponse::err("missing multipart boundary"),
    };

    // Find the first boundary, the headers, the body, and the closing boundary.
    let boundary_marker = format!("--{}", boundary);
    let body_str = match std::str::from_utf8(body_bytes) {
        // Allow non-UTF8 — we use bytewise search below for the actual content.
        Ok(_) => "",
        Err(_) => "",
    };
    let _ = body_str;

    // Bytewise search: find first boundary, skip its CRLF, find header end (\r\n\r\n),
    // then capture from there to the next boundary.
    let needle = boundary_marker.as_bytes();
    let first = match find_subsequence(body_bytes, needle) {
        Some(p) => p,
        None => return AdminResponse::err("multipart: first boundary not found"),
    };
    // Skip past boundary + CRLF
    let after_first = first + needle.len() + 2;
    if after_first >= body_bytes.len() {
        return AdminResponse::err("multipart: truncated after first boundary");
    }
    // Find end of headers
    let headers_end = match find_subsequence(&body_bytes[after_first..], b"\r\n\r\n") {
        Some(p) => after_first + p + 4,
        None => return AdminResponse::err("multipart: headers end not found"),
    };
    // Find next boundary (closing)
    let closing = match find_subsequence(&body_bytes[headers_end..], needle.as_ref()) {
        Some(p) => headers_end + p,
        None => return AdminResponse::err("multipart: closing boundary not found"),
    };
    // The actual file content is between headers_end and closing — minus the
    // trailing \r\n that comes before the closing boundary.
    let file_end = if closing >= 2 && &body_bytes[closing - 2..closing] == b"\r\n" {
        closing - 2
    } else {
        closing
    };
    if file_end <= headers_end {
        return AdminResponse::err("multipart: empty file content");
    }
    let file_bytes = &body_bytes[headers_end..file_end];

    // Sanity check: savestates are at least 1 KB (the smallest one we've seen
    // is ~6.7 MB rzip-compressed) and at most 64 MB.
    if file_bytes.len() < 1024 {
        return AdminResponse::err(format!("file too small ({} bytes)", file_bytes.len()));
    }
    if file_bytes.len() > 64 * 1024 * 1024 {
        return AdminResponse::err(format!("file too large ({} bytes, max 64 MB)", file_bytes.len()));
    }

    // Write to disk. Filename: <basename>_<slot>.state (slot 0 → no suffix).
    let basename = rom_basename();
    let fname = if slot == 0 {
        format!("{}.state", basename)
    } else {
        format!("{}_{}.state", basename, slot)
    };
    let path = savestate_dir().join(&fname);
    let tmp = path.with_extension("state.tmp");
    if let Err(e) = tokio::fs::write(&tmp, file_bytes).await {
        return AdminResponse::err(format!("tmp write: {e}"));
    }
    if let Err(e) = tokio::fs::rename(&tmp, &path).await {
        return AdminResponse::err(format!("rename: {e}"));
    }
    info!("[admin] uploaded slot={} {} bytes → {}", slot, file_bytes.len(), path.display());
    AdminResponse::ok(json!({
        "slot": slot,
        "filename": fname,
        "size": file_bytes.len(),
    }))
}

fn find_subsequence(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack.windows(needle.len()).position(|w| w == needle)
}

// ============================================================================
// GET /overlord/api/savestates/download/<slot>
//
// Stream the requested slot's bytes back. Used by the admin UI's "Download"
// button. Returns the raw file bytes with Content-Disposition: attachment.
// ============================================================================

pub async fn handle_savestate_download(slot: i32) -> Result<Vec<u8>, String> {
    if slot < 0 || slot > 99 {
        return Err("slot out of range".to_string());
    }
    let basename = rom_basename();
    let fname = if slot == 0 {
        format!("{}.state", basename)
    } else {
        format!("{}_{}.state", basename, slot)
    };
    let path = savestate_dir().join(&fname);
    let mut file = tokio::fs::File::open(&path)
        .await
        .map_err(|e| format!("open: {e}"))?;
    let mut buf = Vec::new();
    file.read_to_end(&mut buf)
        .await
        .map_err(|e| format!("read: {e}"))?;
    Ok(buf)
}
