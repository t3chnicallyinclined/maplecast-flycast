//! ZCS2 thin-wire decode: envelope + persistent streaming-zstd + SoA-inverse + belt.
//!
//! This is the low-bandwidth wire (what webgpu-test.html renders when "ZCS2" is
//! checked). Produces the inner legacy delta frame that FrameDecoder consumes.
//! Ported byte-exact from the browser ZCS2 worker.
//!
//! Envelope: 'ZCS2'(4) epoch(1) flags(1) innerSize(u32 LE)
//!   + optional [cam 132 | vframe 4 | order 1+3n | seq 2] by ascending flag bit
//!   + ONE zstd chunk (one long frame flushed per message; window spans frames).

use zstd_safe::{DCtx, InBuffer, OutBuffer, ResetDirective};

type BoxErr = Box<dyn std::error::Error + Send + Sync>;

pub struct Zcs2 {
    dctx: DCtx<'static>,
    epoch: u8,
    seq: u16,
    synced: bool,
    acc: Vec<u8>,
    scratch: Vec<u8>,
}

impl Zcs2 {
    pub fn new() -> Self {
        Self {
            dctx: DCtx::create(),
            epoch: 0,
            seq: 0,
            synced: false,
            acc: Vec::new(),
            scratch: vec![0u8; 1 << 20],
        }
    }

    /// Feed one ZCS2 message; returns the reconstructed inner frame when complete.
    pub fn feed(&mut self, msg: &[u8]) -> Result<Option<Vec<u8>>, BoxErr> {
        if msg.len() < 10 || &msg[0..4] != b"ZCS2" {
            return Ok(None);
        }
        let epoch = msg[4];
        let flags = msg[5];
        let inner_size = u32::from_le_bytes(msg[6..10].try_into().unwrap()) as usize;

        let cam = if flags & 0x08 != 0 { 132 } else { 0 };
        let vf = if flags & 0x20 != 0 { 4 } else { 0 };
        let ord = if flags & 0x40 != 0 {
            let p = 10 + cam + vf;
            if p >= msg.len() {
                return Ok(None);
            }
            1 + 3 * msg[p] as usize
        } else {
            0
        };
        let seqp = if flags & 0x80 != 0 { 2 } else { 0 };
        let seq_val = if seqp == 2 {
            let o = 10 + cam + vf + ord;
            if o + 2 > msg.len() {
                return Ok(None);
            }
            u16::from_le_bytes([msg[o], msg[o + 1]])
        } else {
            0
        };
        let zoff = 10 + cam + vf + ord + seqp;
        if zoff > msg.len() {
            return Ok(None);
        }

        if flags & 0x01 != 0 {
            // stream-start: fresh session
            self.dctx.reset(ResetDirective::SessionOnly).map_err(zerr)?;
            self.acc.clear();
            self.epoch = epoch;
            self.seq = seq_val;
            self.synced = true;
        } else {
            if !self.synced || epoch != self.epoch {
                self.desync();
                return Ok(None);
            }
            if seqp == 2 {
                if seq_val != self.seq.wrapping_add(1) {
                    self.desync();
                    return Ok(None);
                }
                self.seq = seq_val;
            }
        }

        // streaming decompress this message's chunk (persistent DCtx spans frames)
        let comp = &msg[zoff..];
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
            self.desync();
            return Err(format!("ZCS2 chunk overrun: {got} != {inner_size}").into());
        }
        let inner = std::mem::take(&mut self.acc);

        let fin = if flags & 0x02 != 0 { soa_inverse(&inner)? } else { inner };

        if fin.len() < 4 {
            return Err("ZCS2 belt: short frame".into());
        }
        let fsz = u32::from_le_bytes(fin[0..4].try_into().unwrap()) as usize;
        if fsz + 4 != fin.len() {
            self.synced = false;
            return Err(format!("ZCS2 belt: {fsz}+4 != {}", fin.len()).into());
        }
        Ok(Some(fin))
    }

    fn desync(&mut self) {
        self.synced = false;
        self.acc.clear();
    }
}

fn zerr(code: usize) -> BoxErr {
    format!("zstd: {}", zstd_safe::get_error_name(code)).into()
}

/// Rebuild the interleaved legacy delta section from the SoA-transformed frame.
fn soa_inverse(v: &[u8]) -> Result<Vec<u8>, BoxErr> {
    if v.len() < 84 {
        return Err("SoA: short".into());
    }
    let rd32 = |o: usize| u32::from_le_bytes([v[o], v[o + 1], v[o + 2], v[o + 3]]);
    let rd16 = |o: usize| u16::from_le_bytes([v[o], v[o + 1]]);
    let v2_sec = rd32(76) as usize;
    let n_runs = rd32(80) as usize;
    let offs = 84;
    let lens = 84 + n_runs * 4;
    let dat = lens + n_runs * 2;
    if dat > v.len() {
        return Err("SoA: runs OOB".into());
    }
    let mut data_b = 0usize;
    for i in 0..n_runs {
        data_b += rd16(lens + i * 2) as usize;
    }
    let tail_off = 80 + v2_sec;
    if tail_off > v.len() || dat + data_b > v.len() {
        return Err("SoA: tail/data OOB".into());
    }
    let legacy_sec = n_runs * 6 + data_b + 4;
    let out_len = 80 + legacy_sec + (v.len() - tail_off);
    let mut out = vec![0u8; out_len];
    out[0..80].copy_from_slice(&v[0..80]);
    out[76..80].copy_from_slice(&(legacy_sec as u32).to_le_bytes());
    let mut o = 80usize;
    let mut prev = 0u32;
    let mut d = dat;
    for i in 0..n_runs {
        prev = prev.wrapping_add(rd32(offs + i * 4));
        let rl = rd16(lens + i * 2) as usize;
        out[o..o + 4].copy_from_slice(&prev.to_le_bytes());
        o += 4;
        out[o..o + 2].copy_from_slice(&(rl as u16).to_le_bytes());
        o += 2;
        out[o..o + rl].copy_from_slice(&v[d..d + rl]);
        o += rl;
        d += rl;
    }
    out[o..o + 4].copy_from_slice(&0xFFFF_FFFFu32.to_le_bytes());
    o += 4;
    out[o..].copy_from_slice(&v[tail_off..]);
    Ok(out)
}
