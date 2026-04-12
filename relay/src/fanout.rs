// ============================================================================
// FANOUT — Core relay engine
//
// Two modes:
// 1. Raw TCP upstream from flycast → broadcast to WebSocket clients
// 2. Future: splice() zero-copy for raw TCP clients
//
// The relay maintains:
// - Cached SYNC state (VRAM + PVR), updated incrementally from dirty pages
// - Latest frame for late joiners
// - Client list with backpressure tracking
//
// Frame flow:
//   flycast → [raw TCP, 4-byte length prefix] → relay
//   relay  → [WebSocket binary] → all connected browsers
//
// SYNC flow:
//   On new client connect → send cached SYNC as WebSocket binary
//   Then stream delta frames
// ============================================================================

use crate::protocol;
use bytes::Bytes;
use std::net::SocketAddr;
use std::sync::Arc;
use tokio::io::AsyncReadExt;
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{broadcast, RwLock, Mutex};
use tokio_tungstenite::tungstenite::Message;
use futures_util::{SinkExt, StreamExt};
use tracing::{info, warn, error, debug};

// ============================================================================
// Relay State — shared across all tasks
// ============================================================================

#[derive(Clone)]
pub struct RelayState {
    inner: Arc<RelayInner>,
}

struct RelayInner {
    /// Broadcast channel for delta frames (binary, opaque bytes)
    frame_tx: broadcast::Sender<Bytes>,

    /// Cached SYNC state — rebuilt from initial SYNC + incremental dirty pages
    sync_cache: RwLock<Option<SyncCache>>,

    /// Connected client count
    client_count: Mutex<usize>,
    max_clients: usize,

    /// Stats
    stats: Mutex<RelayStats>,

    /// JSON signaling broadcast (for relay_* messages)
    signal_tx: broadcast::Sender<String>,

    /// Client → upstream forwarding (text messages: join, queue, register_stick, chat, etc.)
    upstream_text_tx: tokio::sync::mpsc::Sender<String>,
    upstream_text_rx: Mutex<Option<tokio::sync::mpsc::Receiver<String>>>,

    /// Client → upstream forwarding (binary messages: gamepad input)
    upstream_bin_tx: tokio::sync::mpsc::Sender<Vec<u8>>,
    upstream_bin_rx: Mutex<Option<tokio::sync::mpsc::Receiver<Vec<u8>>>>,
}

struct SyncCache {
    vram: Vec<u8>,
    pvr: Vec<u8>,
    /// Pre-built SYNC binary for fast send to new clients
    raw: Bytes,
}

#[derive(Default)]
struct RelayStats {
    // Video-only counters — audio packets are excluded so FPS / jitter /
    // bytes-per-frame metrics reflect the video stream only.
    frames_received: u64,
    frames_broadcast: u64,
    bytes_received: u64,
    bytes_broadcast: u64,
    sync_count: u32,
    upstream_connected: bool,
    /// Most recent VIDEO frame size (audio not tracked here)
    last_frame_size_bytes: u64,
    /// Track time of last VIDEO frame to compute frame interval jitter
    last_frame_at_us: u64,
    /// Cumulative jitter sum (microseconds, to avoid float perf hit)
    frame_interval_sum_us: u64,
    frame_interval_count: u64,
    /// Max observed VIDEO frame interval (worst-case jitter)
    max_frame_interval_us: u64,

    // Audio counters — tracked separately so we know audio is flowing
    // without polluting the video metrics. The overlord dashboard can
    // optionally surface these, but the primary FPS / jitter cards
    // should read the video-only fields above.
    audio_packets_received: u64,
    audio_packets_broadcast: u64,
    audio_bytes_received: u64,
    audio_bytes_broadcast: u64,
}

/// Snapshot of relay metrics for /metrics endpoint
#[derive(Clone, Default)]
pub struct MetricsSnapshot {
    pub frames_received: u64,
    pub frames_broadcast: u64,
    pub bytes_received: u64,
    pub bytes_broadcast: u64,
    pub sync_count: u32,
    pub upstream_connected: bool,
    pub clients: u64,
    pub last_frame_size_bytes: u64,
    pub avg_frame_interval_us: u64,
    pub max_frame_interval_us: u64,
    pub has_sync_cache: bool,
    pub sync_cache_bytes: u64,
    // Audio telemetry (separate from video counters above)
    pub audio_packets_received: u64,
    pub audio_packets_broadcast: u64,
    pub audio_bytes_received: u64,
    pub audio_bytes_broadcast: u64,
}

impl RelayState {
    pub fn new(max_clients: usize) -> Self {
        // 16 slots — if a client falls behind 16 frames, they drop frames (good)
        let (frame_tx, _) = broadcast::channel(16);
        let (signal_tx, _) = broadcast::channel(64);
        let (upstream_text_tx, upstream_text_rx) = tokio::sync::mpsc::channel(256);
        let (upstream_bin_tx, upstream_bin_rx) = tokio::sync::mpsc::channel(256);

        Self {
            inner: Arc::new(RelayInner {
                frame_tx,
                sync_cache: RwLock::new(None),
                client_count: Mutex::new(0),
                max_clients,
                stats: Mutex::new(RelayStats::default()),
                signal_tx,
                upstream_text_tx,
                upstream_text_rx: Mutex::new(Some(upstream_text_rx)),
                upstream_bin_tx,
                upstream_bin_rx: Mutex::new(Some(upstream_bin_rx)),
            }),
        }
    }

    /// Handle incoming frame from upstream flycast server.
    /// Frames may be ZCST-compressed; we decompress to inspect for state cache,
    /// but forward the original (compressed) bytes downstream to save bandwidth.
    async fn on_upstream_frame(&self, data: Bytes) {
        // Audio packets FIRST — fast path, no decompression, no state cache
        // update, no video frame counter bookkeeping. Audio rides the same
        // wire as video so the P2P fan-out tree just works, but we must
        // keep it out of the video-only FPS / jitter / bytes-per-frame
        // metrics so the overlord dashboard reports sensible numbers.
        if protocol::is_audio(&data) {
            let len = data.len();
            let receivers = self.inner.frame_tx.receiver_count();
            let _ = self.inner.frame_tx.send(data);

            let mut stats = self.inner.stats.lock().await;
            stats.audio_packets_received += 1;
            stats.audio_bytes_received += len as u64;
            stats.audio_packets_broadcast += receivers as u64;
            stats.audio_bytes_broadcast += (len * receivers) as u64;
            return;
        }

        // Decompress for inspection if needed (held only as long as we need it)
        let inspect_buf: Option<Vec<u8>> = if protocol::is_compressed(&data) {
            protocol::decompress(&data)
        } else {
            None
        };
        // Inspect view: either the decompressed payload, or the original
        let inspect: &[u8] = inspect_buf.as_deref().unwrap_or(&data);

        if protocol::is_sync(inspect) {
            // SYNC frame — cache the decompressed state
            if let Some((vram, pvr)) = protocol::parse_sync(inspect) {
                info!(
                    "SYNC received: VRAM={:.1}MB PVR={:.1}KB (wire={:.1}MB compressed={})",
                    vram.len() as f64 / 1024.0 / 1024.0,
                    pvr.len() as f64 / 1024.0,
                    data.len() as f64 / 1024.0 / 1024.0,
                    inspect_buf.is_some(),
                );
                // Cache stores the ORIGINAL wire bytes (compressed if it came compressed)
                // so new clients receive the same bandwidth-efficient SYNC
                let raw = data.clone();
                let mut cache = self.inner.sync_cache.write().await;
                *cache = Some(SyncCache { vram, pvr, raw });
            }

            let mut stats = self.inner.stats.lock().await;
            stats.sync_count += 1;

            // Broadcast SYNC to all connected clients via a special channel
            // (we send it as a frame — clients check for SYNC magic)
            let _ = self.inner.frame_tx.send(data);
        } else {
            // Delta frame — update cached SYNC state from the decompressed view
            {
                let mut cache = self.inner.sync_cache.write().await;
                if let Some(ref mut c) = *cache {
                    protocol::apply_dirty_pages(inspect, &mut c.vram, &mut c.pvr);
                    // We don't rebuild c.raw here — late joiners get the cached
                    // SYNC as it was last received from upstream (compressed or not).
                    // The flycast server sends fresh keyframes periodically anyway.
                }
            }

            let frame_num = protocol::frame_num(inspect).unwrap_or(0);
            let len = data.len();

            // Broadcast to all subscribers (forwarding original wire bytes)
            let receivers = self.inner.frame_tx.receiver_count();
            let _ = self.inner.frame_tx.send(data);

            let mut stats = self.inner.stats.lock().await;
            stats.frames_received += 1;
            stats.bytes_received += len as u64;
            stats.frames_broadcast += receivers as u64;
            stats.bytes_broadcast += (len * receivers) as u64;
            stats.last_frame_size_bytes = len as u64;

            // Frame interval jitter — measured between successive frames received
            let now_us = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_micros() as u64)
                .unwrap_or(0);
            if stats.last_frame_at_us > 0 {
                let interval = now_us.saturating_sub(stats.last_frame_at_us);
                stats.frame_interval_sum_us += interval;
                stats.frame_interval_count += 1;
                if interval > stats.max_frame_interval_us {
                    stats.max_frame_interval_us = interval;
                }
            }
            stats.last_frame_at_us = now_us;

            if stats.frames_received % 600 == 0 {
                info!(
                    "📊 frame={} clients={} total_frames={} total_rx={:.1}MB total_tx={:.1}MB syncs={}",
                    frame_num,
                    receivers,
                    stats.frames_received,
                    stats.bytes_received as f64 / 1024.0 / 1024.0,
                    stats.bytes_broadcast as f64 / 1024.0 / 1024.0,
                    stats.sync_count,
                );
            }
        }
    }

    /// Get cached SYNC for new client
    pub(crate) async fn get_sync(&self) -> Option<Bytes> {
        let cache = self.inner.sync_cache.read().await;
        cache.as_ref().map(|c| c.raw.clone())
    }

    /// Snapshot all metrics for /metrics endpoint (called by HTTP listener).
    ///
    /// IMPORTANT: this RESETS the rolling jitter window (avg + max interval)
    /// so each Prometheus scrape sees a fresh window. Without this, a single
    /// startup spike would pin max_frame_interval_us forever and the average
    /// would converge to a meaningless lifetime mean. Counters that should be
    /// monotonic (frames_received, bytes_received, sync_count) are NOT reset.
    pub async fn metrics(&self) -> MetricsSnapshot {
        let mut stats = self.inner.stats.lock().await;
        let clients = *self.inner.client_count.lock().await as u64;
        let cache = self.inner.sync_cache.read().await;
        let (has_sync, sync_bytes) = match cache.as_ref() {
            Some(c) => (true, c.raw.len() as u64),
            None => (false, 0),
        };
        let avg_interval = if stats.frame_interval_count > 0 {
            stats.frame_interval_sum_us / stats.frame_interval_count
        } else {
            0
        };
        let snap = MetricsSnapshot {
            frames_received: stats.frames_received,
            frames_broadcast: stats.frames_broadcast,
            bytes_received: stats.bytes_received,
            bytes_broadcast: stats.bytes_broadcast,
            sync_count: stats.sync_count,
            upstream_connected: stats.upstream_connected,
            clients,
            last_frame_size_bytes: stats.last_frame_size_bytes,
            avg_frame_interval_us: avg_interval,
            max_frame_interval_us: stats.max_frame_interval_us,
            has_sync_cache: has_sync,
            sync_cache_bytes: sync_bytes,
            audio_packets_received: stats.audio_packets_received,
            audio_packets_broadcast: stats.audio_packets_broadcast,
            audio_bytes_received: stats.audio_bytes_received,
            audio_bytes_broadcast: stats.audio_bytes_broadcast,
        };
        // Reset the rolling jitter window so the next scrape covers a fresh
        // window. We deliberately do NOT reset last_frame_at_us — the very
        // next frame should compute its delta against the previous frame, not
        // restart from zero.
        stats.frame_interval_sum_us = 0;
        stats.frame_interval_count = 0;
        stats.max_frame_interval_us = 0;
        snap
    }

    /// Subscribe to frame broadcast
    pub(crate) fn subscribe_frames(&self) -> broadcast::Receiver<Bytes> {
        self.inner.frame_tx.subscribe()
    }

    /// Subscribe to signaling broadcast
    fn subscribe_signals(&self) -> broadcast::Receiver<String> {
        self.inner.signal_tx.subscribe()
    }

    /// Broadcast a signaling message to all clients
    fn broadcast_signal(&self, msg: &str) {
        let _ = self.inner.signal_tx.send(msg.to_string());
    }

    /// Forward a client's text message to the upstream flycast server
    fn forward_text_to_upstream(&self, msg: &str) {
        let _ = self.inner.upstream_text_tx.try_send(msg.to_string());
    }

    /// Forward a client's binary message to the upstream flycast server
    fn forward_bin_to_upstream(&self, data: &[u8]) {
        let _ = self.inner.upstream_bin_tx.try_send(data.to_vec());
    }

    /// Take the upstream receivers (called once by the upstream connector)
    async fn take_upstream_receivers(&self) -> (
        tokio::sync::mpsc::Receiver<String>,
        tokio::sync::mpsc::Receiver<Vec<u8>>,
    ) {
        let text_rx = self.inner.upstream_text_rx.lock().await.take()
            .expect("upstream receivers already taken");
        let bin_rx = self.inner.upstream_bin_rx.lock().await.take()
            .expect("upstream receivers already taken");
        (text_rx, bin_rx)
    }

    pub(crate) async fn add_client(&self) -> bool {
        let mut count = self.inner.client_count.lock().await;
        if *count >= self.inner.max_clients {
            return false;
        }
        *count += 1;
        true
    }

    pub(crate) async fn remove_client(&self) {
        let mut count = self.inner.client_count.lock().await;
        *count = count.saturating_sub(1);
    }

    pub(crate) async fn client_count(&self) -> usize {
        *self.inner.client_count.lock().await
    }
}

// ============================================================================
// TCP Upstream Listener — flycast pushes frames to us
// ============================================================================

pub async fn tcp_upstream_listener(
    addr: SocketAddr,
    state: RelayState,
) -> std::io::Result<()> {
    let listener = TcpListener::bind(addr).await?;
    info!("TCP upstream listener ready on {}", addr);

    loop {
        let (stream, peer) = listener.accept().await?;
        info!("Upstream flycast connected from {}", peer);

        // Disable Nagle — we want frames ASAP
        stream.set_nodelay(true).ok();

        let state = state.clone();
        tokio::spawn(async move {
            if let Err(e) = handle_upstream(stream, state.clone()).await {
                warn!("Upstream connection lost: {}", e);
            }
            {
                let mut stats = state.inner.stats.lock().await;
                stats.upstream_connected = false;
            }
            info!("Upstream disconnected from {}", peer);
        });
    }
}

async fn handle_upstream(mut stream: TcpStream, state: RelayState) -> std::io::Result<()> {
    {
        let mut stats = state.inner.stats.lock().await;
        stats.upstream_connected = true;
    }

    let mut len_buf = [0u8; 4];

    loop {
        // Read 4-byte length prefix
        stream.read_exact(&mut len_buf).await?;
        let frame_len = u32::from_le_bytes(len_buf) as usize;

        if frame_len == 0 || frame_len > 16 * 1024 * 1024 {
            warn!("Invalid frame length: {} — skipping", frame_len);
            continue;
        }

        // Read frame payload
        let mut buf = vec![0u8; frame_len];
        stream.read_exact(&mut buf).await?;

        let data = Bytes::from(buf);
        state.on_upstream_frame(data).await;
    }
}

// ============================================================================
// WebSocket Client Listener — browsers and players connect here
// ============================================================================

pub async fn ws_client_listener(
    addr: SocketAddr,
    state: RelayState,
) -> std::io::Result<()> {
    let listener = TcpListener::bind(addr).await?;
    info!("WebSocket client listener ready on {}", addr);

    loop {
        let (stream, peer) = listener.accept().await?;
        stream.set_nodelay(true).ok();

        let state = state.clone();
        tokio::spawn(async move {
            if !state.add_client().await {
                warn!("Rejected {} — max clients reached", peer);
                return;
            }

            let count = state.client_count().await;
            info!("Client connected: {} (total: {})", peer, count);

            match handle_ws_client(stream, peer, state.clone()).await {
                Ok(_) => debug!("Client {} disconnected cleanly", peer),
                Err(e) => debug!("Client {} error: {}", peer, e),
            }

            state.remove_client().await;
            let count = state.client_count().await;
            info!("Client disconnected: {} (total: {})", peer, count);
        });
    }
}

async fn handle_ws_client(
    stream: TcpStream,
    peer: SocketAddr,
    state: RelayState,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let ws = tokio_tungstenite::accept_async(stream).await?;
    let (mut ws_tx, mut ws_rx) = ws.split();

    // Step 1: Send cached SYNC if available
    if let Some(sync_data) = state.get_sync().await {
        info!("Sending cached SYNC to {} ({:.1}MB)", peer, sync_data.len() as f64 / 1024.0 / 1024.0);
        ws_tx.send(Message::Binary(sync_data.to_vec().into())).await?;
    } else {
        info!("No SYNC cached yet — {} will wait for first SYNC", peer);
    }

    // Step 2: Subscribe to frame broadcast
    let mut frame_rx = state.subscribe_frames();
    let mut signal_rx = state.subscribe_signals();

    // Stats for this client
    let mut frames_sent: u64 = 0;
    let mut frames_dropped: u64 = 0;
    let mut bytes_sent: u64 = 0;

    loop {
        tokio::select! {
            // Forward TA frames to this client
            frame = frame_rx.recv() => {
                match frame {
                    Ok(data) => {
                        let len = data.len();
                        match ws_tx.send(Message::Binary(data.to_vec().into())).await {
                            Ok(_) => {
                                frames_sent += 1;
                                bytes_sent += len as u64;
                            }
                            Err(e) => {
                                debug!("Client {} send error: {}", peer, e);
                                break;
                            }
                        }
                    }
                    Err(broadcast::error::RecvError::Lagged(n)) => {
                        frames_dropped += n;
                        debug!("Client {} lagged {} frames (total dropped: {})", peer, n, frames_dropped);
                    }
                    Err(broadcast::error::RecvError::Closed) => break,
                }
            }

            // Forward signaling messages
            signal = signal_rx.recv() => {
                match signal {
                    Ok(msg) => {
                        if ws_tx.send(Message::Text(msg.into())).await.is_err() {
                            break;
                        }
                    }
                    Err(broadcast::error::RecvError::Lagged(_)) => {}
                    Err(broadcast::error::RecvError::Closed) => break,
                }
            }

            // Handle incoming messages from client — forward to upstream flycast
            msg = ws_rx.next() => {
                match msg {
                    Some(Ok(Message::Text(text))) => {
                        // Fast path: ping echo at the relay — saves a full round-trip
                        // to the home flycast (~10ms on a typical connection).
                        // The ping format is {"type":"ping","t":<f64>}; we echo back
                        // {"type":"pong","t":<same>} immediately without parsing JSON.
                        if text.starts_with("{\"type\":\"ping\"") {
                            // Extract the "t" field verbatim and reply.
                            // Avoids serde_json overhead — the timestamp is whatever
                            // string the client sent us.
                            if let Some(t_start) = text.find("\"t\":") {
                                let t_str = &text[t_start + 4..];
                                let t_end = t_str.find(|c: char| c == '}' || c == ',').unwrap_or(t_str.len());
                                let t_val = &t_str[..t_end];
                                let pong = format!("{{\"type\":\"pong\",\"t\":{}}}", t_val);
                                if ws_tx.send(Message::Text(pong.into())).await.is_err() {
                                    break;
                                }
                            }
                            continue;
                        }
                        debug!("Client {} → upstream: {}", peer, &text[..text.len().min(80)]);
                        // Forward to upstream flycast (join, queue, register_stick, chat, etc.)
                        state.forward_text_to_upstream(&text);
                        // Also broadcast relay_* messages to other clients
                        if text.contains("relay_") {
                            state.broadcast_signal(&text);
                        }
                    }
                    Some(Ok(Message::Binary(data))) => {
                        // Binary from client — gamepad input, forward to upstream
                        state.forward_bin_to_upstream(&data);
                    }
                    Some(Ok(Message::Close(_))) | None => break,
                    Some(Ok(Message::Ping(data))) => {
                        let _ = ws_tx.send(Message::Pong(data)).await;
                    }
                    Some(Err(e)) => {
                        debug!("Client {} ws error: {}", peer, e);
                        break;
                    }
                    _ => {}
                }
            }
        }
    }

    if frames_sent > 0 {
        info!(
            "Client {} final stats: sent={} dropped={} bytes={:.1}MB",
            peer, frames_sent, frames_dropped, bytes_sent as f64 / 1024.0 / 1024.0
        );
    }

    Ok(())
}

// ============================================================================
// WebSocket Upstream — connect to flycast's existing WS on port 7200
// Zero modifications to flycast needed. Relay acts as a WS client.
// ============================================================================

pub async fn ws_upstream_connector(
    url: String,
    state: RelayState,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    // Take the upstream forwarding receivers (one-time)
    let (mut text_rx, mut bin_rx) = state.take_upstream_receivers().await;

    loop {
        info!("Connecting to upstream flycast at {}...", url);

        match tokio_tungstenite::connect_async(&url).await {
            Ok((ws, _response)) => {
                info!("Connected to upstream flycast at {}", url);
                {
                    let mut stats = state.inner.stats.lock().await;
                    stats.upstream_connected = true;
                }

                let (mut ws_tx, mut ws_rx) = ws.split();
                let mut frames: u64 = 0;

                loop {
                    tokio::select! {
                        // Receive from upstream flycast → broadcast to clients
                        msg = ws_rx.next() => {
                            match msg {
                                Some(Ok(Message::Binary(data))) => {
                                    let data = Bytes::from(data.to_vec());
                                    frames += 1;
                                    state.on_upstream_frame(data).await;
                                }
                                Some(Ok(Message::Text(text))) => {
                                    debug!("Upstream JSON: {}...", &text[..text.len().min(100)]);
                                    state.broadcast_signal(&text);
                                }
                                Some(Ok(Message::Close(_))) => {
                                    info!("Upstream closed connection after {} frames", frames);
                                    break;
                                }
                                Some(Ok(Message::Ping(data))) => {
                                    debug!("Upstream ping ({} bytes)", data.len());
                                }
                                Some(Err(e)) => {
                                    error!("Upstream WS error: {}", e);
                                    break;
                                }
                                None => break,
                                _ => {}
                            }
                        }

                        // Forward client text messages → upstream flycast
                        text = text_rx.recv() => {
                            if let Some(text) = text {
                                if let Err(e) = ws_tx.send(Message::Text(text.into())).await {
                                    warn!("Failed to forward text to upstream: {}", e);
                                    break;
                                }
                            }
                        }

                        // Forward client binary messages → upstream flycast
                        bin = bin_rx.recv() => {
                            if let Some(data) = bin {
                                if let Err(e) = ws_tx.send(Message::Binary(data.into())).await {
                                    warn!("Failed to forward binary to upstream: {}", e);
                                    break;
                                }
                            }
                        }
                    }
                }

                {
                    let mut stats = state.inner.stats.lock().await;
                    stats.upstream_connected = false;
                }
                warn!("Lost upstream connection after {} frames — reconnecting in 2s...", frames);
            }
            Err(e) => {
                warn!("Failed to connect to upstream: {} — retrying in 2s...", e);
            }
        }

        tokio::time::sleep(tokio::time::Duration::from_secs(2)).await;
    }
}
