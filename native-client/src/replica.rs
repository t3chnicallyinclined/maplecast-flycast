//! Replica-live wire consumer.
//!
//! Rebuilds and maintains the resident 16 MiB SH4 area-3 RAM image (plus VRAM and
//! PVR registers/palette) that `render_frame` draws from, off the MapleCast
//! `/replica-live` WebSocket — a second socket alongside the TA-mirror wire.
//!
//! Ported byte-for-byte from the C++ producer
//! (core/network/maplecast_replica_live.cpp: buildPrefixLocked ~549, captureFrame ~810,
//! the region tables ~408) and the reference consumer
//! (core/network/maplecast_mirror.cpp: gstaApplyPrefix ~4667, gstaApplyFrame ~5573,
//! the receive loop ~6456).
//!
//! WIRE ENVELOPE: every `/replica-live` binary message is a ONE-SHOT ZCST envelope —
//! `"ZCST"(4) + uncompressedSize(u32 LE) + zstd_blob(N)` — decoded exactly like the
//! main mirror wire's `MirrorDecompressor` (core/network/maplecast_compress.h). It is
//! NOT the ZCS2 persistent-streaming envelope: the producer's senderLoop
//! (maplecast_replica_live.cpp:706 `_frameComp.compress(..., 1)`, prefix
//! buildPrefixLocked:606 level 3) compresses each message independently, and the
//! consumer (maplecast_mirror.cpp:6460) feeds each message through a stateless
//! `MirrorDecompressor::decompress`. The first message carries the MCRR static prefix
//! (seed once); every later message is an FRMx per-frame patch.

use futures_util::StreamExt;
use std::collections::VecDeque;
use std::sync::{Arc, Mutex};
use tokio_tungstenite::tungstenite::Message;
use zstd_safe::DCtx;

type BoxErr = Box<dyn std::error::Error + Send + Sync>;

// SH4 area-3 RAM (16 MiB), Dreamcast VRAM (8 MiB), PVR reg/palette block (32 KiB).
const RAM_SIZE: usize = 16 * 1024 * 1024;
const VRAM_SIZE: usize = 8 * 1024 * 1024;
const PVR_REG_SIZE: usize = 32 * 1024;

// MCRR / FRMx / tail magics — little-endian on the wire (maplecast_replica_live.cpp:377-381).
const MCRR_MAGIC: u32 = 0x5252_434D; // "MCRR"
const FRMX_MAGIC: u32 = 0x784D_5246; // "FRMx"
const HUDQ_MAGIC: u32 = 0x4855_4451; // "HUDQ"
const BTCW_MAGIC: u32 = 0x5743_5442; // "BTCW"
const PL3D_MAGIC: u32 = 0x4433_4C50; // "PL3D"

// Tail sanity bounds — mirror the oracle capture caps (maplecast_oracle_hook.h:256-272).
const BTCW_MAX_WORDS: usize = 32 * (2 + 128); // MC_BTCW_MAX_NODES * (2 + MC_BTCW_MAX_TILES)
const P3D_LINE_BYTES: usize = 36; // MC_P3D_LINE_BYTES
const P3D_MAX_BYTES: usize = 910 * P3D_LINE_BYTES; // MC_P3D_MAX_LINES * MC_P3D_LINE_BYTES
const HUDQ_QUAD_BYTES: usize = 96; // sizeof(HudQuad) — wire interface

/// One entry of the MCRR region table: `{ addr u32, len u32, tag[8] }`.
#[derive(Clone, Default)]
struct Region {
    addr: u32,
    len: u32,
    tag: [u8; 8],
}

pub struct ReplicaState {
    pub ram: Vec<u8>,       // 16 MiB SH4 area-3 RAM (render_frame's ctx.ram)
    pub vram: Vec<u8>,      // 8 MiB VRAM from the prefix (body-texture decode target, later)
    pub pvr_regs: Vec<u8>,  // 32 KiB PVR regs / palette
    pub body_tcws: Vec<u32>, // latest BTCW override (empty if none this frame)
    pub vframe: u32,
    pub seeded: bool, // true once the MCRR prefix has been applied

    // Dynamic region table from the prefix — defines each FRMx frame's region
    // addr/len/order. Captured once at seed, reused for every FRMx patch.
    dyn_regs: Vec<Region>,
}

impl ReplicaState {
    pub fn new() -> Self {
        Self {
            ram: vec![0u8; RAM_SIZE],
            vram: vec![0u8; VRAM_SIZE],
            pvr_regs: vec![0u8; PVR_REG_SIZE],
            body_tcws: Vec::new(),
            vframe: 0,
            seeded: false,
            dyn_regs: Vec::new(),
        }
    }

    /// Apply the MCRR static prefix. Seeds `ram` (16 MiB), `vram` (8 MiB) and
    /// `pvr_regs` (32 KiB), splats the static regions, and records the dynamic
    /// region table for later FRMx patches. Returns false on a malformed prefix.
    /// Port of maplecast_mirror.cpp gstaApplyPrefix (~4667).
    fn apply_prefix(&mut self, d: &[u8]) -> bool {
        let n = d.len();
        if n < 32 || rd32(d, 0) != MCRR_MAGIC {
            return false;
        }
        // header (32B): magic, version, nStatic, nDynamic, nFrames, vramBytes, pvrBytes, reserved
        let n_static = rd32(d, 8) as usize;
        let n_dynamic = rd32(d, 12) as usize;
        let vram_bytes = rd32(d, 20) as usize;
        let pvr_bytes = rd32(d, 24) as usize;
        let mut p = 32usize;

        // Static + dynamic region tables: nStatic+nDynamic × { addr u32, len u32, tag[8] } (16B each).
        if (n_static + n_dynamic)
            .checked_mul(16)
            .and_then(|t| t.checked_add(p))
            .map_or(true, |end| end > n)
        {
            return false;
        }
        let mut static_regs = Vec::with_capacity(n_static);
        for _ in 0..n_static {
            static_regs.push(read_region(d, p));
            p += 16;
        }
        self.dyn_regs.clear();
        self.dyn_regs.reserve(n_dynamic);
        for _ in 0..n_dynamic {
            self.dyn_regs.push(read_region(d, p));
            p += 16;
        }

        // Static payload: VRAM (8 MiB), PVR regs (32 KiB), then each static region's bytes.
        if p + vram_bytes + pvr_bytes > n {
            return false;
        }
        if vram_bytes <= VRAM_SIZE {
            self.vram[..vram_bytes].copy_from_slice(&d[p..p + vram_bytes]);
        }
        p += vram_bytes;
        if pvr_bytes <= PVR_REG_SIZE {
            self.pvr_regs[..pvr_bytes].copy_from_slice(&d[p..p + pvr_bytes]);
        }
        p += pvr_bytes;

        for r in &static_regs {
            let len = r.len as usize;
            if p + len > n {
                return false;
            }
            if tag_eq(&r.tag, "ram16") {
                // The "ram16" region is the full 16 MiB RAM image (last static region).
                let m = len.min(self.ram.len());
                self.ram[..m].copy_from_slice(&d[p..p + m]);
            } else {
                let off = (r.addr & 0x00FF_FFFF) as usize;
                if off + len <= self.ram.len() {
                    self.ram[off..off + len].copy_from_slice(&d[p..p + len]);
                }
            }
            p += len;
        }

        self.seeded = true;
        true
    }

    /// Patch one FRMx frame into the resident image. Port of maplecast_mirror.cpp
    /// gstaApplyFrame (~5573): dynamic regions in table order, then the optional
    /// GFX / PALETTE / HUDQ / BTCW / PL3D tails (parsed defensively, present-or-not).
    pub fn apply_frame(&mut self, d: &[u8]) {
        let n = d.len();
        if n < 12 || rd32(d, 0) != FRMX_MAGIC {
            return;
        }
        let mut p = 4usize;
        self.vframe = rd32(d, p);
        p += 4;
        // taSize (0 in the live stream) — skipped.
        p += 4;

        // Dynamic regions in table order: splat each into ram at addr & 0xFFFFFF.
        // (A "bodytex" region is the retired texture band and is skipped; none ship today.)
        for i in 0..self.dyn_regs.len() {
            let addr = self.dyn_regs[i].addr;
            let len = self.dyn_regs[i].len as usize;
            let skip = tag_eq(&self.dyn_regs[i].tag, "bodytex");
            if p + len > n {
                return; // truncated frame
            }
            if !skip {
                let off = (addr & 0x00FF_FFFF) as usize;
                if off + len <= self.ram.len() {
                    self.ram[off..off + len].copy_from_slice(&d[p..p + len]);
                }
            }
            p += len;
        }

        // GFX tail: u32 nGfx, then nGfx × { u32 base, u32 len, len bytes } splatted at base & 0xFFFFFF.
        if p + 4 <= n {
            let n_gfx = rd32(d, p);
            if n_gfx <= 64 {
                p += 4;
                for _ in 0..n_gfx {
                    if p + 8 > n {
                        break;
                    }
                    let base = rd32(d, p);
                    let len = rd32(d, p + 4) as usize;
                    p += 8;
                    if len > 0x0080_0000 || p + len > n {
                        break;
                    }
                    let off = (base & 0x00FF_FFFF) as usize;
                    if off + len <= self.ram.len() {
                        self.ram[off..off + len].copy_from_slice(&d[p..p + len]);
                    }
                    p += len;
                }
            }
        }

        // PALETTE tail: u32 palLen, then palLen bytes overwriting pvr_regs (0 in steady state).
        if p + 4 <= n {
            let pal_len = rd32(d, p) as usize;
            p += 4;
            if pal_len != 0 && pal_len <= PVR_REG_SIZE && p + pal_len <= n {
                self.pvr_regs[..pal_len].copy_from_slice(&d[p..p + pal_len]);
                p += pal_len;
            }
        }

        // HUDQ tail: u32 magic, u32 nHud, nHud × 96-byte HudQuad — skipped (HUD render is later).
        if p + 8 <= n && rd32(d, p) == HUDQ_MAGIC {
            let n_hud = rd32(d, p + 4) as usize;
            if n_hud <= 4096 && p + 8 + n_hud * HUDQ_QUAD_BYTES <= n {
                p += 8 + n_hud * HUDQ_QUAD_BYTES;
            }
        }

        // BTCW tail: u32 magic, u32 nWords, nWords × u32 resolved per-tile body TCWs.
        // Cleared every frame (matches render_frame_set_body_tcws(nullptr,0)); filled only if present.
        self.body_tcws.clear();
        if p + 8 <= n && rd32(d, p) == BTCW_MAGIC {
            let n_words = rd32(d, p + 4) as usize;
            if n_words <= BTCW_MAX_WORDS && p + 8 + n_words * 4 <= n {
                p += 8;
                self.body_tcws.reserve(n_words);
                for _ in 0..n_words {
                    self.body_tcws.push(rd32(d, p));
                    p += 4;
                }
            }
        }

        // PL3D tail: u32 magic, u32 nBytes, nBytes of 36-byte flush records — skipped for now.
        if p + 8 <= n && rd32(d, p) == PL3D_MAGIC {
            let nb = rd32(d, p + 4) as usize;
            if nb <= P3D_MAX_BYTES && nb % P3D_LINE_BYTES == 0 && p + 8 + nb <= n {
                p += 8 + nb;
            }
        }
        let _ = p; // last cursor advance is intentionally unread
    }
}

impl Default for ReplicaState {
    fn default() -> Self {
        Self::new()
    }
}

/// Read a `{ addr u32, len u32, tag[8] }` region entry at `p`. Caller guarantees `p + 16 <= len`.
fn read_region(d: &[u8], p: usize) -> Region {
    let mut r = Region {
        addr: rd32(d, p),
        len: rd32(d, p + 4),
        tag: [0u8; 8],
    };
    r.tag.copy_from_slice(&d[p + 8..p + 16]);
    r
}

/// Compare a null-padded 8-byte wire tag against a Rust string (C `strcmp` semantics).
fn tag_eq(tag: &[u8; 8], s: &str) -> bool {
    let b = s.as_bytes();
    b.len() < 8 && tag[..b.len()] == *b && tag[b.len()] == 0
}

/// Little-endian u32 read. Caller guarantees `o + 4 <= d.len()`.
#[inline]
fn rd32(d: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]])
}

/// One-shot ZCST decoder with a reused output buffer — the Rust analogue of the C++
/// `MirrorDecompressor` (core/network/maplecast_compress.h). Non-ZCST messages pass
/// through verbatim (the compressor's raw fallback), matching the reference.
struct Decompressor {
    dctx: DCtx<'static>,
    buf: Vec<u8>,
}

impl Decompressor {
    fn new() -> Self {
        Self {
            dctx: DCtx::create(),
            buf: Vec::new(),
        }
    }

    fn decompress(&mut self, msg: &[u8]) -> Option<&[u8]> {
        // Raw-fallback / non-envelope passthrough (MirrorDecompressor returns src as-is).
        if msg.len() < 8 || &msg[0..4] != b"ZCST" {
            self.buf.clear();
            self.buf.extend_from_slice(msg);
            return Some(&self.buf[..]);
        }
        let uncomp = rd32(msg, 4) as usize;
        if self.buf.len() < uncomp {
            self.buf.resize(uncomp, 0);
        }
        match self.dctx.decompress(&mut self.buf[..uncomp], &msg[8..]) {
            Ok(got) => Some(&self.buf[..got]),
            Err(code) => {
                log::warn!("[replica] zstd decode: {}", zstd_safe::get_error_name(code));
                None
            }
        }
    }
}

/// Connect `wss://.../replica-live`, seed from MCRR, patch every FRMx into `shared`.
/// Reconnects on error. Runs on its own thread with a single-threaded tokio runtime.
pub fn spawn_replica_thread(
    url: String,
    shared: Arc<Mutex<ReplicaState>>,
    queue: Arc<Mutex<VecDeque<Vec<u8>>>>,
    debug: Arc<crate::debug::DebugState>,
) {
    std::thread::Builder::new()
        .name("maplecast-replica".into())
        .spawn(move || {
            let rt = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .expect("tokio runtime");
            rt.block_on(async move {
                loop {
                    if let Err(e) = run(&url, &shared, &queue, &debug).await {
                        log::warn!("[replica] {e} — reconnecting");
                    }
                    tokio::time::sleep(std::time::Duration::from_secs(1)).await;
                }
            });
        })
        .expect("spawn replica thread");
}

async fn run(
    url: &str,
    shared: &Arc<Mutex<ReplicaState>>,
    queue: &Arc<Mutex<VecDeque<Vec<u8>>>>,
    debug: &crate::debug::DebugState,
) -> Result<(), BoxErr> {
    // rustls 0.23 needs a process-level crypto provider selected explicitly (see net.rs).
    static CRYPTO: std::sync::Once = std::sync::Once::new();
    CRYPTO.call_once(|| {
        let _ = rustls::crypto::ring::default_provider().install_default();
    });

    let (mut ws, _) = tokio_tungstenite::connect_async(url).await?;
    log::info!("[replica] connected {url}");

    let mut decomp = Decompressor::new();
    while let Some(msg) = ws.next().await {
        let b = match msg? {
            Message::Binary(b) => b,
            Message::Close(c) => {
                log::warn!("[replica] server closed: {c:?}");
                break;
            }
            _ => continue,
        };
        let inner = match decomp.decompress(&b) {
            Some(x) => x,
            None => continue,
        };
        if inner.len() < 12 {
            continue;
        }
        let magic = rd32(inner, 0);
        // A prefix message (re)seeds — robust to reconnect; frames apply only once seeded.
        if magic == MCRR_MAGIC {
            let mut st = shared.lock().unwrap();
            if st.apply_prefix(inner) {
                use std::sync::atomic::Ordering::Relaxed;
                debug.seeded.store(true, Relaxed);
                debug.regions.store(st.dyn_regs.len() as u64, Relaxed);
                log::info!(
                    "[replica] seeded from MCRR prefix ({} dynamic regions)",
                    st.dyn_regs.len()
                );
            }
        } else if magic == FRMX_MAGIC {
            // Queue every frame; the render loop is the SINGLE consumer (paced release when
            // the jitter buffer is on, immediate drain when off). One consumer keeps the
            // FRMx delta sequence strictly in order.
            queue.lock().unwrap().push_back(inner.to_vec());
        }
    }
    Ok(())
}
