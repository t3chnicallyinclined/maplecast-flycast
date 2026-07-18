//! Network client: direct wss:// to nobd, thin ZCS2 wire -> shared FrameDecoder.
//!
//! Exactly what webgpu-test.html does with "ZCS2" checked: request a SYNC base on
//! connect, then decode the thin ZCS2 per-frame wire, and tell the relay to drop
//! the heavy legacy deltas. rustls speaks TLS, so this reaches nobd directly.

use crate::frame::FrameDecoder;
use crate::tdw::Tdw;
use crate::zcs2::Zcs2;
use futures_util::{SinkExt, StreamExt};
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use tokio_tungstenite::tungstenite::Message;

type BoxErr = Box<dyn std::error::Error + Send + Sync>;

pub fn spawn_net_thread(
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
                // STARTUP = the SERVER PICKER (user decision 2026-07-16): the
                // panel opens on the Servers tab with live pings; NOTHING
                // connects until the user picks (clean direct connect, no
                // auto-placement dance). MC_AUTO=1 restores the probe placement
                // (with a 10s fallback). Local play is always manual.
                {
                    use std::sync::atomic::Ordering::Relaxed;
                    // MC_CONNECT=<idx>: connect straight to servers[idx] (0 = the local
                    // rig / MAPLECAST_WS), skipping the picker AND the auto-closest probe
                    // (which deliberately excludes local). For one-command local testing.
                    if let Some(i) = std::env::var("MC_CONNECT").ok()
                        .and_then(|s| s.trim().parse::<u64>().ok())
                    {
                        debug.switch_server.store(i, Relaxed);
                    }
                    let auto = std::env::var("MC_NO_AUTO").is_err();
                    let t0 = std::time::Instant::now();
                    loop {
                        if debug.manual_server.load(Relaxed)
                            || debug.switch_server.load(Relaxed) != u64::MAX
                        {
                            break;
                        }
                        // a connect ▶ click before any session: nothing to carry —
                        // consume it as the first placement.
                        let t = debug.transfer_server.load(Relaxed);
                        if t != u64::MAX {
                            debug.transfer_server.store(u64::MAX, Relaxed);
                            debug.switch_server.store(t, Relaxed);
                            break;
                        }
                        if auto && t0.elapsed().as_secs() >= 10 {
                            break; // offline fallback: connect to whatever is active
                        }
                        *debug.migrate_status.lock().unwrap() = if auto {
                            "finding the closest server... (or pick one)".into()
                        } else {
                            "pick a server to connect (Servers tab)".into()
                        };
                        tokio::time::sleep(std::time::Duration::from_millis(250)).await;
                    }
                }
                loop {
                    // apply a pending switch BEFORE connecting (startup placement
                    // and dead-wire cases both land here with no live socket)
                    {
                        use std::sync::atomic::Ordering::Relaxed;
                        let s = debug.switch_server.load(Relaxed);
                        if s != u64::MAX {
                            debug.switch_server.store(u64::MAX, Relaxed);
                            debug.active_server.store(s, Relaxed);
                            shared.lock().unwrap().reset_for_new_server();
                        }
                    }
                    // the ACTIVE server's wire url (settings panel can switch it)
                    let url = {
                        use std::sync::atomic::Ordering::Relaxed;
                        let idx = debug.active_server.load(Relaxed) as usize;
                        let servers = debug.servers.lock().unwrap();
                        servers
                            .get(idx)
                            .map(|s| s.ws.clone())
                            .unwrap_or_else(|| "ws://127.0.0.1:7200".into())
                    };
                    if let Err(e) = run(&url, &shared, &debug).await {
                        log::warn!("[net] {e} — reconnecting");
                    }
                    // A connect ▶ click (or the startup auto-closest pick) needs
                    // the CURRENT wire to deliver it; if we're here the wire is
                    // down, so apply the request as a plain switch instead of
                    // spinning on the dead server forever.
                    {
                        use std::sync::atomic::Ordering::Relaxed;
                        let t = debug.transfer_server.load(Relaxed);
                        if t != u64::MAX {
                            debug.transfer_server.store(u64::MAX, Relaxed);
                            debug.active_server.store(t, Relaxed);
                            shared.lock().unwrap().reset_for_new_server();
                            *debug.migrate_status.lock().unwrap() =
                                "old server unreachable — connected fresh (no state to carry)".into();
                        }
                        let s = debug.switch_server.load(Relaxed);
                        if s != u64::MAX {
                            debug.switch_server.store(u64::MAX, Relaxed);
                            debug.active_server.store(s, Relaxed);
                            shared.lock().unwrap().reset_for_new_server();
                            log::info!("[net] switch applied while disconnected -> #{s}");
                        }
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
    // Players mode: shed the legacy legs server-side (ZCST deltas/ZCS2/GSTA/
    // PALF stop being SENT, not just skipped) — on prod-class servers those
    // legs are multi-Mbps that this client would otherwise pay for and drop.
    if std::env::var("MC_TDW").as_deref() == Ok("players") {
        write
            .send(Message::Text("{\"type\":\"subscribe\",\"mode\":\"tdw\"}".into()))
            .await?;
        log::info!("[net] subscribed tdw-only (legacy legs shed server-side)");
    }

    let mut zcs2 = Zcs2::new();
    let mut thin = false;
    let mut bytes: u64 = 0;
    let mut t0 = std::time::Instant::now();

    // G0 wire capture (docs/TDW2-DESIGN.md): MC_TDW_CAPTURE=<path> dumps every
    // binary wire message as [u32 LE len][bytes] for the deterministic loss gate
    // (maplecast-native gate <path>). Fresh file per connect; flushed per message
    // (a dev tool, not the hot path).
    let mut capture = std::env::var("MC_TDW_CAPTURE").ok().and_then(|p| {
        match std::fs::File::create(&p) {
            Ok(f) => { log::info!("[gate] capturing wire -> {p}"); Some(std::io::BufWriter::new(f)) }
            Err(e) => { log::warn!("[gate] capture open failed {p}: {e}"); None }
        }
    });

    // "host[:port]" from a ws:// URL — the address another fleet node dials for
    // a state hand-off, and the key we match "migrated" replies against.
    fn ws_dest(ws: &str) -> String {
        ws.trim_start_matches("wss://")
            .trim_start_matches("ws://")
            .split('/')
            .next()
            .unwrap_or("")
            .to_string()
    }

    // === TDW dict-wire test mode (MC_TDW=gate|render, docs/TA-DICT-WIRE-PLAN.md) ===
    // gate:   decode TDW1/TDWS alongside ZCS2 and byte-compare the two TA buffers.
    // render: gate + swap the TDW TA into the FrameDecoder — pixels come from the
    //         dict wire (pages still ride the ZCS2 frame as today).
    let tdw_mode = std::env::var("MC_TDW").unwrap_or_default();
    let tdw_players = tdw_mode == "players";  // players-only wire: render TDW TA directly
    let tdw_on = tdw_mode == "gate" || tdw_mode == "render" || tdw_players;
    let tdw_render = tdw_mode == "render";
    let mut tdw = Tdw::new();
    let mut arr_probe = crate::arrival::ArrivalProbe::new("TCP");   // S0 same-stage jitter probe
    let mut tdw_pending: HashMap<u32, crate::tdw::TdwFrame> = HashMap::new();
    let (mut tdw_eq, mut tdw_ne) = (0u64, 0u64);
    if tdw_on {
        log::info!("[tdw] mode={tdw_mode}");
        *debug.tdw_mode.lock().unwrap() = tdw_mode.clone();
    }
    // Per-leg wire byte counters over the same 2s window as the total (the F1
    // overlay decomposes the "bandwidth" number — locally EVERY leg rides the
    // one socket, so the total alone is misleading).
    let (mut b_zcst, mut b_zcs2, mut b_tdw, mut b_side) = (0u64, 0u64, 0u64, 0u64);
    // connect ▶ in flight: the target index, so a migrate_failed can fall back
    // to a plain switch to the SAME target instead of leaving the user parked.
    let mut pending_transfer: Option<u64> = None;

    while let Some(msg) = read.next().await {
        let b = match msg? {
            Message::Binary(b) => b,
            Message::Text(t) => {
                // Server JSON: migration replies (docs/STATE-HANDOFF-PLAN.md).
                // "migrated" -> the dest node now RUNS OUR GAME; follow it via
                // the normal switch machinery (reset_for_new_server + input
                // retarget fire on reconnect).
                if let Ok(j) = serde_json::from_str::<serde_json::Value>(&t) {
                    match j.get("type").and_then(|v| v.as_str()) {
                        Some("migrated") => {
                            let _ = pending_transfer.take();
                            // dest is the ws host[:port] we sent (ws_dest form).
                            let dest = j.get("dest").and_then(|v| v.as_str()).unwrap_or("");
                            let idx = debug
                                .servers
                                .lock()
                                .unwrap()
                                .iter()
                                .position(|s| ws_dest(&s.ws) == dest);
                            if let Some(i) = idx {
                                *debug.migrate_status.lock().unwrap() =
                                    format!("game transferred -> {dest} ✓");
                                log::info!("[migrate] dest confirmed; following the game -> {dest}");
                                debug.switch_server.store(i as u64, Relaxed);
                            } else {
                                *debug.migrate_status.lock().unwrap() =
                                    format!("transferred to {dest} but it's not in the directory");
                            }
                        }
                        Some("migrate_failed") => {
                            let e = j.get("error").and_then(|v| v.as_str()).unwrap_or("unknown");
                            log::warn!("[migrate] failed: {e}");
                            // One-button semantics: the user asked to be ON that
                            // server. If the game couldn't come along (memory
                            // guard, no key, push failure), connect fresh anyway.
                            if let Some(t) = pending_transfer.take() {
                                *debug.migrate_status.lock().unwrap() =
                                    format!("couldn't carry the game ({e}) — connected fresh");
                                debug.switch_server.store(t, Relaxed);
                            } else {
                                *debug.migrate_status.lock().unwrap() = format!("transfer failed: {e}");
                            }
                        }
                        _ => {}
                    }
                }
                continue;
            }
            Message::Close(c) => {
                log::warn!("[net] server closed: {c:?}");
                break;
            }
            _ => continue,
        };
        // server switch requested (settings panel): apply + reconnect fresh.
        {
            let req = debug.switch_server.load(Relaxed);
            if req != u64::MAX {
                debug.active_server.store(req, Relaxed);
                debug.switch_server.store(u64::MAX, Relaxed);
                shared.lock().unwrap().reset_for_new_server();
                log::info!("[net] server switch -> #{req}; reconnecting");
                return Ok(());
            }
        }
        // transfer requested (settings panel): ask the CURRENT server to hand
        // the live game to the target node, then wait for its "migrated" reply.
        {
            let t = debug.transfer_server.load(Relaxed);
            if t != u64::MAX {
                debug.transfer_server.store(u64::MAX, Relaxed);
                // dest = the target's WS host[:port] (the server pushes to the
                // dest's :7200-class mirror WS, NOT its input port).
                let dest = {
                    let servers = debug.servers.lock().unwrap();
                    servers.get(t as usize).map(|s| ws_dest(&s.ws)).unwrap_or_default()
                };
                if dest.is_empty() {
                    *debug.migrate_status.lock().unwrap() = "transfer: target has no host".into();
                } else {
                    let key = std::env::var("MC_FLEET_KEY").unwrap_or_default();
                    let m = serde_json::json!({"type":"migrate","dest":dest,"key":key});
                    let _ = write.send(Message::Text(m.to_string().into())).await;
                    pending_transfer = Some(t);
                    *debug.migrate_status.lock().unwrap() =
                        format!("carrying game -> {dest} ... (snapshot+push in flight)");
                    log::info!("[migrate] requested -> {dest}");
                }
            }
        }
        if let Some(w) = capture.as_mut() {
            use std::io::Write;
            let _ = w.write_all(&(b.len() as u32).to_le_bytes());
            let _ = w.write_all(&b[..]);
            let _ = w.flush();
        }
        bytes += b.len() as u64;
        if b.len() >= 4 {
            match &b[0..4] {
                b"ZCST" => b_zcst += b.len() as u64,
                b"ZCS2" => b_zcs2 += b.len() as u64,
                b"TDW1" | b"TDWS" => b_tdw += b.len() as u64,
                _ => b_side += b.len() as u64,
            }
        }
        let el = t0.elapsed().as_secs_f64();
        if el >= 2.0 {
            let mbps = (bytes as f64 * 8.0) / el / 1e6;
            let to_mbps = |v: u64| (v as f64 * 8.0) / el / 1e6;
            debug.set_mbps(mbps);
            debug.set_leg_mbps(to_mbps(b_zcst), to_mbps(b_zcs2), to_mbps(b_tdw), to_mbps(b_side));
            log::info!(
                "[net] {:.2} Mbps (zcst {:.2} | zcs2 {:.2} | tdw {:.2} | side {:.2}){}",
                mbps, to_mbps(b_zcst), to_mbps(b_zcs2), to_mbps(b_tdw), to_mbps(b_side),
                if thin { " (thin)" } else { "" }
            );
            bytes = 0;
            (b_zcst, b_zcs2, b_tdw, b_side) = (0, 0, 0, 0);
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
            b"TDWS" if tdw_on => {
                if let Err(e) = tdw.feed_snapshot(&b) {
                    log::warn!("[tdw] {e}");
                }
                if let Some(ep) = tdw.epoch() {
                    debug.tdw_epoch.store(ep as u64, Relaxed);
                }
                debug.tdw_dict_blocks.store(tdw.dict_len() as u64, Relaxed);
                debug.tdw_dict_kb.store((tdw.dict_bytes() / 1024) as u64, Relaxed);
                debug.tdw_synced.store(tdw.is_synced(), Relaxed);
            }
            b"TDW1" if tdw_on => {
                match tdw.feed(&b) {
                    Ok(Some(fr)) => {
                        if tdw_players {
                            arr_probe.on_frame(fr.frame_num);   // S0: same-stage arrival jitter + loss
                            // TDW-ONLY: apply directly — no ZCS2 pairing at all.
                            // TDW1 carries geometry+camera+pvr+pages+E2E tail.
                            {
                                let mut fd = shared.lock().unwrap();
                                if let Some(pg) = fr.pages.as_deref() {
                                    let n = fd.apply_page_section(pg, 0);
                                    debug.tdw_pages.store(n as u64, Relaxed);
                                }
                                fd.tdw_ta = Some(fr.ta);
                                if fr.cam.is_some() {
                                    fd.tdw_cam = fr.cam;
                                }
                                if let Some(p) = fr.pvr {
                                    fd.pvr_snapshot = p;
                                }
                                fd.mark_tdw_frame(fr.frame_num);
                            }
                            if let Some((s0, s1)) = fr.e2e {
                                debug.e2e_echo(s0, s1);
                            }
                            tdw_eq += 1;
                            debug.tdw_eq.store(tdw_eq, Relaxed);
                            debug.wire_frame.fetch_add(1, Relaxed);
                        } else {
                            let cur = fr.frame_num;
                            tdw_pending.insert(fr.frame_num, fr);
                            if tdw_pending.len() > 240 {
                                tdw_pending.retain(|k, _| cur.wrapping_sub(*k) < 120);
                            }
                            debug.tdw_pending_depth.store(tdw_pending.len() as u64, Relaxed);
                        }
                    }
                    Ok(None) => {}
                    Err(e) => log::warn!("[tdw] {e}"),
                }
                debug.tdw_dict_blocks.store(tdw.dict_len() as u64, Relaxed);
                debug.tdw_dict_kb.store((tdw.dict_bytes() / 1024) as u64, Relaxed);
                debug.tdw_synced.store(tdw.is_synced(), Relaxed);
            }
            // TDW-ONLY client: ZCS2 fully ignored in players mode (no zstd decode,
            // no apply — TDW1 is the complete wire). Other modes keep it.
            b"ZCS2" if tdw_players => {}
            b"ZCS2" => match zcs2.feed(&b) {
                Ok(Some(inner)) => {
                    // TDW pairing: the server broadcasts TDW1(N) before ZCS2(N),
                    // so the dict TA for this frame is already pending.
                    let tdw_ta = if tdw_on && inner.len() >= 8 {
                        let fnum = u32::from_le_bytes(inner[4..8].try_into().unwrap());
                        tdw_pending.remove(&fnum)
                    } else {
                        None
                    };
                    let renderable = {
                        let mut fd = shared.lock().unwrap();
                        if let Err(e) = fd.apply_delta_frame(&inner) {
                            log::warn!("[frame] {e}");
                        }
                        if let Some(fr) = tdw_ta {
                            {
                                let ta = fr.ta;
                                if fd.ta() == ta.as_slice() {
                                    tdw_eq += 1;
                                } else {
                                    tdw_ne += 1;
                                    log::warn!(
                                        "[tdw] TA MISMATCH frame={} chain={}B tdw={}B",
                                        fd.frame_num, fd.ta().len(), ta.len()
                                    );
                                }
                                debug.tdw_eq.store(tdw_eq, Relaxed);
                                debug.tdw_ne.store(tdw_ne, Relaxed);
                                if tdw_render {
                                    fd.replace_ta(&ta); // pixels now come from the dict wire
                                }
                                if (tdw_eq + tdw_ne) % 300 == 0 {
                                    log::info!("[tdw] eq={tdw_eq} ne={tdw_ne} mode={tdw_mode}");
                                }
                            }
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
            b"GSTA" if tdw_players => {
                // 253B game state — feeds the state-drawn HUD (hud.rs).
                if b.len() > 4 {
                    shared.lock().unwrap().gsta = Some(b[4..].to_vec());
                }
            }
            _ => {} // OBJS/PALF/TXTR/... side-channels: not needed for the TA render
        }
    }
    Ok(())
}
