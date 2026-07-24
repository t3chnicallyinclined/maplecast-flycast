//! FrameDecoder — reconstructs the browser's post-decode state from the ZCST wire.
//!
//! ZCST path (fastest to pixels; full bodies, one-shot zstd, no streaming/SoA):
//!   ZCST >1MB  -> SYNC keyframe  (full VRAM + PVR base)
//!   ZCST <=1MB -> legacy delta frame (80B header + TA section + dirty pages)
//! Persistent state across frames: prev_ta (patched by deltas), vram (8MB),
//! pvr_regs (32KB), and a VCACHE hash->page cache.
//! Layout per frame-decoder.mjs:46-224 + CLAUDE.md wire-format. Migrate to the
//! thin ZCS2 wire in a later milestone.

use std::collections::HashMap;

type BoxErr = Box<dyn std::error::Error + Send + Sync>;

const VRAM_SIZE: usize = 8 * 1024 * 1024;
const PVR_SIZE: usize = 32 * 1024;
const PAGE: usize = 4096;

pub struct FrameDecoder {
    pub vram: Vec<u8>,
    pub pvr_regs: Vec<u8>,
    prev_ta: Vec<u8>,
    prev_ta_size: usize,
    has_prev_ta: bool,
    pub frame_num: u32,
    pub pvr_snapshot: [u32; 16],
    pub renderable: bool, // false until we have both a TA base and a SYNC
    have_sync: bool,
    vcache: HashMap<u64, [u8; PAGE]>,
    /// MC_TDW=players: the players-only TA from the TDW wire — rendered
    /// DIRECTLY (instead of the chain TA), while pages/VRAM still come from
    /// the ZCS2 chain. None in every other mode.
    pub tdw_ta: Option<Vec<u8>>,
    /// In-band TDW camera (stage_id, M2 raw dwords, M1 raw dwords) — drives
    /// the local stage reprojection.
    pub tdw_cam: Option<(u32, [f32; 16], [f32; 16])>,
    /// Latest raw GSTA game-state payload (the 253B side-channel, magic
    /// stripped) — feeds the state-drawn HUD.
    pub gsta: Option<Vec<u8>>,
    /// Present-pacing (MC_PACE=N): the net/QUIC receive threads QUEUE decoded TDW frames
    /// here and the render loop releases ONE per vsync, turning bursty network arrival
    /// (p50 12ms / p95 41ms) into an even display cadence. 0 = off (apply immediately).
    tdw_pace_q: std::collections::VecDeque<crate::tdw::TdwFrame>,
    pace_depth: usize,
    pace_primed: bool,
    next_release: Option<std::time::Instant>,
}

impl FrameDecoder {
    pub fn new() -> Self {
        Self {
            vram: vec![0; VRAM_SIZE],
            pvr_regs: vec![0; PVR_SIZE],
            prev_ta: Vec::new(),
            prev_ta_size: 0,
            has_prev_ta: false,
            frame_num: 0,
            pvr_snapshot: [0; 16],
            renderable: false,
            have_sync: false,
            vcache: HashMap::new(),
            tdw_ta: None,
            tdw_cam: None,
            gsta: None,
            tdw_pace_q: std::collections::VecDeque::new(),
            pace_depth: {
                let d = std::env::var("MC_PACE").ok()
                    .and_then(|s| s.trim().parse().ok()).unwrap_or(0);
                if d > 0 {
                    log::info!("[pace] present-pacing ON: {d}-frame buffer, 60fps time-based release");
                }
                d
            },
            pace_primed: false,
            next_release: None,
        }
    }

    /// Present-pacing on (MC_PACE>0)? The receive path queues instead of applying.
    pub fn pace_on(&self) -> bool {
        self.pace_depth > 0
    }

    /// Queue a decoded TDW frame for paced release. Capped at pace_depth+1 so latency
    /// stays ~pace_depth frames: if arrival outruns the 60fps release grid we DROP the
    /// oldest (skip a stale frame, catch up) instead of ballooning to 100s of ms. That
    /// cap being 30 was the "press->present 500ms" bug (30 frames = 500ms).
    pub fn queue_tdw_frame(&mut self, fr: crate::tdw::TdwFrame) {
        self.tdw_pace_q.push_back(fr);
        // Loose safety cap only — the PLL in pace_release holds the buffer near pace_depth
        // by adjusting the release RATE (not by dropping), so this rarely triggers.
        let cap = self.pace_depth + 8;
        while self.tdw_pace_q.len() > cap {
            self.tdw_pace_q.pop_front();
        }
    }

    /// Apply one decoded TDW frame: pages -> VRAM, TA, camera, PVR, E2E echo. Shared by
    /// the immediate receive path and the paced drain.
    pub fn apply_tdw_frame(&mut self, fr: crate::tdw::TdwFrame, debug: &crate::debug::DebugState) {
        use std::sync::atomic::Ordering::Relaxed;
        if let Some(pg) = fr.pages.as_deref() {
            let n = self.apply_page_section(pg, 0);
            debug.tdw_pages.store(n as u64, Relaxed);
        }
        self.tdw_ta = Some(fr.ta);
        if fr.cam.is_some() {
            self.tdw_cam = fr.cam;
        }
        if let Some(p) = fr.pvr {
            self.pvr_snapshot = p;
        }
        self.mark_tdw_frame(fr.frame_num);
        if let Some((s0, s1)) = fr.e2e {
            debug.e2e_echo(s0, s1);
        }
        debug.wire_frame.fetch_add(1, Relaxed);
    }

    /// Present-pacing drain — call ONCE per vsync from the render loop. Primes to
    /// `pace_depth` frames, then releases one per call (catching up 2 if the buffer
    /// overran, re-priming on underrun) so the display advances at a steady cadence.
    pub fn pace_release(&mut self, debug: &crate::debug::DebugState, now: std::time::Instant) {
        use std::sync::atomic::Ordering::Relaxed;
        let target = self.pace_depth;
        if target == 0 {
            return;
        }
        let depth = self.tdw_pace_q.len();
        debug.jitter_depth.store(depth as u64, Relaxed);
        // The display refreshes far faster than 60fps (e.g. 240Hz), so advance the GAME
        // at ~60fps BY TIME, not once-per-render. Between ticks the render loop re-presents
        // the current frame (held 240/60 = 4 refreshes, evenly) -> smooth motion.
        if let Some(t) = self.next_release {
            if now < t {
                return; // not yet time for the next game frame
            }
        }
        // Prime the cushion first so arrival jitter (p95 41ms) can't underrun the grid.
        if !self.pace_primed {
            if depth >= target {
                self.pace_primed = true;
            } else {
                return;
            }
        }
        match self.tdw_pace_q.pop_front() {
            Some(fr) => {
                self.apply_tdw_frame(fr, debug);
                // PLL: hold the buffer near `target` by nudging the release INTERVAL, not by
                // dropping frames. Too full -> release a hair faster (drain the excess); too
                // empty -> a hair slower (build). Base 60fps (16.67ms), +/-1.2ms per frame of
                // error, clamped 50-83fps. Smooth speed changes read far better than drops,
                // and latency self-limits to ~target frames without the old 500ms balloon.
                let err = ((depth as i64 - 1) - target as i64).clamp(-4, 4);
                let interval_us = (16_667i64 - err * 1_200).clamp(12_000, 20_000) as u64;
                let interval = std::time::Duration::from_micros(interval_us);
                // schedule on the running grid; resync after a long stall so we don't
                // machine-gun a burst to catch up.
                let base = match self.next_release {
                    Some(t) if now <= t + std::time::Duration::from_millis(50) => t,
                    _ => now,
                };
                self.next_release = Some(base + interval);
            }
            None => {
                self.pace_primed = false; // underrun -> re-prime, hold the last frame
            }
        }
    }

    pub fn ta(&self) -> &[u8] {
        &self.prev_ta[..self.prev_ta_size.min(self.prev_ta.len())]
    }

    /// Apply a ZCST message ONLY if it's the one-time SYNC keyframe (the VRAM/PVR
    /// base the client requests on connect). Legacy ZCST deltas are ignored —
    /// per-frame rendering runs off the thin ZCS2 wire (apply_delta_frame).
    pub fn apply_sync_zcst(&mut self, msg: &[u8]) -> Result<bool, BoxErr> {
        let inner = decompress_zcst(msg)?;
        if inner.len() < 4 {
            return Ok(false);
        }
        match &inner[0..4] {
            b"SYNC" => {
                self.apply_sync(&inner)?;
                Ok(true)
            }
            b"FSYN" => {
                self.apply_fsyn(&inner)?;
                Ok(true)
            }
            _ => Ok(false), // SAVE or legacy delta -> ignored (thin wire renders from ZCS2)
        }
    }

    /// SYNC: 'SYNC'(4) + vramSize(u32) + vram + pvrSize(u32) + pvr.
    fn apply_sync(&mut self, b: &[u8]) -> Result<(), BoxErr> {
        let vram_size = rd_u32(b, 4)? as usize;
        let voff = 8;
        let poff = voff + vram_size;
        if poff + 4 > b.len() || vram_size > VRAM_SIZE {
            return Err("SYNC: vram overrun".into());
        }
        self.vram[..vram_size].copy_from_slice(&b[voff..poff]);
        let pvr_size = rd_u32(b, poff)? as usize;
        let pstart = poff + 4;
        if pstart + pvr_size > b.len() || pvr_size > PVR_SIZE {
            return Err("SYNC: pvr overrun".into());
        }
        self.pvr_regs[..pvr_size].copy_from_slice(&b[pstart..pstart + pvr_size]);
        self.has_prev_ta = false;
        self.have_sync = true;
        self.vcache.clear();
        log::info!("[frame] SYNC applied: vram={vram_size}B pvr={pvr_size}B");
        Ok(())
    }

    /// FSYN: 'FSYN' + pad(2) + recordCount u16 + recordCount×{tag[4], recSize u32, bytes}.
    fn apply_fsyn(&mut self, b: &[u8]) -> Result<(), BoxErr> {
        let count = rd_u16(b, 6)? as usize;
        let mut off = 8;
        for _ in 0..count {
            if off + 8 > b.len() {
                break;
            }
            let tag = &b[off..off + 4];
            let sz = rd_u32(b, off + 4)? as usize;
            let data_off = off + 8;
            if data_off + sz > b.len() {
                break;
            }
            match tag {
                b"VRAM" if sz <= VRAM_SIZE => self.vram[..sz].copy_from_slice(&b[data_off..data_off + sz]),
                b"PREG" if sz <= PVR_SIZE => self.pvr_regs[..sz].copy_from_slice(&b[data_off..data_off + sz]),
                _ => {}
            }
            off = data_off + sz;
        }
        self.has_prev_ta = false;
        self.have_sync = true;
        self.vcache.clear();
        log::info!("[frame] FSYN applied: {count} records");
        Ok(())
    }

    /// The inner delta frame (from the ZCS2 wire, or a ZCST delta): 80B header +
    /// TA section + checksum + dirty pages. Reconstructs the persistent TA + VRAM.
    pub fn apply_delta_frame(&mut self, f: &[u8]) -> Result<(), BoxErr> {
        if f.len() < 80 {
            return Err("delta: short header".into());
        }
        let frame_num = rd_u32(f, 4)?;
        for i in 0..16 {
            self.pvr_snapshot[i] = rd_u32(f, 8 + i * 4)?;
        }
        let ta_size = rd_u32(f, 72)? as usize;
        let delta_payload = rd_u32(f, 76)? as usize;
        let mut off = 80usize;

        // --- TA section (keyframe / delta / drop) ---
        if delta_payload == ta_size {
            // keyframe: the section IS the whole TA buffer
            if off + ta_size > f.len() {
                return Err("delta: TA keyframe overrun".into());
            }
            self.prev_ta.clear();
            self.prev_ta.extend_from_slice(&f[off..off + ta_size]);
            self.prev_ta_size = ta_size;
            self.has_prev_ta = true;
            off += delta_payload;
        } else if !self.has_prev_ta {
            // no base to patch onto -> can't render this frame's TA
            off += delta_payload;
        } else {
            // delta: run-patch prev_ta in place
            if self.prev_ta.len() < ta_size {
                self.prev_ta.resize(ta_size, 0);
            }
            self.prev_ta_size = ta_size;
            let end = (off + delta_payload).min(f.len());
            let mut p = off;
            while p + 6 <= end {
                let doff = rd_u32(f, p)? as usize;
                p += 4;
                if doff == 0xFFFF_FFFF {
                    break;
                }
                let run = rd_u16(f, p)? as usize;
                p += 2;
                if p + run <= f.len() && doff + run <= self.prev_ta.len() {
                    self.prev_ta[doff..doff + run].copy_from_slice(&f[p..p + run]);
                }
                p += run;
            }
            off = end;
        }

        // checksum (skipped)
        off += 4;

        self.apply_page_section(f, off);

        self.frame_num = frame_num;
        self.renderable = self.has_prev_ta && self.have_sync;
        Ok(())
    }

    /// Dirty-page section walk (VRAM region 1 / PVR region 3, incl the VCACHE
    /// sentinel encoding): [count-or-0xFFFFFFFF] + entries. Shared by the
    /// legacy/ZCS2 inner frame AND the TDW1 in-band page section (bit4) —
    /// byte-identical layouts by construction.
    pub fn apply_page_section(&mut self, f: &[u8], mut off: usize) -> u32 {
        let mut applied = 0u32;
        let rd = |b: &[u8], o: usize| -> Option<u32> {
            b.get(o..o + 4)
                .map(|s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
        };
        let Some(first) = rd(f, off) else { return 0 };
        off += 4;
        if first == 0xFFFF_FFFF {
            // VCACHE: real count follows; pages carry a hash + optional data
            let Some(real) = rd(f, off) else { return 0 };
            off += 4;
            for _ in 0..real as usize {
                if off + 14 > f.len() {
                    break;
                }
                let region = f[off];
                let page_idx = rd(f, off + 1).unwrap_or(0) as usize;
                let hash_lo = rd(f, off + 5).unwrap_or(0) as u64;
                let hash_hi = rd(f, off + 9).unwrap_or(0) as u64;
                let has_data = f[off + 13];
                off += 14;
                let hash = hash_lo | (hash_hi << 32);
                let mut page = [0u8; PAGE];
                if has_data == 1 {
                    if off + PAGE > f.len() {
                        break;
                    }
                    page.copy_from_slice(&f[off..off + PAGE]);
                    off += PAGE;
                    self.vcache.insert(hash, page);
                } else if let Some(cached) = self.vcache.get(&hash) {
                    page = *cached;
                }
                self.apply_page(region, page_idx, &page);
                applied += 1;
            }
        } else {
            // standard: count × [regionId u8][pageIdx u32][pageData 4096]
            for _ in 0..first as usize {
                if off + 5 + PAGE > f.len() {
                    break;
                }
                let region = f[off];
                let page_idx = rd(f, off + 1).unwrap_or(0) as usize;
                let data_off = off + 5;
                let page = &f[data_off..data_off + PAGE];
                self.apply_page_slice(region, page_idx, page);
                applied += 1;
                off = data_off + PAGE;
            }
        }
        applied
    }

    /// Server switch: drop every per-server assumption (seed, chains, dict TA,
    /// camera, page cache) — renderable again after the new server's SYNC+TDW.
    pub fn reset_for_new_server(&mut self) {
        self.has_prev_ta = false;
        self.prev_ta_size = 0;
        self.have_sync = false;
        self.renderable = false;
        self.tdw_ta = None;
        self.tdw_cam = None;
        self.gsta = None;
        self.vcache.clear();
    }

    /// TDW-only mode: mark renderable from the TDW feed (the ZCS2 chain is no
    /// longer applied — TDW carries geometry, camera, AND pages; the seed still
    /// arrives via the one-shot SYNC at connect).
    pub fn mark_tdw_frame(&mut self, frame_num: u32) {
        self.frame_num = frame_num;
        self.renderable = self.have_sync && self.tdw_ta.is_some();
    }

    /// TDW test mode (MC_TDW=render): swap the dict-wire-reassembled TA in as
    /// the render source AND the base the next ZCS2 delta patches. Safe only
    /// because the TDW TA is byte-identical to the chain TA (gated live).
    pub fn replace_ta(&mut self, ta: &[u8]) {
        self.prev_ta.clear();
        self.prev_ta.extend_from_slice(ta);
        self.prev_ta_size = ta.len();
        self.has_prev_ta = true;
    }

    fn apply_page(&mut self, region: u8, page_idx: usize, page: &[u8; PAGE]) {
        self.apply_page_slice(region, page_idx, page);
    }

    fn apply_page_slice(&mut self, region: u8, page_idx: usize, page: &[u8]) {
        let o = page_idx * PAGE;
        match region {
            1 if o + PAGE <= self.vram.len() => self.vram[o..o + PAGE].copy_from_slice(page),
            3 if o + PAGE <= self.pvr_regs.len() => self.pvr_regs[o..o + PAGE].copy_from_slice(page),
            _ => {}
        }
    }
}

/// ZCST envelope: "ZCST"(4) + uncompressedSize(u32 LE) + zstd blob -> raw bytes.
fn decompress_zcst(msg: &[u8]) -> Result<Vec<u8>, BoxErr> {
    if msg.len() < 8 || &msg[0..4] != b"ZCST" {
        return Err("not a ZCST message".into());
    }
    Ok(zstd::stream::decode_all(&msg[8..])?)
}

fn rd_u32(b: &[u8], o: usize) -> Result<u32, BoxErr> {
    b.get(o..o + 4)
        .map(|s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
        .ok_or_else(|| "rd_u32 OOB".into())
}
fn rd_u16(b: &[u8], o: usize) -> Result<u16, BoxErr> {
    b.get(o..o + 2)
        .map(|s| u16::from_le_bytes([s[0], s[1]]))
        .ok_or_else(|| "rd_u16 OOB".into())
}
