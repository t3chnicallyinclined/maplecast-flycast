// Browser TDW decoder — thin stateful raw-ABI wrapper over the shipping tdw.rs.
//
// Single-threaded (wasm): pointers returned by tdw_feed stay valid until the NEXT
// tdw_feed/tdw_snapshot call. JS must copy ta/pages/pvr out before calling again.
// Memory may grow during a feed, so JS must re-view memory.buffer AFTER the call.
//
// tdw.rs's feed() dispatches BOTH 'TDW1' (streaming zstd, reliable transport) and
// 'TDW2' (ACK-reference keyframe/delta, loss-tolerant — for datagram/WebTransport),
// so this one entry point covers every TDW variant.

#[path = "../../native-client-tdw/src/tdw.rs"]
mod tdw;

use tdw::{Tdw, TdwFrame};

pub struct Dec {
    t: Tdw,
    last: Option<TdwFrame>,
    inbuf: Vec<u8>,
    hdr: [u32; 16],
}

#[no_mangle]
pub extern "C" fn tdw_new() -> *mut Dec {
    Box::into_raw(Box::new(Dec {
        t: Tdw::new(),
        last: None,
        inbuf: Vec::new(),
        hdr: [0u32; 16],
    }))
}

/// Ensure the internal input buffer holds >= len bytes; return its pointer.
/// JS writes the raw message bytes there, then calls tdw_feed / tdw_snapshot.
#[no_mangle]
pub extern "C" fn tdw_inbuf(d: *mut Dec, len: usize) -> *mut u8 {
    let d = unsafe { &mut *d };
    if d.inbuf.len() < len {
        d.inbuf.resize(len, 0);
    }
    d.inbuf.as_mut_ptr()
}

/// Feed a 'TDWS' dictionary snapshot (first `len` bytes of the input buffer).
/// Returns 0 on success, -1 on error.
#[no_mangle]
pub extern "C" fn tdw_snapshot(d: *mut Dec, len: usize) -> i32 {
    let d = unsafe { &mut *d };
    let msg = d.inbuf[..len].to_vec();
    match d.t.feed_snapshot(&msg) {
        Ok(()) => 0,
        Err(_) => -1,
    }
}

/// Feed a 'TDW1' or 'TDW2' frame (first `len` bytes of the input buffer). Returns
/// a pointer to a 16-word header, or null if no frame decoded this message.
/// hdr: [has_frame, frame_num, ta_ptr, ta_len, pages_ptr, pages_len, has_pvr,
///       pvr_ptr(16 u32), 0..].
#[no_mangle]
pub extern "C" fn tdw_feed(d: *mut Dec, len: usize) -> *const u32 {
    let d = unsafe { &mut *d };
    let msg = d.inbuf[..len].to_vec();
    match d.t.feed(&msg) {
        Ok(Some(fr)) => {
            d.last = Some(fr);
            let f = d.last.as_ref().unwrap();
            let ta_ptr = f.ta.as_ptr() as u32;
            let ta_len = f.ta.len() as u32;
            let (pg_ptr, pg_len) = match &f.pages {
                Some(p) => (p.as_ptr() as u32, p.len() as u32),
                None => (0, 0),
            };
            let (has_pvr, pvr_ptr) = match &f.pvr {
                Some(a) => (1u32, a.as_ptr() as u32),
                None => (0, 0),
            };
            let fnum = f.frame_num;
            d.hdr = [1, fnum, ta_ptr, ta_len, pg_ptr, pg_len, has_pvr, pvr_ptr, 0, 0, 0, 0, 0, 0, 0, 0];
            d.hdr.as_ptr()
        }
        _ => std::ptr::null(),
    }
}

// --- telemetry / TDW2 ACK ---

#[no_mangle]
pub extern "C" fn tdw_dict_len(d: *mut Dec) -> u32 {
    let d = unsafe { &mut *d };
    d.t.dict_len() as u32
}

#[no_mangle]
pub extern "C" fn tdw_synced(d: *mut Dec) -> u32 {
    let d = unsafe { &mut *d };
    d.t.is_synced() as u32
}

/// TDW2 ACK-reference: the highest frameId decoded so far (0xFFFFFFFF = none).
/// The browser sends this back so the server keeps referenced frames in its ring.
#[no_mangle]
pub extern "C" fn tdw_ack_frame(d: *mut Dec) -> u32 {
    let d = unsafe { &mut *d };
    d.t.tdw2_ack_frame().unwrap_or(0xFFFF_FFFF)
}
