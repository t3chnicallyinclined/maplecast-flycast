//! Frame decode.
//!
//! Fastest path to first pixels = the `ZCST` legacy wire: one-shot zstd, FULL
//! bodies (no CHARSTRIP, no render_frame). Decompresses to the "delta frame"
//! whose layout is documented in CLAUDE.md:
//!   frameSize(4) frameNum(4) pvr_snapshot[16x4](64) taSize(4) deltaPayloadSize(4)
//!   [TA delta payload] checksum(4) dirtyPageCount(4)
//!   [regionId(1) pageIdx(4) pageData(4096)] x N
//! (We migrate to the thin ZCS2 wire once pixels are on screen.)

type BoxErr = Box<dyn std::error::Error + Send + Sync>;

#[derive(Debug, Default)]
pub struct DeltaFrame {
    pub frame_num: u32,
    pub ta_size: u32,             // full (reconstructed) TA size
    pub delta_payload_size: u32,  // bytes of TA delta in this frame
    pub dirty_page_count: u32,
    pub decompressed_len: usize,
}

/// ZCST envelope: "ZCST"(4) + uncompressedSize(u32 LE) + zstd_blob -> raw delta frame.
pub fn decompress_zcst(msg: &[u8]) -> Result<Vec<u8>, BoxErr> {
    if msg.len() < 8 || &msg[0..4] != b"ZCST" {
        return Err("not a ZCST message".into());
    }
    let hint = u32::from_le_bytes(msg[4..8].try_into().unwrap()) as usize;
    let out = zstd::stream::decode_all(&msg[8..])?;
    if out.len() != hint {
        log::warn!("[decode] ZCST size hint {hint} != decompressed {}", out.len());
    }
    Ok(out)
}

/// Parse the delta-frame header fields (validation; full TA reconstruction is next).
pub fn parse_delta_header(f: &[u8]) -> Option<DeltaFrame> {
    if f.len() < 80 {
        return None;
    }
    let rd = |o: usize| u32::from_le_bytes(f[o..o + 4].try_into().unwrap());
    let frame_num = rd(4);
    let ta_size = rd(72); // 8 + pvr_snapshot(64)
    let delta_payload_size = rd(76);
    let after_ta = 80usize.checked_add(delta_payload_size as usize)?;
    let dirty_page_count = if f.len() >= after_ta + 8 {
        rd(after_ta + 4) // skip checksum(4)
    } else {
        0
    };
    Some(DeltaFrame {
        frame_num,
        ta_size,
        delta_payload_size,
        dirty_page_count,
        decompressed_len: f.len(),
    })
}
