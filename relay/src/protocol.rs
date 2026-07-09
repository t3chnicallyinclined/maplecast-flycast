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

/// Audio packet header bytes. Raw 16-bit stereo PCM over the same WebSocket
/// as TA mirror frames. Packet layout:
///   [0xAD][0x10][seqHi][seqLo][512 × int16 stereo PCM]  = 2052 bytes
/// The relay must distinguish audio from video so per-frame metrics (FPS,
/// jitter, bytes/frame) aren't polluted by the ~86 audio packets/sec.
pub const AUDIO_MAGIC_0: u8 = 0xAD;
pub const AUDIO_MAGIC_1: u8 = 0x10;

/// State-replica frame magics. A client in "state" subscription mode receives
/// ONLY these — GSTA (per-frame game state), OBJF (full object pool), and MCSV
/// (mid-match join savestate). Every TA/VRAM/SYNC/audio "video" frame is dropped
/// at the relay for such clients, cutting per-client egress from ~510 KB/s to the
/// ~10-30 KB/s these three packet types occupy.
pub const GSTA_MAGIC: &[u8; 4] = b"GSTA";
pub const OBJF_MAGIC: &[u8; 4] = b"OBJF";
pub const MCSV_MAGIC: &[u8; 4] = b"MCSV";

/// STAF (stripped-TA full-frame geometry) + TX64 (ship-once decoded texture)
/// magics. These are a PARALLEL render channel (docs/STRIPPED-TA-DESIGN.md §6),
/// NOT the dirty-page delta wire. STAF rides ZCST compression (so its outer
/// wire magic is ZCST and its decompressed payload starts with "STAF"); TX64 is
/// shipped raw (outer magic "TX64"). Neither carries a dirty-page list, so the
/// relay must forward them VERBATIM and never feed them to apply_dirty_pages —
/// doing so reads frame[76] (which is STAF's polyCount, not delta_payload_size)
/// and corrupts the cached SYNC VRAM.
pub const STAF_MAGIC: &[u8; 4] = b"STAF";
pub const TX64_MAGIC: &[u8; 4] = b"TX64";

/// True if the payload (decompressed if needed) is a STAF geometry frame.
/// `inspect` is the decompressed view when the wire was ZCST-compressed.
pub fn is_staf(inspect: &[u8]) -> bool {
    inspect.len() >= 4 && &inspect[0..4] == STAF_MAGIC
}

/// True if the RAW wire bytes are a TX64 ship-once texture packet (uncompressed).
pub fn is_tx64(data: &[u8]) -> bool {
    data.len() >= 4 && &data[0..4] == TX64_MAGIC
}

/// True for the state-replica keep-list (GSTA / OBJF / MCSV). Keep-list, not
/// drop-list: anything we don't recognize is treated as video and dropped for
/// state-only subscribers, so a future video packet type can't silently inflate
/// their bandwidth. The state-replica client ignores everything else anyway.
pub fn is_state_frame(data: &[u8]) -> bool {
    if data.len() < 4 {
        return false;
    }
    let m = &data[0..4];
    m == GSTA_MAGIC || m == OBJF_MAGIC || m == MCSV_MAGIC
}

/// Check if a message is a SYNC frame (uncompressed only)
pub fn is_sync(data: &[u8]) -> bool {
    data.len() >= 4 && &data[0..4] == SYNC_MAGIC
}

/// Check if a message is a ZCST-compressed frame
pub fn is_compressed(data: &[u8]) -> bool {
    data.len() >= 4 && &data[0..4] == ZCST_MAGIC
}

/// Check if a message is an audio PCM packet. Fast path — just two byte reads.
pub fn is_audio(data: &[u8]) -> bool {
    data.len() >= 4 && data[0] == AUDIO_MAGIC_0 && data[1] == AUDIO_MAGIC_1
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
    page_cache: &mut std::collections::HashMap<u64, Vec<u8>>,
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

    let mut dirty_count =
        u32::from_le_bytes([frame[off], frame[off + 1], frame[off + 2], frame[off + 3]]) as usize;
    off += 4;

    // VCACHE (wire-v2, MAPLECAST_VCACHE): content-addressed pages. Sentinel count
    // 0xFFFFFFFF, then the real count; each entry = regionId(1) + pageIdx(4) +
    // contentHash(8 LE) + hasData(1) + [4096 bytes iff hasData]. hasData=0 is a
    // reference resolved from page_cache (filled by earlier hasData=1 entries;
    // the server reseeds content every VCACHE_RESEED_FRAMES, so a post-clear miss
    // self-heals within that window). Mirrors web/webgpu/frame-decoder.mjs.
    let vcache = dirty_count == 0xFFFF_FFFF;
    if vcache {
        if off + 4 > frame.len() {
            return 0;
        }
        dirty_count =
            u32::from_le_bytes([frame[off], frame[off + 1], frame[off + 2], frame[off + 3]]) as usize;
        off += 4;
        // Growth cap: a long session can accumulate 100k+ unique pages. Clearing
        // forces ref-misses only until the server's periodic reseed.
        if page_cache.len() > 20_000 {
            page_cache.clear();
        }
    }

    fn apply_page(region_id: u8, page_offset: usize, data: &[u8], vram: &mut [u8], pvr: &mut [u8]) -> bool {
        match region_id {
            1 if page_offset + PAGE_SIZE <= vram.len() => {
                vram[page_offset..page_offset + PAGE_SIZE].copy_from_slice(data);
                true
            }
            3 if page_offset + PAGE_SIZE <= pvr.len() => {
                pvr[page_offset..page_offset + PAGE_SIZE].copy_from_slice(data);
                true
            }
            _ => false,
        }
    }

    let mut applied = 0;
    for _ in 0..dirty_count {
        if off + 5 > frame.len() {
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

        if vcache {
            if off + 9 > frame.len() {
                break;
            }
            let h_lo = u32::from_le_bytes([frame[off], frame[off + 1], frame[off + 2], frame[off + 3]]) as u64;
            let h_hi = u32::from_le_bytes([frame[off + 4], frame[off + 5], frame[off + 6], frame[off + 7]]) as u64;
            let hash = h_lo | (h_hi << 32);
            let has_data = frame[off + 8];
            off += 9;
            if has_data != 0 {
                if off + PAGE_SIZE > frame.len() {
                    break;
                }
                page_cache.insert(hash, frame[off..off + PAGE_SIZE].to_vec());
                if apply_page(region_id, page_offset, &frame[off..off + PAGE_SIZE], vram, pvr) {
                    applied += 1;
                }
                off += PAGE_SIZE;
            } else if let Some(p) = page_cache.get(&hash) {
                if apply_page(region_id, page_offset, p, vram, pvr) {
                    applied += 1;
                }
            }
        } else {
            if off + PAGE_SIZE > frame.len() {
                break;
            }
            if apply_page(region_id, page_offset, &frame[off..off + PAGE_SIZE], vram, pvr) {
                applied += 1;
            }
            off += PAGE_SIZE;
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
