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

    /// Cached last MCSV (mid-match savestate) wire frame. The upstream server
    /// only re-broadcasts MCSV every ~60s; without caching, a client that joins
    /// between broadcasts waits up to a minute for one — and the state-replica
    /// reconnect loop drops the socket every ~2s, so it never survives long
    /// enough to receive it. Cache the last one and replay it on connect.
    mcsv_cache: RwLock<Option<Bytes>>,

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
    /// VCACHE content-hash -> page bytes (wire-v2 content-addressed pages).
    /// Fresh per SYNC, mirroring the browser's clear-on-SYNC.
    page_cache: std::collections::HashMap<u64, Vec<u8>>,
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
        // 1024 slots of refcounted Bytes handles (~16KB of pointers). Per-client
        // send tasks never block on the socket anymore (see SendQueue), so
        // receivers drain in microseconds — this is pure headroom. The old
        // 16-slot buffer silently dropped ~0.25s of messages whenever a multi-MB
        // SYNC crossed a slow client's socket, corrupting that client's ZCS2
        // zstd epoch (the "perfect until the sync hits" bug).
        let (frame_tx, _) = broadcast::channel(1024);
        let (signal_tx, _) = broadcast::channel(64);
        let (upstream_text_tx, upstream_text_rx) = tokio::sync::mpsc::channel(256);
        let (upstream_bin_tx, upstream_bin_rx) = tokio::sync::mpsc::channel(256);

        Self {
            inner: Arc::new(RelayInner {
                frame_tx,
                sync_cache: RwLock::new(None),
                mcsv_cache: RwLock::new(None),
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

        // MCSV mid-match savestate — cache it (so joining clients get it on
        // connect, not up to 60s later) and forward it real-time. The MCSV frame
        // is "MCSV"(4) + size(4) + inner ZCST blob; its outer magic is NOT ZCST,
        // so it never matches is_compressed/is_sync and would otherwise be
        // mis-handled as a delta. Detect + short-circuit here.
        if data.len() >= 4 && &data[0..4] == b"MCSV" {
            *self.inner.mcsv_cache.write().await = Some(data.clone());
            let _ = self.inner.frame_tx.send(data);
            return;
        }

        // TX64 ship-once texture packet — RAW (uncompressed) wire. It is a
        // parallel render-channel packet, NOT a dirty-page delta. Forward
        // verbatim and short-circuit BEFORE the apply_dirty_pages path, which
        // would otherwise mis-read frame[76] and corrupt the cached SYNC VRAM.
        if protocol::is_tx64(&data) {
            let len = data.len();
            let receivers = self.inner.frame_tx.receiver_count();
            let _ = self.inner.frame_tx.send(data);
            let mut stats = self.inner.stats.lock().await;
            stats.frames_received += 1;
            stats.bytes_received += len as u64;
            stats.frames_broadcast += receivers as u64;
            stats.bytes_broadcast += (len * receivers) as u64;
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

        // STAF stripped-TA full-frame geometry — ZCST-wrapped, so its decompressed
        // payload starts with "STAF". It is a parallel render channel that carries
        // NO dirty-page list; running apply_dirty_pages on it reads frame[76]
        // (STAF's polyCount, not delta_payload_size) and corrupts the cached SYNC
        // VRAM. Forward the ORIGINAL (compressed) wire bytes verbatim, short-circuit
        // before the SYNC/apply_dirty_pages branching below.
        if protocol::is_staf(inspect) {
            let len = data.len();
            let receivers = self.inner.frame_tx.receiver_count();
            let _ = self.inner.frame_tx.send(data);
            let mut stats = self.inner.stats.lock().await;
            stats.frames_received += 1;
            stats.bytes_received += len as u64;
            stats.frames_broadcast += receivers as u64;
            stats.bytes_broadcast += (len * receivers) as u64;
            return;
        }

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
                *cache = Some(SyncCache { vram, pvr, raw, page_cache: std::collections::HashMap::new() });
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
                    protocol::apply_dirty_pages(inspect, &mut c.vram, &mut c.pvr, &mut c.page_cache);
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

    /// Cached last MCSV wire frame for mid-match joiners, if any.
    pub(crate) async fn get_mcsv(&self) -> Option<Bytes> {
        self.inner.mcsv_cache.read().await.clone()
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
// Per-client send queue — type-aware backpressure (2026-07-09)
//
// The old path sent every broadcast message inline in the client's select!
// loop: while a slow socket drained (e.g. an 8MB SYNC crossing a home
// downlink), the loop couldn't poll frame_rx, the broadcast channel lagged,
// and tokio silently DROPPED messages — including ZCS2 stream chunks, which
// corrupt the whole remaining zstd epoch for that client (the live "perfect
// until the SYNC hits, then background gone + garble" bug).
//
// Now the select! loop only classifies + enqueues (never awaits the socket)
// and a dedicated sender task drains the queue. Drop policy by FrameClass:
//   - Critical (ZCS2/state/audio/unknown): NEVER dropped.
//   - LegacyDelta: over-budget evicts oldest legacy first; the relay then
//     auto-requests a SYNC upstream on the client's behalf (self-healing).
//   - Sync: supersedes — evicts every queued LegacyDelta and older Sync
//     (they describe state the snapshot already contains), then queues.
// Surviving messages keep EXACT arrival order — selective loss, never
// reordering, so the wire's delta-chain invariants hold by construction.
// Every entry is a refcounted Bytes handle — queuing copies nothing.
//
// Single consumer: exactly one sender task pops per queue (close() relies on
// notify_one's stored permit reaching that one waiter).
// ============================================================================

/// Queued legacy-delta budget: ~9s of the full legacy wire (~7 Mbps). Deltas
/// are the ONLY droppable class, so this is the knob that bounds a slow
/// client's queue; recovery is a SYNC away and auto-requested on eviction.
const LEGACY_QUEUE_BUDGET: usize = 8 * 1024 * 1024;
/// Total-queue cutoff: if even the lossless set backs up this far, the client
/// is gone (or so far behind that a reconnect beats delivering the backlog).
const QUEUE_HARD_CAP: usize = 64 * 1024 * 1024;

struct SendQueueInner {
    q: std::collections::VecDeque<(Message, protocol::FrameClass)>,
    bytes: usize,
    legacy_bytes: usize,
    closed: bool,
    // stats
    dropped_legacy: u64,
    superseded: u64,
    peak_bytes: usize,
}

struct SendQueue {
    // std Mutex, not tokio: critical sections are a few pointer ops and never
    // await; uncontended lock/unlock is ~20ns vs a possible task switch.
    inner: std::sync::Mutex<SendQueueInner>,
    notify: tokio::sync::Notify,
}

#[derive(Debug)]
enum Enqueued {
    Ok,
    /// Queued, but legacy deltas were evicted to make room — the client's
    /// page state is now stale and needs a SYNC.
    OkForcedDrop,
    /// Queue is closed (sender died or hard cap tripped) — stop the client.
    Closed,
}

fn msg_wire_len(m: &Message) -> usize {
    match m {
        Message::Binary(b) => b.len(),
        Message::Text(t) => t.len(),
        Message::Ping(b) | Message::Pong(b) => b.len(),
        _ => 0,
    }
}

impl SendQueue {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            inner: std::sync::Mutex::new(SendQueueInner {
                q: std::collections::VecDeque::with_capacity(64),
                bytes: 0,
                legacy_bytes: 0,
                closed: false,
                dropped_legacy: 0,
                superseded: 0,
                peak_bytes: 0,
            }),
            notify: tokio::sync::Notify::new(),
        })
    }

    fn push(&self, msg: Message, class: protocol::FrameClass) -> Enqueued {
        let len = msg_wire_len(&msg);
        let mut g = self.inner.lock().unwrap();
        if g.closed {
            return Enqueued::Closed;
        }
        let mut forced = false;
        match class {
            protocol::FrameClass::Sync => {
                // Snapshot supersedes queued legacy deltas + older snapshots:
                // they describe state this SYNC already contains. This also
                // instantly deflates a backlogged queue right when recovery
                // starts, instead of making the client chew stale deltas first.
                let mut freed = 0usize;
                let mut freed_legacy = 0usize;
                let mut n = 0u64;
                g.q.retain(|(m, c)| match c {
                    protocol::FrameClass::LegacyDelta | protocol::FrameClass::Sync => {
                        let l = msg_wire_len(m);
                        freed += l;
                        if *c == protocol::FrameClass::LegacyDelta {
                            freed_legacy += l;
                        }
                        n += 1;
                        false
                    }
                    protocol::FrameClass::Critical => true,
                });
                g.bytes -= freed;
                g.legacy_bytes -= freed_legacy;
                g.superseded += n;
            }
            protocol::FrameClass::LegacyDelta => {
                // Over budget: evict oldest queued legacy first. Critical
                // entries are never touched.
                while g.legacy_bytes + len > LEGACY_QUEUE_BUDGET {
                    let Some(idx) = g
                        .q
                        .iter()
                        .position(|(_, c)| *c == protocol::FrameClass::LegacyDelta)
                    else {
                        break;
                    };
                    let (m, _) = g.q.remove(idx).expect("position() just found it");
                    let l = msg_wire_len(&m);
                    g.bytes -= l;
                    g.legacy_bytes -= l;
                    g.dropped_legacy += 1;
                    forced = true;
                }
            }
            protocol::FrameClass::Critical => {}
        }
        g.q.push_back((msg, class));
        g.bytes += len;
        if class == protocol::FrameClass::LegacyDelta {
            g.legacy_bytes += len;
        }
        if g.bytes > g.peak_bytes {
            g.peak_bytes = g.bytes;
        }
        if g.bytes > QUEUE_HARD_CAP {
            g.closed = true;
            drop(g);
            self.notify.notify_one();
            return Enqueued::Closed;
        }
        drop(g);
        self.notify.notify_one();
        if forced {
            Enqueued::OkForcedDrop
        } else {
            Enqueued::Ok
        }
    }

    /// Pop the next message in order; awaits when empty; None once closed.
    async fn pop(&self) -> Option<Message> {
        loop {
            let notified = self.notify.notified();
            {
                let mut g = self.inner.lock().unwrap();
                if g.closed {
                    return None;
                }
                if let Some((m, c)) = g.q.pop_front() {
                    let l = msg_wire_len(&m);
                    g.bytes -= l;
                    if c == protocol::FrameClass::LegacyDelta {
                        g.legacy_bytes -= l;
                    }
                    return Some(m);
                }
            }
            notified.await;
        }
    }

    fn close(&self) {
        self.inner.lock().unwrap().closed = true;
        // notify_one stores a permit if the (single) consumer isn't parked yet,
        // so the close can't be lost to the register-then-await race.
        self.notify.notify_one();
    }

    fn stats(&self) -> (u64, u64, usize) {
        let g = self.inner.lock().unwrap();
        (g.dropped_legacy, g.superseded, g.peak_bytes)
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

    // Step 1: Send cached SYNC if available (direct, pre-loop — the socket is
    // otherwise idle here; refcounted Bytes handle, zero copies).
    if let Some(sync_data) = state.get_sync().await {
        info!("Sending cached SYNC to {} ({:.1}MB)", peer, sync_data.len() as f64 / 1024.0 / 1024.0);
        ws_tx.send(Message::Binary(sync_data)).await?;
    } else {
        info!("No SYNC cached yet — {} will wait for first SYNC", peer);
    }

    // Step 1b: Send cached MCSV (mid-match savestate) if a match is live, so a
    // state-replica client joining mid-match applies it immediately instead of
    // waiting for the next ~60s broadcast (and reconnect-looping in the gap).
    if let Some(mcsv_data) = state.get_mcsv().await {
        info!("Sending cached MCSV to {} ({:.1}MB)", peer, mcsv_data.len() as f64 / 1024.0 / 1024.0);
        ws_tx.send(Message::Binary(mcsv_data)).await?;
    }

    // Step 2: Subscribe to frame broadcast
    let mut frame_rx = state.subscribe_frames();
    let mut signal_rx = state.subscribe_signals();

    // Step 3: per-client send queue + dedicated sender task (see the SendQueue
    // block comment). The select! loop below NEVER awaits the socket, so a slow
    // drain (multi-MB SYNC on a home downlink) can no longer lag frame_rx into
    // silent broadcast drops that corrupt the client's ZCS2 zstd epoch.
    let queue = SendQueue::new();
    let sender = {
        let q = queue.clone();
        tokio::spawn(async move {
            let mut sent: u64 = 0;
            let mut bytes: u64 = 0;
            while let Some(msg) = q.pop().await {
                let n = msg_wire_len(&msg) as u64;
                if ws_tx.send(msg).await.is_err() {
                    break;
                }
                sent += 1;
                bytes += n;
            }
            q.close(); // send error (or main-loop close): stop accepting enqueues
            (sent, bytes)
        })
    };

    // Stats for this client
    let mut frames_filtered: u64 = 0;
    let mut syncs_skipped: u64 = 0;
    let mut lagged_at_source: u64 = 0;
    let mut auto_resyncs: u64 = 0;

    // Subscription mode for this connection. Default Full = today's behavior
    // (every frame forwarded). A state-replica client flips this to true by
    // sending {"type":"subscribe","mode":"state"} once its local SH4 takes over
    // rendering (post-MCSV) — from then on the relay forwards only GSTA/OBJF/MCSV
    // and drops all TA/VRAM/SYNC/audio video for this socket. Lock-free: this
    // task owns both the subscribe-message arm and the forward arm.
    let mut state_only = false;
    // ZCS2 subscribers (wire-v2): the client renders from the ZCS2 streaming
    // envelope, so legacy ZCST DELTA frames are dead weight (~5 Mbps). Shed them
    // at classification time (never queued); SYNCs and ZCS2/CAMM/audio/side
    // channels still flow. The client flips back to "full" on decode desync so
    // the legacy fallback keeps working.
    let mut zcs2_only = false;
    // Targeted SYNC delivery: a wire-v2 client announces itself by sending
    // {"type":"request_sync"} (the page does so on connect). From then on it
    // receives ONLY snapshots it asked for — a multi-MB SYNC broadcast triggered
    // by ANOTHER client's join no longer stalls this client's socket (that stall
    // was the queue backup that dropped its ZCS2 messages). Legacy clients that
    // never send request_sync receive every SYNC, exactly as before.
    let mut sync_capable = false;
    let mut wants_sync = false;
    let mut last_auto_resync = std::time::Instant::now() - std::time::Duration::from_secs(60);

    loop {
        tokio::select! {
            // Classify + enqueue TA frames for this client (never blocks)
            frame = frame_rx.recv() => {
                match frame {
                    Ok(data) => {
                        // State-only subscribers receive ONLY the state keep-list.
                        // Drop video frames before they touch the queue.
                        if state_only && !protocol::is_state_frame(&data) {
                            frames_filtered += 1;
                            continue;
                        }
                        let class = protocol::classify(&data);
                        if zcs2_only && class == protocol::FrameClass::LegacyDelta {
                            frames_filtered += 1;
                            continue;
                        }
                        if class == protocol::FrameClass::Sync {
                            if sync_capable && !wants_sync {
                                syncs_skipped += 1;
                                continue;
                            }
                            wants_sync = false;
                        }
                        match queue.push(Message::Binary(data), class) {
                            Enqueued::Ok => {}
                            Enqueued::OkForcedDrop => {
                                // Legacy deltas were evicted for this client — its page
                                // state is now stale. Self-heal: request a SYNC upstream
                                // on its behalf (rate-limited here AND server-side).
                                if last_auto_resync.elapsed() >= std::time::Duration::from_secs(2) {
                                    last_auto_resync = std::time::Instant::now();
                                    wants_sync = true;
                                    auto_resyncs += 1;
                                    state.forward_text_to_upstream("{\"type\":\"request_sync\"}");
                                    info!("Client {} backpressure: legacy deltas dropped — auto request_sync", peer);
                                }
                            }
                            Enqueued::Closed => {
                                warn!("Client {} send queue hard-capped — disconnecting", peer);
                                break;
                            }
                        }
                    }
                    Err(broadcast::error::RecvError::Lagged(n)) => {
                        // Near-impossible now (enqueue never blocks, channel is 1024
                        // deep), but if it fires we lost messages at the SOURCE. A
                        // ZCS2 client detects that deterministically via the bit7 seq
                        // gap and resyncs itself; still self-heal the page state here.
                        lagged_at_source += n;
                        warn!("Client {} lagged {} frames at source (total: {})", peer, n, lagged_at_source);
                        if last_auto_resync.elapsed() >= std::time::Duration::from_secs(2) {
                            last_auto_resync = std::time::Instant::now();
                            wants_sync = true;
                            auto_resyncs += 1;
                            state.forward_text_to_upstream("{\"type\":\"request_sync\"}");
                        }
                    }
                    Err(broadcast::error::RecvError::Closed) => break,
                }
            }

            // Forward signaling messages (through the queue: text stays ordered
            // with the frames instead of racing them on the socket)
            signal = signal_rx.recv() => {
                match signal {
                    Ok(msg) => {
                        if matches!(queue.push(Message::Text(msg.into()), protocol::FrameClass::Critical), Enqueued::Closed) {
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
                            // string the client sent us. Rides the queue so the
                            // measured RTT honestly includes head-of-line latency.
                            if let Some(t_start) = text.find("\"t\":") {
                                let t_str = &text[t_start + 4..];
                                let t_end = t_str.find(|c: char| c == '}' || c == ',').unwrap_or(t_str.len());
                                let t_val = &t_str[..t_end];
                                let pong = format!("{{\"type\":\"pong\",\"t\":{}}}", t_val);
                                if matches!(queue.push(Message::Text(pong.into()), protocol::FrameClass::Critical), Enqueued::Closed) {
                                    break;
                                }
                            }
                            continue;
                        }
                        // request_sync marks this client wire-v2 sync-capable: it
                        // gets the snapshot it just asked for, and from now on ONLY
                        // snapshots it asks for (targeted SYNC delivery). Still
                        // forwarded upstream below, as before.
                        if text.contains("\"type\":\"request_sync\"") {
                            sync_capable = true;
                            wants_sync = true;
                        }
                        // Subscription control: a state-replica client switching
                        // to SH4-rendered mode sends {"type":"subscribe","mode":"state"}
                        // to shed the TA/VRAM firehose. Substring match keeps this
                        // off the serde path, matching the ping/join fast checks above.
                        // This message is relay-local — never forwarded upstream.
                        if text.contains("\"type\":\"subscribe\"") {
                            let want_state = text.contains("\"mode\":\"state\"");
                            let want_zcs2  = text.contains("\"mode\":\"zcs2\"");
                            if want_state != state_only {
                                state_only = want_state;
                                info!("Client {} → subscribe mode: {}", peer,
                                    if state_only { "state-only (GSTA/OBJF/MCSV)" } else { "full mirror" });
                            }
                            if want_zcs2 != zcs2_only {
                                zcs2_only = want_zcs2;
                                info!("Client {} → zcs2 mode: {} (legacy deltas {})", peer,
                                    zcs2_only, if zcs2_only { "SHED" } else { "restored" });
                            }
                            continue;
                        }
                        // Do NOT forward join/leave — those go via direct /play connection.
                        // Forwarding them here causes slot conflicts because flycast maps
                        // the join to the relay's upstream hdl, not the browser's direct hdl.
                        if text.contains("\"type\":\"join\"") || text.contains("\"type\":\"leave\"") {
                            debug!("Client {} join/leave NOT forwarded (use /play direct)", peer);
                            continue;
                        }
                        debug!("Client {} → upstream: {}", peer, &text[..text.len().min(80)]);
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
                        let _ = queue.push(Message::Pong(data), protocol::FrameClass::Critical);
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

    queue.close();
    let (frames_sent, bytes_sent) = sender.await.unwrap_or((0, 0));
    let (dropped_legacy, superseded, peak_queue) = queue.stats();
    if frames_sent > 0 || dropped_legacy > 0 {
        info!(
            "Client {} final stats: sent={} bytes={:.1}MB filtered={} dropped_legacy={} superseded={} syncs_skipped={} lagged_src={} auto_resyncs={} peak_queue={:.1}MB",
            peer, frames_sent, bytes_sent as f64 / 1024.0 / 1024.0, frames_filtered,
            dropped_legacy, superseded, syncs_skipped, lagged_at_source, auto_resyncs,
            peak_queue as f64 / 1024.0 / 1024.0
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
                                    // tungstenite 0.26 hands us Bytes already — forward
                                    // the refcounted handle straight into the broadcast,
                                    // zero copies end to end.
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

// ============================================================================
// SendQueue policy tests — the drop rules are load-bearing for wire integrity
// (a wrongly-dropped ZCS2 chunk corrupts a whole client epoch), so they are
// pinned here as executable spec.
// ============================================================================
#[cfg(test)]
mod send_queue_tests {
    use super::*;
    use protocol::FrameClass;

    fn bin(n: usize, fill: u8) -> Message {
        Message::Binary(vec![fill; n].into())
    }

    fn first_byte(m: Message) -> u8 {
        match m {
            Message::Binary(b) => b[0],
            _ => panic!("expected binary"),
        }
    }

    #[tokio::test]
    async fn critical_never_dropped_and_order_preserved() {
        let q = SendQueue::new();
        // 50MB of critical — way over the legacy budget, under the hard cap.
        for i in 0..50u8 {
            assert!(matches!(q.push(bin(1_000_000, i), FrameClass::Critical), Enqueued::Ok));
        }
        for i in 0..50u8 {
            assert_eq!(first_byte(q.pop().await.unwrap()), i);
        }
        let (dropped, superseded, _) = q.stats();
        assert_eq!((dropped, superseded), (0, 0));
    }

    #[tokio::test]
    async fn legacy_evicts_oldest_first_and_reports_forced_drop() {
        let q = SendQueue::new();
        q.push(bin(100, 0xCC), FrameClass::Critical);
        let n = (LEGACY_QUEUE_BUDGET / 1_000_000 + 4) as u8;
        let mut forced = false;
        for i in 0..n {
            match q.push(bin(1_000_000, i), FrameClass::LegacyDelta) {
                Enqueued::OkForcedDrop => forced = true,
                Enqueued::Ok => {}
                Enqueued::Closed => panic!("hard cap must not trip here"),
            }
        }
        assert!(forced);
        let (dropped, _, _) = q.stats();
        assert!(dropped >= 1);
        // Critical survives untouched, at its original (front) position…
        assert_eq!(first_byte(q.pop().await.unwrap()), 0xCC);
        // …and the surviving legacy run starts exactly at the eviction count
        // (oldest-first) and stays in order.
        for i in (dropped as u8)..n {
            assert_eq!(first_byte(q.pop().await.unwrap()), i);
        }
    }

    #[tokio::test]
    async fn sync_supersedes_queued_legacy_and_older_syncs() {
        let q = SendQueue::new();
        q.push(bin(1_000, 1), FrameClass::LegacyDelta);
        q.push(bin(1_000, 2), FrameClass::Critical);
        q.push(bin(5_000, 3), FrameClass::Sync); // supersedes 1
        q.push(bin(1_000, 4), FrameClass::LegacyDelta);
        q.push(bin(5_000, 5), FrameClass::Sync); // supersedes 3 + 4
        q.push(bin(1_000, 6), FrameClass::LegacyDelta);
        let (_, superseded, _) = q.stats();
        assert_eq!(superseded, 3);
        assert_eq!(first_byte(q.pop().await.unwrap()), 2);
        assert_eq!(first_byte(q.pop().await.unwrap()), 5);
        assert_eq!(first_byte(q.pop().await.unwrap()), 6);
    }

    #[tokio::test]
    async fn hard_cap_closes_queue() {
        let q = SendQueue::new();
        let mut closed = false;
        for _ in 0..(QUEUE_HARD_CAP / 1_000_000 + 8) {
            if matches!(q.push(bin(1_000_000, 0), FrameClass::Critical), Enqueued::Closed) {
                closed = true;
                break;
            }
        }
        assert!(closed);
        // Closed queue rejects everything and pops nothing (drain is pointless
        // for a client we are disconnecting).
        assert!(matches!(q.push(bin(10, 0), FrameClass::Critical), Enqueued::Closed));
        assert!(q.pop().await.is_none());
    }

    #[tokio::test]
    async fn pop_parks_until_push_and_close_wakes() {
        let q = SendQueue::new();
        let q2 = q.clone();
        let popper = tokio::spawn(async move { q2.pop().await.map(first_byte) });
        tokio::time::sleep(tokio::time::Duration::from_millis(20)).await;
        q.push(bin(4, 7), FrameClass::Critical);
        assert_eq!(popper.await.unwrap(), Some(7));

        let q3 = q.clone();
        let popper = tokio::spawn(async move { q3.pop().await });
        tokio::time::sleep(tokio::time::Duration::from_millis(20)).await;
        q.close();
        assert!(popper.await.unwrap().is_none());
    }

    #[tokio::test]
    async fn byte_accounting_survives_mixed_traffic() {
        let q = SendQueue::new();
        for i in 0..200u8 {
            let class = match i % 3 {
                0 => FrameClass::Critical,
                1 => FrameClass::LegacyDelta,
                _ => FrameClass::Sync,
            };
            q.push(bin(10_000 + i as usize, i), class);
        }
        while q.pop().await.is_some() {
            let g = q.inner.lock().unwrap();
            if g.q.is_empty() {
                assert_eq!(g.bytes, 0, "bytes counter must return to zero");
                assert_eq!(g.legacy_bytes, 0, "legacy counter must return to zero");
                break;
            }
        }
    }
}
