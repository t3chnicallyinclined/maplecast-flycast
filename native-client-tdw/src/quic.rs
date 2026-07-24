//! QUIC datagram/stream wire (roadmap D1, Phase 1). MC_QUIC=1 connects to the
//! co-located maplecast-quic-bridge instead of the TCP WS, and receives the TDW
//! players wire as:
//!   - datagrams  -> TDW1 frames (unreliable; a lost one is skipped, the next
//!     full snapshot arrives on time — NO TCP head-of-line stall)
//!   - uni-streams -> SYNC / TDWS / oversized frames (reliable, each its own
//!     stream so one stalled frame never blocks the others)
//!
//! Phase-1 goal is as much MEASUREMENT as transport: it logs real datagram loss%
//! (TDW1 frame-number gaps) and inter-arrival jitter, so we finally see whether
//! the path impairment is LOSS (redundancy helps) or DELAY (buffering helps).
//! The players-mode frame handling mirrors net.rs; kept separate so the working
//! WS path is untouched (unify once QUIC proves out).

use crate::debug::DebugState;
use crate::frame::FrameDecoder;
use crate::tdw::Tdw;
use std::sync::atomic::Ordering::Relaxed;
use std::sync::{Arc, Mutex};

type BoxErr = Box<dyn std::error::Error + Send + Sync>;

/// Spawn the QUIC net thread. Reconnects on drop like the WS thread.
pub fn spawn_quic_thread(shared: Arc<Mutex<FrameDecoder>>, debug: Arc<DebugState>) {
    std::thread::Builder::new()
        .name("maplecast-quic".into())
        .spawn(move || {
            let rt = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .expect("tokio rt");
            rt.block_on(async move {
                // MC_QUIC=host:port -> explicit bridge; anything else (1/on/true,
                // or a stray trailing space from `set MC_QUIC=1 &&`) -> default.
                // Only treat it as an address if it actually looks like one.
                let v = std::env::var("MC_QUIC").unwrap_or_default();
                let v = v.trim();
                let addr = if v.contains(':') {
                    v.to_string()
                } else {
                    "127.0.0.1:7300".into()
                };
                loop {
                    if let Err(e) = run(&addr, &shared, &debug).await {
                        log::warn!("[quic] {e} — reconnecting");
                    }
                    tokio::time::sleep(std::time::Duration::from_secs(1)).await;
                }
            });
        })
        .expect("spawn quic thread");
}

/// rustls verifier that accepts any server cert — Phase-1 self-signed bridge.
/// Productionization uses the real play.nobd.net cert and drops this.
#[derive(Debug)]
struct SkipVerify;
impl rustls::client::danger::ServerCertVerifier for SkipVerify {
    fn verify_server_cert(
        &self,
        _e: &rustls::pki_types::CertificateDer<'_>,
        _i: &[rustls::pki_types::CertificateDer<'_>],
        _s: &rustls::pki_types::ServerName<'_>,
        _o: &[u8],
        _n: rustls::pki_types::UnixTime,
    ) -> Result<rustls::client::danger::ServerCertVerified, rustls::Error> {
        Ok(rustls::client::danger::ServerCertVerified::assertion())
    }
    fn verify_tls12_signature(
        &self,
        _m: &[u8],
        _c: &rustls::pki_types::CertificateDer<'_>,
        _d: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }
    fn verify_tls13_signature(
        &self,
        _m: &[u8],
        _c: &rustls::pki_types::CertificateDer<'_>,
        _d: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }
    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        rustls::crypto::ring::default_provider()
            .signature_verification_algorithms
            .supported_schemes()
    }
}

async fn run(addr: &str, shared: &Arc<Mutex<FrameDecoder>>, debug: &DebugState) -> Result<(), BoxErr> {
    static CRYPTO: std::sync::Once = std::sync::Once::new();
    CRYPTO.call_once(|| {
        let _ = rustls::crypto::ring::default_provider().install_default();
    });

    let mut crypto = rustls::ClientConfig::builder()
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(SkipVerify))
        .with_no_client_auth();
    crypto.alpn_protocols = vec![b"mc-tdw".to_vec()];
    let client_cfg = quinn::ClientConfig::new(Arc::new(
        quinn::crypto::rustls::QuicClientConfig::try_from(crypto)?,
    ));

    let mut endpoint = quinn::Endpoint::client("0.0.0.0:0".parse()?)?;
    endpoint.set_default_client_config(client_cfg);

    // Resolve MC_QUIC via DNS — accept host:port (e.g. play.nobd.net:7300), not just
    // ip:port. SocketAddr::parse only handles literal IPs (the "invalid socket address
    // syntax" bug when pointed at prod by hostname).
    use std::net::ToSocketAddrs;
    let server = addr
        .to_socket_addrs()?
        .next()
        .ok_or_else(|| format!("MC_QUIC: could not resolve '{addr}'"))?;
    let conn = endpoint.connect(server, "maplecast-bridge")?.await?;
    log::info!("[quic] connected {server} (datagrams + uni-streams)");
    *debug.server.lock().unwrap() = format!("quic://{server}");

    let mut tdw = Tdw::new();
    // S0 same-stage jitter probe (IDENTICAL metric + log format to the TCP path)
    let mut probe = crate::arrival::ArrivalProbe::new("QUIC");
    let mut bytes = 0u64;
    let mut t0 = std::time::Instant::now();
    let mut last_ack = u32::MAX;

    loop {
        tokio::select! {
            dg = conn.read_datagram() => {
                let b = dg?;
                bytes += b.len() as u64;
                handle(&b, &mut tdw, shared, debug, &mut probe);
            }
            uni = conn.accept_uni() => {
                let mut s = uni?;
                let b = s.read_to_end(32 * 1024 * 1024).await?;
                bytes += b.len() as u64;
                handle(&b, &mut tdw, shared, debug, &mut probe);
            }
            _ = tokio::time::sleep(std::time::Duration::from_millis(250)) => {}
        }

        // ACK-reference: tell the bridge (→ flycast) the highest TDW2 frame we hold, so
        // the server encodes each delta against a frame we can decode. Sent as an 8-byte
        // "ACKF"+frameId QUIC datagram; the bridge forwards it upstream over its WS.
        if let Some(f) = tdw.tdw2_ack_frame() {
            if f != last_ack && f % 2 == 0 {
                last_ack = f;
                let mut m = Vec::with_capacity(8);
                m.extend_from_slice(b"ACKF");
                m.extend_from_slice(&f.to_le_bytes());
                let _ = conn.send_datagram(m.into());
            }
        }

        let el = t0.elapsed().as_secs_f64();
        if el >= 2.0 {
            let mbps = bytes as f64 * 8.0 / el / 1e6;
            debug.set_mbps(mbps);
            debug.set_leg_mbps(0.0, 0.0, mbps, 0.0);
            log::info!("[quic] {mbps:.2} Mbps");
            bytes = 0;
            t0 = std::time::Instant::now();
        }
    }
}

fn handle(
    b: &[u8],
    tdw: &mut Tdw,
    shared: &Arc<Mutex<FrameDecoder>>,
    debug: &DebugState,
    probe: &mut crate::arrival::ArrivalProbe,
) {
    if b.len() < 4 {
        return;
    }
    match &b[0..4] {
        b"ZCST" => {
            let _ = shared.lock().unwrap().apply_sync_zcst(b);
        }
        b"TDWS" => {
            if let Err(e) = tdw.feed_snapshot(b) {
                log::warn!("[quic/tdw] {e}");
            }
            if let Some(ep) = tdw.epoch() {
                debug.tdw_epoch.store(ep as u64, Relaxed);
            }
            debug.tdw_dict_blocks.store(tdw.dict_len() as u64, Relaxed);
            debug.tdw_dict_kb.store((tdw.dict_bytes() / 1024) as u64, Relaxed);
            debug.tdw_synced.store(tdw.is_synced(), Relaxed);
        }
        b"TDW1" | b"TDW2" => match tdw.feed(b) {
            Ok(Some(fr)) => {
                probe.on_frame(fr.frame_num);   // S0: same-stage arrival jitter + loss

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
                drop(fd);
                if let Some((s0, s1)) = fr.e2e {
                    debug.e2e_echo(s0, s1);
                }
                debug.tdw_synced.store(tdw.is_synced(), Relaxed);
                debug.wire_frame.fetch_add(1, Relaxed);
            }
            Ok(None) => {}
            Err(e) => log::warn!("[quic/tdw] feed: {e}"),
        },
        _ => {}
    }
}
