// ============================================================================
// REPLAYS — hub endpoints for .mcrec upload/download/listing
//
// Native clients POST recorded matches here. Anyone can download them
// by UUID. Replays are stored on-disk at REPLAY_DIR (defaults to
// /var/lib/maplecast/replays, overridable via MAPLECAST_HUB_REPLAY_DIR).
//
// Browser playback (via server-side on-demand TA regeneration from the
// .mcrec) is a follow-up — for now, native clients can fetch and play
// each other's replays directly via `flycast --replay-url <id>`.
// ============================================================================

use axum::{
    body::Bytes,
    extract::{Path, State},
    http::{header, StatusCode},
    response::IntoResponse,
    Json,
};
use chrono::{DateTime, Utc};
use serde::Serialize;
use std::path::PathBuf;
use std::sync::Arc;
use tokio::sync::RwLock;
use tracing::{info, warn};
use uuid::Uuid;

// ── Config ────────────────────────────────────────────────────────────

pub fn replay_dir() -> PathBuf {
    std::env::var("MAPLECAST_HUB_REPLAY_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("/var/lib/maplecast/replays"))
}

pub async fn ensure_replay_dir() {
    let dir = replay_dir();
    if let Err(e) = tokio::fs::create_dir_all(&dir).await {
        warn!("Could not create replay dir {:?}: {}", dir, e);
    }
}

// ── .mcrec header parsing (just enough for metadata) ──────────────────

#[derive(Debug, Clone, Serialize)]
pub struct ReplayMetadata {
    pub id: String,              // UUID (filename without .mcrec)
    pub match_id_hex: String,
    pub duration_ms: u64,
    pub p1_name: String,
    pub p2_name: String,
    pub winner: u8,              // 0=p1, 1=p2, 0xFF=unknown
    pub size_bytes: u64,
    pub uploaded_at: DateTime<Utc>,
    pub download_url: String,    // /hub/api/replays/:id (relative)
}

fn parse_mcrec_header(data: &[u8]) -> Option<(String, u64, String, String, u8)> {
    // Header layout mirror of the writer (271 bytes). See replay_writer.h.
    if data.len() < 271 {
        return None;
    }
    if &data[0..8] != b"MCREC\0\0\0" {
        return None;
    }
    let version = u32::from_le_bytes([data[8], data[9], data[10], data[11]]);
    if version != 1 {
        return None;
    }

    // match_id at 16, 16 bytes → hex
    let match_id_hex: String = data[16..32]
        .iter()
        .map(|b| format!("{:02x}", b))
        .collect();

    // duration_us at 56, 8 bytes LE
    let duration_us = u64::from_le_bytes([
        data[56], data[57], data[58], data[59],
        data[60], data[61], data[62], data[63],
    ]);

    // p1_name at 96, 64 bytes null-padded
    let p1_end = data[96..160].iter().position(|&b| b == 0).unwrap_or(64);
    let p1_name = String::from_utf8_lossy(&data[96..96 + p1_end]).into_owned();

    // p2_name at 160, 64 bytes null-padded
    let p2_end = data[160..224].iter().position(|&b| b == 0).unwrap_or(64);
    let p2_name = String::from_utf8_lossy(&data[160..160 + p2_end]).into_owned();

    // winner at 230
    let winner = data[230];

    Some((match_id_hex, duration_us / 1000, p1_name, p2_name, winner))
}

// ── In-memory cache (parsed metadata) ─────────────────────────────────

#[derive(Default)]
pub struct ReplayCache {
    pub list: Vec<ReplayMetadata>,
}

pub type SharedReplayCache = Arc<RwLock<ReplayCache>>;

pub fn new_cache() -> SharedReplayCache {
    Arc::new(RwLock::new(ReplayCache::default()))
}

/// Scan the replay directory on startup + periodically, parse headers,
/// populate the cache. Called once at boot + every 60s.
pub async fn refresh_cache(cache: SharedReplayCache) {
    let dir = replay_dir();
    let mut list = Vec::new();

    let mut entries = match tokio::fs::read_dir(&dir).await {
        Ok(e) => e,
        Err(_) => {
            // Directory doesn't exist yet — empty list is fine
            let mut c = cache.write().await;
            c.list.clear();
            return;
        }
    };

    while let Ok(Some(entry)) = entries.next_entry().await {
        let path = entry.path();
        if path.extension().and_then(|s| s.to_str()) != Some("mcrec") {
            continue;
        }

        let id = path.file_stem().and_then(|s| s.to_str()).unwrap_or("").to_string();
        if id.is_empty() {
            continue;
        }

        // Read just the header (first 271 bytes) — cheap
        let mut hdr = vec![0u8; 271];
        let mut f = match tokio::fs::File::open(&path).await {
            Ok(f) => f,
            Err(_) => continue,
        };
        use tokio::io::AsyncReadExt;
        if f.read_exact(&mut hdr).await.is_err() {
            continue;
        }

        let (match_id, dur_ms, p1, p2, winner) = match parse_mcrec_header(&hdr) {
            Some(v) => v,
            None => continue,
        };

        let metadata = match entry.metadata().await {
            Ok(m) => m,
            Err(_) => continue,
        };
        let size_bytes = metadata.len();
        let uploaded_at: DateTime<Utc> = metadata
            .modified()
            .ok()
            .and_then(|t| DateTime::<Utc>::from(t).into())
            .unwrap_or_else(Utc::now);

        list.push(ReplayMetadata {
            id: id.clone(),
            match_id_hex: match_id,
            duration_ms: dur_ms,
            p1_name: p1,
            p2_name: p2,
            winner,
            size_bytes,
            uploaded_at,
            download_url: format!("/hub/api/replays/{}", id),
        });
    }

    // Newest first
    list.sort_by(|a, b| b.uploaded_at.cmp(&a.uploaded_at));

    let mut c = cache.write().await;
    c.list = list;
    info!("Replay cache refreshed: {} replays", c.list.len());
}

pub async fn cache_refresher(cache: SharedReplayCache) {
    let mut interval = tokio::time::interval(std::time::Duration::from_secs(60));
    loop {
        interval.tick().await;
        refresh_cache(cache.clone()).await;
    }
}

// ── HTTP handlers ─────────────────────────────────────────────────────

/// POST /hub/api/replays — body is raw .mcrec bytes.
/// Validates the MCREC magic, generates a UUID, saves to replay_dir().
/// Returns `{"id": "uuid", "download_url": "..."}`.
pub async fn upload_replay(
    State(cache): State<SharedReplayCache>,
    body: Bytes,
) -> impl IntoResponse {
    if body.len() < 271 {
        return (
            StatusCode::BAD_REQUEST,
            Json(serde_json::json!({"ok": false, "error": "too short to be a .mcrec"})),
        );
    }
    if &body[0..8] != b"MCREC\0\0\0" {
        return (
            StatusCode::BAD_REQUEST,
            Json(serde_json::json!({"ok": false, "error": "bad magic — not a .mcrec file"})),
        );
    }

    // Generate a fresh UUID for storage (match_id from header may be
    // non-unique across attempts)
    let id = Uuid::new_v4().to_string();
    let dir = replay_dir();
    if let Err(e) = tokio::fs::create_dir_all(&dir).await {
        warn!("create_dir_all {:?} failed: {}", dir, e);
    }
    let path = dir.join(format!("{}.mcrec", id));

    if let Err(e) = tokio::fs::write(&path, &body).await {
        warn!("replay write {:?} failed: {}", path, e);
        return (
            StatusCode::INTERNAL_SERVER_ERROR,
            Json(serde_json::json!({"ok": false, "error": "write failed"})),
        );
    }

    info!("Replay uploaded: {} ({} bytes)", id, body.len());

    // Refresh the cache in the background so the new replay shows up in listings
    let cache2 = cache.clone();
    tokio::spawn(async move { refresh_cache(cache2).await; });

    (
        StatusCode::OK,
        Json(serde_json::json!({
            "ok": true,
            "id": id,
            "download_url": format!("/hub/api/replays/{}", id),
            "size_bytes": body.len(),
        })),
    )
}

/// GET /hub/api/replays — list all stored replays with metadata.
pub async fn list_replays(
    State(cache): State<SharedReplayCache>,
) -> impl IntoResponse {
    let c = cache.read().await;
    Json(serde_json::json!({ "replays": c.list }))
}

/// GET /hub/api/replays/:id/info — metadata for a single replay.
pub async fn replay_info(
    State(cache): State<SharedReplayCache>,
    Path(id): Path<String>,
) -> impl IntoResponse {
    let c = cache.read().await;
    if let Some(m) = c.list.iter().find(|m| m.id == id) {
        (StatusCode::OK, Json(serde_json::json!(m))).into_response()
    } else {
        (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({"ok": false, "error": "not found"})),
        )
            .into_response()
    }
}

/// GET /hub/api/replays/:id — stream the raw .mcrec file bytes.
pub async fn download_replay(
    State(_cache): State<SharedReplayCache>,
    Path(id): Path<String>,
) -> impl IntoResponse {
    // Sanitize: only hex + dashes (UUID format)
    if !id.chars().all(|c| c.is_ascii_hexdigit() || c == '-') || id.len() > 40 {
        return (StatusCode::BAD_REQUEST, "invalid id").into_response();
    }

    let path = replay_dir().join(format!("{}.mcrec", id));
    match tokio::fs::read(&path).await {
        Ok(bytes) => {
            let headers = [
                (header::CONTENT_TYPE, "application/octet-stream"),
                (header::CONTENT_DISPOSITION, "attachment"),
            ];
            (StatusCode::OK, headers, bytes).into_response()
        }
        Err(_) => (StatusCode::NOT_FOUND, "not found").into_response(),
    }
}
