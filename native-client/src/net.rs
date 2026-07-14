//! Network client: direct wss:// to nobd, thin ZCS2 wire -> shared FrameDecoder.
//!
//! Exactly what webgpu-test.html does with "ZCS2" checked: request a SYNC base on
//! connect, then decode the thin ZCS2 per-frame wire, and tell the relay to drop
//! the heavy legacy deltas. rustls speaks TLS, so this reaches nobd directly.

use crate::frame::FrameDecoder;
use crate::zcs2::Zcs2;
use futures_util::{SinkExt, StreamExt};
use std::sync::{Arc, Mutex};
use tokio_tungstenite::tungstenite::Message;

type BoxErr = Box<dyn std::error::Error + Send + Sync>;

pub fn spawn_net_thread(
    url: String,
    shared: Arc<Mutex<FrameDecoder>>,
    debug: Arc<crate::debug::DebugState>,
) {
    std::thread::Builder::new()
        .name("maplecast-net".into())
        .spawn(move || {
            let rt = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .expect("tokio runtime");
            rt.block_on(async move {
                loop {
                    if let Err(e) = run(&url, &shared, &debug).await {
                        log::warn!("[net] {e} — reconnecting");
                    }
                    tokio::time::sleep(std::time::Duration::from_secs(1)).await;
                }
            });
        })
        .expect("spawn net thread");
}

async fn run(
    url: &str,
    shared: &Arc<Mutex<FrameDecoder>>,
    debug: &crate::debug::DebugState,
) -> Result<(), BoxErr> {
    use std::sync::atomic::Ordering::Relaxed;
    debug.thin_active.store(false, Relaxed);

    // rustls 0.23 needs a process-level crypto provider selected explicitly.
    static CRYPTO: std::sync::Once = std::sync::Once::new();
    CRYPTO.call_once(|| {
        let _ = rustls::crypto::ring::default_provider().install_default();
    });

    let (ws, _) = tokio_tungstenite::connect_async(url).await?;
    log::info!("[net] connected {url}");
    *debug.server.lock().unwrap() = url.to_string();
    let (mut write, mut read) = ws.split();

    // Ask for the one-time SYNC keyframe (full VRAM/PVR base).
    write
        .send(Message::Text("{\"type\":\"request_sync\"}".into()))
        .await?;

    let mut zcs2 = Zcs2::new();
    let mut thin = false;
    let mut bytes: u64 = 0;
    let mut t0 = std::time::Instant::now();

    while let Some(msg) = read.next().await {
        let b = match msg? {
            Message::Binary(b) => b,
            Message::Close(c) => {
                log::warn!("[net] server closed: {c:?}");
                break;
            }
            _ => continue,
        };
        bytes += b.len() as u64;
        let el = t0.elapsed().as_secs_f64();
        if el >= 2.0 {
            let mbps = (bytes as f64 * 8.0) / el / 1e6;
            debug.set_mbps(mbps);
            log::info!("[net] {:.2} Mbps{}", mbps, if thin { " (thin)" } else { "" });
            bytes = 0;
            t0 = std::time::Instant::now();
        }
        if b.len() < 4 {
            continue;
        }
        match &b[0..4] {
            b"ZCST" => {
                // one-time SYNC base only; legacy deltas are ignored (thin wire)
                let _ = shared.lock().unwrap().apply_sync_zcst(&b);
            }
            b"ZCS2" => match zcs2.feed(&b) {
                Ok(Some(inner)) => {
                    let renderable = {
                        let mut fd = shared.lock().unwrap();
                        if let Err(e) = fd.apply_delta_frame(&inner) {
                            log::warn!("[frame] {e}");
                        }
                        fd.renderable
                    };
                    // Signal a new stage/wire frame -> wakes the render loop + drives wire-fps.
                    debug.wire_frame.fetch_add(1, Relaxed);
                    // E2E probe: the server's self-locating E2EP tail (last 32B, magic 'E2EP',
                    // MAPLECAST_E2E_PROBE=1) carries the client input seq it latched into this
                    // frame, per slot. Record ours -> the render loop times press->present.
                    let n = inner.len();
                    if n >= 32 && &inner[n - 4..] == b"E2EP" {
                        let s0 = u32::from_le_bytes(inner[n - 12..n - 8].try_into().unwrap());
                        let s1 = u32::from_le_bytes(inner[n - 8..n - 4].try_into().unwrap());
                        debug.e2e_echo(s0, s1);
                    }
                    if !thin && renderable {
                        // Tell the relay to stop forwarding the heavy legacy deltas.
                        write
                            .send(Message::Text("{\"type\":\"subscribe\",\"mode\":\"zcs2\"}".into()))
                            .await
                            .ok();
                        thin = true;
                        debug.thin_active.store(true, Relaxed);
                        log::info!("[net] thin ZCS2 wire active");
                    }
                }
                Ok(None) => {}
                Err(e) => log::warn!("[zcs2] {e}"),
            },
            _ => {} // GSTA/OBJS/PALF/TXTR/... side-channels: not needed for the TA render
        }
    }
    Ok(())
}
