//! M1 (step 1): direct wss:// connect to nobd + log what arrives.
//!
//! This is the payoff of going Rust-native: rustls speaks TLS, so we reach
//! `wss://nobd.net/ws` DIRECTLY — the exact thing the C++ flycast client could
//! not do (its build dropped OpenSSL). No tunnel, no plaintext port.
//!
//! For now this just connects and logs each binary message's magic + length, to
//! prove the connection and show what the default subscription yields. The
//! ZCS2/ZCST decode (grounded in the browser worker) lands next; decoded frames
//! will flow to the renderer over a channel.

use futures_util::StreamExt;
use tokio_tungstenite::tungstenite::Message;

type BoxErr = Box<dyn std::error::Error + Send + Sync>;

/// Spawn the network client on its own thread + tokio runtime (off the render loop).
pub fn spawn_net_thread(url: String) {
    std::thread::Builder::new()
        .name("maplecast-net".into())
        .spawn(move || {
            let rt = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .expect("tokio runtime");
            rt.block_on(async move {
                if let Err(e) = run(&url).await {
                    log::error!("[net] {e}");
                }
            });
        })
        .expect("spawn net thread");
}

pub async fn run(url: &str) -> Result<(), BoxErr> {
    // rustls 0.23 needs a process-level crypto provider selected explicitly.
    static CRYPTO: std::sync::Once = std::sync::Once::new();
    CRYPTO.call_once(|| {
        let _ = rustls::crypto::ring::default_provider().install_default();
    });

    log::info!("[net] connecting {url} (rustls TLS) ...");
    let (ws, resp) = tokio_tungstenite::connect_async(url).await?;
    log::info!("[net] connected — HTTP {}", resp.status());

    let (mut _write, mut read) = ws.split();
    let mut n: u64 = 0;
    let mut zcst_decoded: u64 = 0;
    let mut by_magic: std::collections::BTreeMap<String, u64> = Default::default();

    while let Some(msg) = read.next().await {
        match msg? {
            Message::Binary(b) => {
                n += 1;
                let magic: String = b
                    .iter()
                    .take(4)
                    .map(|&c| if c.is_ascii_graphic() { c as char } else { '.' })
                    .collect();
                *by_magic.entry(magic.clone()).or_default() += 1;

                // Validate the ZCST decode path against real frames (first few).
                if magic == "ZCST" && zcst_decoded < 5 {
                    zcst_decoded += 1;
                    match crate::decode::decompress_zcst(&b) {
                        Ok(frame) => match crate::decode::parse_delta_header(&frame) {
                            Some(h) => log::info!(
                                "[decode] ZCST #{zcst_decoded}: wire={}B -> {}B  frame_num={} ta_size={} delta={}B dirty_pages={}",
                                b.len(), h.decompressed_len, h.frame_num, h.ta_size, h.delta_payload_size, h.dirty_page_count
                            ),
                            None => log::warn!("[decode] ZCST #{zcst_decoded}: decompressed {}B but header parse failed", frame.len()),
                        },
                        Err(e) => log::warn!("[decode] ZCST #{zcst_decoded} decompress failed: {e}"),
                    }
                }

                if n <= 24 || n % 120 == 0 {
                    log::info!("[net] msg #{n} magic='{magic}' len={}  seen={:?}", b.len(), by_magic);
                }
            }
            Message::Ping(p) => log::debug!("[net] ping {}B", p.len()),
            Message::Close(c) => {
                log::warn!("[net] server closed: {c:?}");
                break;
            }
            _ => {}
        }
    }
    log::info!("[net] stream ended after {n} binary msgs; magics={by_magic:?}");
    Ok(())
}
