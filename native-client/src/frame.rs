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
        }
    }

    pub fn ta(&self) -> &[u8] {
        &self.prev_ta[..self.prev_ta_size.min(self.prev_ta.len())]
    }

    /// Rough sanity metric: how many KB of VRAM are nonzero (proves SYNC landed).
    pub fn vram_nonzero_kb(&self) -> usize {
        self.vram.iter().filter(|&&b| b != 0).count() / 1024
    }

    /// A ZCST envelope: "ZCST"(4) + uncompressedSize(u32 LE) + zstd blob.
    pub fn apply_zcst(&mut self, msg: &[u8]) -> Result<(), BoxErr> {
        let inner = crate::decode::decompress_zcst(msg)?;
        if inner.len() < 4 {
            return Ok(());
        }
        match &inner[0..4] {
            b"SYNC" => self.apply_sync(&inner),
            b"FSYN" => self.apply_fsyn(&inner),
            b"SAVE" => Ok(()), // no-op passthrough
            _ => self.apply_delta(&inner), // legacy delta frame (starts with frameSize u32)
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

    /// Legacy delta frame: 80B header + TA section + checksum + dirty pages.
    fn apply_delta(&mut self, f: &[u8]) -> Result<(), BoxErr> {
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

        // --- dirty pages (VRAM region 1 / PVR region 3), incl VCACHE sentinel ---
        if off + 4 <= f.len() {
            let first = rd_u32(f, off)?;
            off += 4;
            if first == 0xFFFF_FFFF {
                // VCACHE: real count follows; pages carry a hash + optional data
                if off + 4 > f.len() {
                    // header only, nothing to apply
                } else {
                    let real = rd_u32(f, off)? as usize;
                    off += 4;
                    for _ in 0..real {
                        if off + 14 > f.len() {
                            break;
                        }
                        let region = f[off];
                        let page_idx = rd_u32(f, off + 1)? as usize;
                        let hash_lo = rd_u32(f, off + 5)? as u64;
                        let hash_hi = rd_u32(f, off + 9)? as u64;
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
                    }
                }
            } else {
                // standard: count × [regionId u8][pageIdx u32][pageData 4096]
                for _ in 0..first as usize {
                    if off + 5 + PAGE > f.len() {
                        break;
                    }
                    let region = f[off];
                    let page_idx = rd_u32(f, off + 1)? as usize;
                    let data_off = off + 5;
                    let page = &f[data_off..data_off + PAGE];
                    self.apply_page_slice(region, page_idx, page);
                    off = data_off + PAGE;
                }
            }
        }

        self.frame_num = frame_num;
        self.renderable = self.has_prev_ta && self.have_sync;
        Ok(())
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
