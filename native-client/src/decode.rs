//! Low-level decompression helpers.
//!
//! The full frame reconstruction lives in frame.rs (FrameDecoder). This module
//! just owns the ZCST envelope -> zstd step.

type BoxErr = Box<dyn std::error::Error + Send + Sync>;

/// ZCST envelope: "ZCST"(4) + uncompressedSize(u32 LE) + zstd_blob -> raw bytes.
/// The raw bytes are either a SYNC keyframe or a legacy delta frame (see frame.rs).
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
