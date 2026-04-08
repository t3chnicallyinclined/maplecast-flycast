// ============================================================================
// PROTOCOL — Binary frame format for MapleCast TA stream
//
// SYNC frame:  "SYNC"(4) + vramSize(4) + vram(N) + pvrSize(4) + pvr(M)
// Delta frame: frameSize(4) + frameNum(4) + pvr_snapshot(64) + taSize(4)
//              + deltaPayloadSize(4) + ta_data(N) + taChecksum(4)
//              + dirtyCount(4) + [regionId(1) + pageIdx(4) + page(4096)] * N
//
// Compressed envelope (zstd): "ZCST"(4) + uncompressedSize(4) + zstd_blob(N)
//   The decompressed payload is one of the formats above.
//
// All integers little-endian.
// ============================================================================

pub const SYNC_MAGIC: &[u8; 4] = b"SYNC";
pub const ZCST_MAGIC: &[u8; 4] = b"ZCST";
pub const PAGE_SIZE: usize = 4096;
pub const VRAM_SIZE: usize = 8 * 1024 * 1024; // 8MB
pub const PVR_SIZE: usize = 32 * 1024;         // 32KB

/// Check if a message is a SYNC frame (uncompressed only)
pub fn is_sync(data: &[u8]) -> bool {
    data.len() >= 4 && &data[0..4] == SYNC_MAGIC
}

/// Check if a message is a ZCST-compressed frame
pub fn is_compressed(data: &[u8]) -> bool {
    data.len() >= 4 && &data[0..4] == ZCST_MAGIC
}

/// Decompress a ZCST envelope. Returns the decompressed payload bytes.
pub fn decompress(data: &[u8]) -> Option<Vec<u8>> {
    if !is_compressed(data) || data.len() < 8 {
        return None;
    }
    let uncompressed_size = u32::from_le_bytes([data[4], data[5], data[6], data[7]]) as usize;
    match zstd::stream::decode_all(&data[8..]) {
        Ok(out) => {
            if out.len() == uncompressed_size {
                Some(out)
            } else {
                tracing::warn!("zstd decompress size mismatch: expected {} got {}", uncompressed_size, out.len());
                Some(out)
            }
        }
        Err(e) => {
            tracing::warn!("zstd decompress failed: {}", e);
            None
        }
    }
}

/// Returns true if the payload (decompressed if needed) starts with "SYNC".
/// Used by the relay to detect SYNCs whether or not the wire is compressed.
pub fn is_sync_or_compressed_sync(data: &[u8]) -> bool {
    if is_sync(data) {
        return true;
    }
    if is_compressed(data) && data.len() >= 8 {
        // Compressed SYNC has uncompressedSize > 1MB
        let uncompressed_size = u32::from_le_bytes([data[4], data[5], data[6], data[7]]) as usize;
        return uncompressed_size > 1024 * 1024;
    }
    false
}

/// Extract frame number from a delta frame (bytes 4..8, little-endian u32).
/// Returns None for SYNC and compressed frames.
pub fn frame_num(data: &[u8]) -> Option<u32> {
    if data.len() >= 8 && !is_sync(data) && !is_compressed(data) {
        Some(u32::from_le_bytes([data[4], data[5], data[6], data[7]]))
    } else {
        None
    }
}

/// Parse dirty pages from a delta frame and apply them to cached VRAM/PVR.
/// Returns number of pages applied.
pub fn apply_dirty_pages(
    frame: &[u8],
    vram: &mut [u8],
    pvr: &mut [u8],
) -> usize {
    if frame.len() < 80 {
        return 0;
    }

    // Parse header
    let _ta_size = u32::from_le_bytes([frame[72], frame[73], frame[74], frame[75]]) as usize;
    let delta_payload_size =
        u32::from_le_bytes([frame[76], frame[77], frame[78], frame[79]]) as usize;

    // Skip: header(80) + ta_data(delta_payload_size) + checksum(4)
    let mut off = 80 + delta_payload_size + 4;

    if off + 4 > frame.len() {
        return 0;
    }

    let dirty_count =
        u32::from_le_bytes([frame[off], frame[off + 1], frame[off + 2], frame[off + 3]]) as usize;
    off += 4;

    let mut applied = 0;
    for _ in 0..dirty_count {
        if off + 1 + 4 + PAGE_SIZE > frame.len() {
            break;
        }

        let region_id = frame[off];
        off += 1;

        let page_idx = u32::from_le_bytes([
            frame[off],
            frame[off + 1],
            frame[off + 2],
            frame[off + 3],
        ]) as usize;
        off += 4;

        let page_offset = page_idx * PAGE_SIZE;
        let page_data = &frame[off..off + PAGE_SIZE];
        off += PAGE_SIZE;

        match region_id {
            1 => {
                // VRAM
                if page_offset + PAGE_SIZE <= vram.len() {
                    vram[page_offset..page_offset + PAGE_SIZE].copy_from_slice(page_data);
                    applied += 1;
                }
            }
            3 => {
                // PVR regs
                if page_offset + PAGE_SIZE <= pvr.len() {
                    pvr[page_offset..page_offset + PAGE_SIZE].copy_from_slice(page_data);
                    applied += 1;
                }
            }
            _ => {} // unknown region, skip
        }
    }

    applied
}

/// Parse a SYNC frame into (vram_data, pvr_data)
pub fn parse_sync(data: &[u8]) -> Option<(Vec<u8>, Vec<u8>)> {
    if !is_sync(data) || data.len() < 12 {
        return None;
    }

    let mut off = 4; // skip "SYNC"

    let vram_size =
        u32::from_le_bytes([data[off], data[off + 1], data[off + 2], data[off + 3]]) as usize;
    off += 4;

    if off + vram_size + 4 > data.len() {
        return None;
    }
    let vram = data[off..off + vram_size].to_vec();
    off += vram_size;

    let pvr_size =
        u32::from_le_bytes([data[off], data[off + 1], data[off + 2], data[off + 3]]) as usize;
    off += 4;

    if off + pvr_size > data.len() {
        return None;
    }
    let pvr = data[off..off + pvr_size].to_vec();

    Some((vram, pvr))
}

/// Build a SYNC frame from cached VRAM + PVR
pub fn build_sync(vram: &[u8], pvr: &[u8]) -> Vec<u8> {
    let total = 4 + 4 + vram.len() + 4 + pvr.len();
    let mut buf = Vec::with_capacity(total);

    buf.extend_from_slice(SYNC_MAGIC);
    buf.extend_from_slice(&(vram.len() as u32).to_le_bytes());
    buf.extend_from_slice(vram);
    buf.extend_from_slice(&(pvr.len() as u32).to_le_bytes());
    buf.extend_from_slice(pvr);

    buf
}

/// Length-prefix framing for raw TCP: read 4-byte LE length, then payload
pub fn encode_tcp_frame(data: &[u8]) -> Vec<u8> {
    let len = data.len() as u32;
    let mut buf = Vec::with_capacity(4 + data.len());
    buf.extend_from_slice(&len.to_le_bytes());
    buf.extend_from_slice(data);
    buf
}
