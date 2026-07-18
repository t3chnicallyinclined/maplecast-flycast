//! TDW dict-wire decode (docs/TA-DICT-WIRE-PLAN.md §2) — TEST CLONE ONLY.
//!
//! TDW1: 'TDW1'(4) dictEpoch(1) flags(1: bit0 streamStart, bit1 canonMasked)
//!       seq(u16 LE) innerSize(u32 LE) + one streaming-zstd chunk (persistent
//!       window, flushed per message — same discipline as ZCS2).
//!   inner: frameNum u32, vframe u32, taSize u32, nBlocks u32, newSection u32,
//!          refs u32×nBlocks, newBlocks repeat{ u8 len(32|64), len bytes }.
//!   Decoder rule: ref id == dict.len() consumes the next new block (append),
//!   id < len() reuses, id > len() = desync. Concat of emitted blocks = the TA.
//! TDWS: 'TDWS'(4) + uncompressedSize u32 LE + zstd blob (one-shot). Inner:
//!   'TDWS'(4) dictEpoch(1) pad(3) blockCount u32 sectionBytes u32 +
//!   repeat{ u8 len, len bytes } — the dictionary in id order.
//!
//! Sync rules mirror the Python gate (_bwlab/tadict_gate_live.py): a TDWS for
//! epoch E arms the dict; decoding starts at the next TDW1(E) with bit0 set;
//! a seq gap or epoch change desyncs until the next TDWS+streamStart pair.

use zstd_safe::{DCtx, InBuffer, OutBuffer, ResetDirective};

type BoxErr = Box<dyn std::error::Error + Send + Sync>;

pub struct Tdw {
    dctx: DCtx<'static>,
    dict: Vec<Vec<u8>>,
    dict_bytes: usize,
    dict_epoch: Option<u8>,
    seq: u16,
    synced: bool,
    acc: Vec<u8>,
    scratch: Vec<u8>,
    /// content-hash index (FNV64, same as the server) — needed for the
    /// split-position TWIN-INTERN mirror: on interning a new pt7 64B literal,
    /// both sides also intern its zeroed-positions twin IF ABSENT, keeping the
    /// id spaces identical with no extra wire.
    by_hash: std::collections::HashMap<u64, Vec<u32>>,
}

fn fnv64(p: &[u8]) -> u64 {
    let mut h: u64 = 1469598103934665603;
    for &b in p {
        h ^= b as u64;
        h = h.wrapping_mul(1099511628211);
    }
    h
}

pub struct TdwFrame {
    pub frame_num: u32,
    #[allow(dead_code)]
    pub vframe: u32,
    pub ta: Vec<u8>,
    /// The decompressed per-frame inner payload (before ref-expansion) — the blob
    /// the keyframe/delta TDW wire dirty-diffs. Populated for the offline gate.
    #[allow(dead_code)]
    pub inner: Vec<u8>,
    /// In-band camera (envelope flags bit3): stage_id + M2(16f) + M1(16f),
    /// the raw RAM dwords in address order — feeds the local stage reprojection.
    pub cam: Option<(u32, [f32; 16], [f32; 16])>,
    /// In-band page section (envelope flags bit4): the legacy dirty-page
    /// layout verbatim — ONE PROTOCOL: textures ride TDW1 too.
    pub pages: Option<Vec<u8>>,
    /// PVR reg snapshot (envelope flags bit5, 64B after the camera) — the
    /// renderer's projection matrix reads reg[0].
    pub pvr: Option<[u32; 16]>,
    /// E2E probe echo (self-locating 'E2EP' inner tail): latched client input
    /// seqs for slots 0/1 — press->present without any ZCS2 dependency.
    pub e2e: Option<(u32, u32)>,
}

impl Tdw {
    pub fn new() -> Self {
        Self {
            dctx: DCtx::create(),
            dict: Vec::new(),
            dict_bytes: 0,
            dict_epoch: None,
            seq: 0,
            synced: false,
            acc: Vec::new(),
            scratch: vec![0u8; 1 << 20],
            by_hash: std::collections::HashMap::new(),
        }
    }

    fn find_block(&self, p: &[u8]) -> Option<u32> {
        self.by_hash
            .get(&fnv64(p))?
            .iter()
            .copied()
            .find(|&id| self.dict[id as usize] == p)
    }

    fn push_block(&mut self, b: Vec<u8>) {
        let id = self.dict.len() as u32;
        self.by_hash.entry(fnv64(&b)).or_default().push(id);
        self.dict_bytes += b.len();
        self.dict.push(b);
    }

    /// The split-position twin rule (server: twinInternIfVertex) — MUST match
    /// byte-for-byte or the id spaces diverge.
    fn twin_intern_if_vertex(&mut self, p: &[u8]) {
        if p.len() != 64 || (p[3] >> 5) & 7 != 7 {
            return;
        }
        let mut shape = p.to_vec();
        shape[4..48].fill(0);
        if shape != p && self.find_block(&shape).is_none() {
            self.push_block(shape);
        }
    }

    // --- telemetry getters (F1 overlay) ---
    pub fn dict_len(&self) -> usize { self.dict.len() }
    pub fn dict_bytes(&self) -> usize { self.dict_bytes }
    pub fn epoch(&self) -> Option<u8> { self.dict_epoch }
    pub fn is_synced(&self) -> bool { self.synced }

    /// 'TDWS' dictionary snapshot: replaces the dict; decoding resumes at the
    /// next streamStart TDW1 of the same epoch.
    pub fn feed_snapshot(&mut self, msg: &[u8]) -> Result<(), BoxErr> {
        if msg.len() < 8 || &msg[0..4] != b"TDWS" {
            return Err("not a TDWS message".into());
        }
        let inner = zstd::stream::decode_all(&msg[8..])?;
        if inner.len() < 16 || &inner[0..4] != b"TDWS" {
            return Err("TDWS inner malformed".into());
        }
        let epoch = inner[4];
        let n_blk = rd_u32(&inner, 8)? as usize;
        let sec_b = rd_u32(&inner, 12)? as usize;
        let mut p = 16usize;
        let mut dict = Vec::with_capacity(n_blk);
        for _ in 0..n_blk {
            let len = *inner.get(p).ok_or("TDWS: len OOB")? as usize;
            p += 1;
            let blk = inner.get(p..p + len).ok_or("TDWS: block OOB")?;
            dict.push(blk.to_vec());
            p += len;
        }
        if p != 16 + sec_b {
            return Err(format!("TDWS: section {p} != {}", 16 + sec_b).into());
        }
        self.dict.clear();
        self.dict_bytes = 0;
        self.by_hash.clear();
        for b in dict {
            self.push_block(b);   // rebuilds the content index (twin lookup)
        }
        self.dict_epoch = Some(epoch);
        self.synced = false; // need a streamStart TDW1 to (re)enter the zstd stream
        log::info!("[tdw] TDWS applied: epoch={epoch} blocks={n_blk}");
        Ok(())
    }

    /// Feed one 'TDW1' message; returns the reassembled full TA when decodable.
    pub fn feed(&mut self, msg: &[u8]) -> Result<Option<TdwFrame>, BoxErr> {
        if msg.len() < 12 || &msg[0..4] != b"TDW1" {
            return Ok(None);
        }
        let epoch = msg[4];
        let flags = msg[5];
        let seq = u16::from_le_bytes([msg[6], msg[7]]);
        let inner_size = rd_u32(msg, 8)? as usize;

        if self.dict_epoch != Some(epoch) {
            return Ok(None); // dictionary unknown for this epoch — wait for TDWS
        }
        if flags & 0x01 != 0 {
            self.dctx.reset(ResetDirective::SessionOnly).map_err(zerr)?;
            self.acc.clear();
            self.seq = seq;
            self.synced = true;
        } else {
            if !self.synced {
                return Ok(None);
            }
            if seq != self.seq.wrapping_add(1) {
                log::warn!("[tdw] seq gap {seq} != {} — desync until next restart", self.seq.wrapping_add(1));
                self.synced = false;
                self.acc.clear();
                return Ok(None);
            }
            self.seq = seq;
        }

        // streaming decompress this message's chunk
        let comp = &msg[12..];
        let mut inb = InBuffer::around(comp);
        loop {
            let produced;
            {
                let mut outb = OutBuffer::around(&mut self.scratch);
                self.dctx.decompress_stream(&mut outb, &mut inb).map_err(zerr)?;
                produced = outb.pos();
            }
            self.acc.extend_from_slice(&self.scratch[..produced]);
            if inb.pos() >= comp.len() || produced == 0 {
                break;
            }
        }
        if self.acc.len() < inner_size {
            return Ok(None);
        }
        if self.acc.len() != inner_size {
            let got = self.acc.len();
            self.synced = false;
            self.acc.clear();
            return Err(format!("TDW1 chunk overrun: {got} != {inner_size}").into());
        }
        let inner = std::mem::take(&mut self.acc);

        // --- inner payload -> reassemble the TA from dict refs ---
        if inner.len() < 20 {
            return Err("TDW1 inner: short".into());
        }
        let frame_num = rd_u32(&inner, 0)?;
        let vframe = rd_u32(&inner, 4)?;
        let ta_size = rd_u32(&inner, 8)? as usize;
        let n_blocks = rd_u32(&inner, 12)? as usize;
        let new_section = rd_u32(&inner, 16)? as usize;
        let mut cam = None;
        let mut refs = 20usize;
        if flags & 8 != 0 {
            let sid = rd_u32(&inner, 20)?;
            let rdf = |o: usize| -> Result<[f32; 16], BoxErr> {
                let mut m = [0f32; 16];
                for i in 0..16 {
                    m[i] = f32::from_bits(rd_u32(&inner, o + 4 * i)?);
                }
                Ok(m)
            };
            cam = Some((sid, rdf(24)?, rdf(88)?));   // M2 @ +24, M1 @ +88
            refs += 132;
        }
        let mut pvr = None;
        if flags & 32 != 0 {
            let mut p = [0u32; 16];
            for (i, slot) in p.iter_mut().enumerate() {
                *slot = rd_u32(&inner, refs + 4 * i)?;
            }
            pvr = Some(p);
            refs += 64;
        }
        let news = refs + 4 * n_blocks;
        let news_end = news + new_section;
        if news_end > inner.len() {
            self.synced = false;
            return Err("TDW1 inner: sections OOB".into());
        }
        // split-position section (flags bit6): u32 len + raw 44B position runs,
        // consumed in shape-ref order (refs with the top bit set).
        let split = flags & 64 != 0;
        let (pos_start, sections_end) = if split {
            let pl = rd_u32(&inner, news_end)? as usize;
            let ps = news_end + 4;
            if ps + pl > inner.len() {
                self.synced = false;
                return Err("TDW1: pos section OOB".into());
            }
            (ps, ps + pl)
        } else {
            (news_end, news_end)
        };
        let mut np = news;
        let mut pp = pos_start;
        let mut ta = Vec::with_capacity(ta_size);
        for i in 0..n_blocks {
            let raw = rd_u32(&inner, refs + 4 * i)?;
            let shape_ref = split && raw & 0x8000_0000 != 0;
            let id = (raw & 0x7fff_ffff) as usize;
            if shape_ref {
                // known shape + fresh positions: splice bytes 4..48
                if id >= self.dict.len() {
                    self.synced = false;
                    return Err(format!("TDW1: shape ref {id} > dict {}", self.dict.len()).into());
                }
                let pos = inner.get(pp..pp + 44).ok_or("TDW1: pos run OOB")?;
                pp += 44;
                let base = &self.dict[id];
                let mut blk = base.clone();
                blk[4..48].copy_from_slice(pos);
                ta.extend_from_slice(&blk);
                continue;
            }
            if id == self.dict.len() {
                let len = *inner.get(np).ok_or("TDW1: new len OOB")? as usize;
                np += 1;
                let blk = inner.get(np..np + len).ok_or("TDW1: new block OOB")?.to_vec();
                np += len;
                self.push_block(blk);
                if split {
                    let lit = self.dict[id].clone();
                    self.twin_intern_if_vertex(&lit);   // mirror the server rule
                }
            } else if id > self.dict.len() {
                self.synced = false;
                return Err(format!("TDW1: ref {id} > dict {}", self.dict.len()).into());
            }
            ta.extend_from_slice(&self.dict[id]);
        }
        if np != news_end {
            self.synced = false;
            return Err("TDW1: newBlocks length mismatch".into());
        }
        if split && pp != sections_end {
            self.synced = false;
            return Err(format!("TDW1: pos section mismatch {pp} != {sections_end}").into());
        }
        if ta.len() != ta_size {
            self.synced = false;
            return Err(format!("TDW1: taSize {ta_size} != rebuilt {}", ta.len()).into());
        }
        let pages = if flags & 16 != 0 && inner.len() > sections_end {
            Some(inner[sections_end..].to_vec())
        } else {
            None
        };
        // self-locating E2EP tail (page walk is count-driven, ignores the tail)
        let n = inner.len();
        let e2e = if n >= 32 && &inner[n - 4..] == b"E2EP" {
            Some((
                rd_u32(&inner, n - 12)?,
                rd_u32(&inner, n - 8)?,
            ))
        } else {
            None
        };
        Ok(Some(TdwFrame { frame_num, vframe, ta, inner, cam, pages, pvr, e2e }))
    }
}

fn zerr(code: usize) -> BoxErr {
    format!("zstd: {}", zstd_safe::get_error_name(code)).into()
}
fn rd_u32(b: &[u8], o: usize) -> Result<u32, BoxErr> {
    b.get(o..o + 4)
        .map(|s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
        .ok_or_else(|| "rd_u32 OOB".into())
}
