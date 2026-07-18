//! G0 — the deterministic loss gate (docs/TDW2-DESIGN.md, build step G0).
//!
//! Replays a captured TDW wire through the REAL decoder (`tdw::Tdw::feed`) twice:
//!   - reference: no loss           -> ground-truth decoded TA per frame
//!   - test:      induced datagram drops -> the lossy decoded TA per frame
//! and byte-diffs the decoded TA frame-for-frame. Byte-exact TA == pixel-exact
//! (both go through the same renderer), so this is a STRONGER, fully-deterministic
//! stand-in for the live pixel diff — no GPU, no network, no flycast, identical
//! result every run.
//!
//! It turns "loss kills the streaming wire" from theory into a measured number,
//! and is the acceptance gate every TDW2 step must pass:
//!   PASS  = zero silent corruption AND zero permanent desync AND every gap
//!           recovers within --recover frames.
//! On the CURRENT streaming-zstd wire it will FAIL loudly (a dropped P-frame
//! desyncs until the next TDWS+streamStart) — that failure IS the baseline fact
//! that motivates TDW2.
//!
//! Capture format (produced by MC_TDW_CAPTURE=<path> in net.rs): a flat file of
//! `[u32 LE len][len bytes]` records, one per binary wire message, in arrival
//! order. Only TDW1/TDWS are fed to the decoder here; other kinds are ignored.
//!
//! Usage:
//!   maplecast-native gate <capture> [--drop N] [--drop-at a,b,..] [--recover R]
//!     --drop N       drop every Nth droppable (streamStart==0) TDW1  [default 30]
//!     --drop-at ...  drop specific droppable-TDW1 ordinals (overrides --drop)
//!     --recover R    max gap frames still counted as PASS             [default 2]
//!     --drop-keyframes  also allow dropping streamStart==1 frames

use crate::tdw::Tdw;
use std::collections::{HashMap, HashSet};

struct Msg {
    kind: [u8; 4],
    bytes: Vec<u8>,
}

fn load_capture(path: &str) -> Vec<Msg> {
    let data = std::fs::read(path).unwrap_or_else(|e| {
        eprintln!("[gate] cannot read capture {path}: {e}");
        std::process::exit(2);
    });
    let mut out = Vec::new();
    let mut p = 0usize;
    while p + 4 <= data.len() {
        let len = u32::from_le_bytes(data[p..p + 4].try_into().unwrap()) as usize;
        p += 4;
        if len == 0 || p + len > data.len() {
            break;
        }
        let bytes = data[p..p + len].to_vec();
        p += len;
        let mut kind = [0u8; 4];
        if bytes.len() >= 4 {
            kind.copy_from_slice(&bytes[0..4]);
        }
        out.push(Msg { kind, bytes });
    }
    out
}

/// One decode pass. `drop(tdw1_ordinal, stream_start) -> true` skips that TDW1
/// (simulating a lost datagram). Returns the decoded frames in decode order,
/// plus per-pass counters.
fn decode(
    msgs: &[Msg],
    mut drop: impl FnMut(usize, bool) -> bool,
) -> (Vec<(u32, Vec<u8>)>, usize, usize, usize) {
    let mut tdw = Tdw::new();
    let mut frames = Vec::new();
    let mut tdw1_ord = 0usize;
    let mut dropped = 0usize;
    let mut errors = 0usize;
    for m in msgs {
        match &m.kind {
            b"TDWS" => {
                let _ = tdw.feed_snapshot(&m.bytes);
            }
            b"TDW1" => {
                let ss = m.bytes.len() > 5 && m.bytes[5] & 1 != 0;
                let this = tdw1_ord;
                tdw1_ord += 1;
                if drop(this, ss) {
                    dropped += 1;
                    continue;
                }
                match tdw.feed(&m.bytes) {
                    Ok(Some(fr)) => frames.push((fr.frame_num, fr.ta)),
                    Ok(None) => {}
                    Err(_) => errors += 1,
                }
            }
            _ => {}
        }
    }
    (frames, tdw1_ord, dropped, errors)
}

pub fn run_gate(args: &[String]) {
    if args.is_empty() {
        eprintln!("usage: maplecast-native gate <capture> [--drop N] [--drop-at a,b,..] [--recover R] [--drop-keyframes]");
        std::process::exit(2);
    }
    let capture = &args[0];
    let mut drop_n: usize = 30;
    let mut drop_at: Option<HashSet<usize>> = None;
    let mut recover: usize = 2;
    let mut drop_keyframes = false;
    let mut dump_inner: Option<String> = None;
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--drop" => {
                drop_n = args.get(i + 1).and_then(|s| s.parse().ok()).unwrap_or(drop_n);
                i += 2;
            }
            "--drop-at" => {
                let set = args
                    .get(i + 1)
                    .map(|s| s.split(',').filter_map(|x| x.trim().parse().ok()).collect())
                    .unwrap_or_default();
                drop_at = Some(set);
                i += 2;
            }
            "--recover" => {
                recover = args.get(i + 1).and_then(|s| s.parse().ok()).unwrap_or(recover);
                i += 2;
            }
            "--drop-keyframes" => {
                drop_keyframes = true;
                i += 1;
            }
            "--dump-inner" => {
                dump_inner = args.get(i + 1).cloned();
                i += 2;
            }
            other => {
                eprintln!("[gate] unknown arg: {other}");
                i += 1;
            }
        }
    }

    let msgs = load_capture(capture);

    // --dump-inner: decode cleanly and write the per-frame INNER payloads (before
    // ref-expansion) as [u32 len][bytes] — the blob the keyframe/delta TDW wire
    // dirty-diffs. Feeds the offline codec prototype (statewire_tdw_kf.py).
    if let Some(path) = dump_inner {
        let mut tdw = Tdw::new();
        let mut out: Vec<u8> = Vec::new();
        let mut n = 0u32;
        for m in &msgs {
            match &m.kind {
                b"TDWS" => { let _ = tdw.feed_snapshot(&m.bytes); }
                b"TDW1" => {
                    if let Ok(Some(fr)) = tdw.feed(&m.bytes) {
                        out.extend_from_slice(&(fr.inner.len() as u32).to_le_bytes());
                        out.extend_from_slice(&fr.inner);
                        n += 1;
                    }
                }
                _ => {}
            }
        }
        std::fs::write(&path, &out).unwrap_or_else(|e| { eprintln!("[gate] write {path}: {e}"); std::process::exit(2); });
        println!("dumped {n} inner payloads ({} B) -> {path}", out.len());
        return;
    }

    let n_tdw1 = msgs.iter().filter(|m| &m.kind == b"TDW1").count();
    let n_tdws = msgs.iter().filter(|m| &m.kind == b"TDWS").count();
    println!("== G0 loss gate ==");
    println!("capture   : {capture}");
    println!("messages  : {} total ({n_tdw1} TDW1, {n_tdws} TDWS)", msgs.len());

    // --- reference: no loss ---
    let (rframes, _, _, rerr) = decode(&msgs, |_, _| false);
    if rframes.is_empty() {
        println!("\nVERDICT: NO REFERENCE FRAMES DECODED — capture has no decodable TDW1");
        println!("(need a TDWS dict snapshot followed by a streamStart TDW1 of the same epoch)");
        std::process::exit(1);
    }
    // last-writer-wins map of the clean decode
    let mut ref_map: HashMap<u32, Vec<u8>> = HashMap::new();
    for (fnum, ta) in &rframes {
        ref_map.insert(*fnum, ta.clone());
    }

    // --- test: induced loss on droppable (streamStart==0) TDW1 frames ---
    let mut planned = 0usize;
    let dropper = |ord: usize, ss: bool| -> bool {
        let eligible = drop_keyframes || !ss;
        if !eligible {
            return false;
        }
        match &drop_at {
            Some(set) => set.contains(&ord),
            None => drop_n > 0 && ord % drop_n == drop_n - 1,
        }
    };
    // count the plan up front (informational)
    {
        let mut ord = 0usize;
        for m in &msgs {
            if &m.kind == b"TDW1" {
                let ss = m.bytes.len() > 5 && m.bytes[5] & 1 != 0;
                if dropper(ord, ss) {
                    planned += 1;
                }
                ord += 1;
            }
        }
    }
    let (tframes, _, dropped, terr) = decode(&msgs, dropper);
    let mut test_map: HashMap<u32, Vec<u8>> = HashMap::new();
    for (fnum, ta) in &tframes {
        test_map.insert(*fnum, ta.clone());
    }

    if drop_at.is_some() {
        println!("drop plan : specific ordinals {:?}", drop_at.as_ref().unwrap());
    } else {
        println!("drop plan : every {drop_n}th droppable TDW1 (streamStart={})", if drop_keyframes { "any" } else { "0 only" });
    }
    println!("dropped   : {dropped} frames (planned {planned})");
    println!("recover R : {recover} frames");
    println!(
        "decoded   : ref {} frames ({rerr} decode errors) | test {} frames ({terr} decode errors)",
        rframes.len(),
        tframes.len()
    );

    // --- walk the reference timeline; classify each frame under loss ---
    let mut corruptions = 0usize; // present in test but WRONG bytes (silent corruption)
    let mut absent = 0usize; // present in ref, missing in test (a dead/skipped frame)
    let mut matched = 0usize;
    let mut in_gap = false;
    let mut gap_len = 0usize;
    let mut gaps: Vec<usize> = Vec::new();
    let mut first_corrupt_frame: Option<u32> = None;
    for (fnum, rta) in &rframes {
        match test_map.get(fnum) {
            Some(tta) if tta == rta => {
                matched += 1;
                if in_gap {
                    gaps.push(gap_len);
                    in_gap = false;
                    gap_len = 0;
                }
            }
            Some(_) => {
                corruptions += 1;
                first_corrupt_frame.get_or_insert(*fnum);
                if in_gap {
                    gap_len += 1;
                }
            }
            None => {
                absent += 1;
                if !in_gap {
                    in_gap = true;
                    gap_len = 0;
                }
                gap_len += 1;
            }
        }
    }
    let ended_in_gap = in_gap; // never recovered before the capture ended
    if in_gap {
        gaps.push(gap_len);
    }
    let worst_gap = gaps.iter().copied().max().unwrap_or(0);
    let recovered: Vec<usize> = if ended_in_gap && !gaps.is_empty() {
        gaps[..gaps.len() - 1].to_vec()
    } else {
        gaps.clone()
    };

    println!("\n-- results (reference timeline of {} frames) --", rframes.len());
    println!("matched (byte-exact) : {matched}");
    println!("SILENT CORRUPTION    : {corruptions}{}", first_corrupt_frame.map(|f| format!("  (first at frame {f})")).unwrap_or_default());
    println!("absent (dead frames) : {absent}");
    println!("loss gaps            : {} (recovered: {})", gaps.len(), recovered.len());
    if !recovered.is_empty() {
        let sum: usize = recovered.iter().sum();
        println!("  recovery frames    : worst {} | mean {:.1} | {:?}", recovered.iter().copied().max().unwrap_or(0), sum as f64 / recovered.len() as f64, recovered);
    }
    if ended_in_gap {
        println!("  PERMANENT DESYNC   : the final gap of {} frames never recovered before capture end", gaps.last().copied().unwrap_or(0));
    }
    println!("worst gap            : {worst_gap} frames");

    // --- verdict ---
    let pass = corruptions == 0 && !ended_in_gap && worst_gap <= recover;
    println!("\nVERDICT: {}", if pass { "PASS" } else { "FAIL" });
    if !pass {
        if corruptions > 0 {
            println!("  - silent corruption: the client would show WRONG pixels as if correct");
        }
        if ended_in_gap {
            println!("  - permanent desync: a single lost datagram killed the wire until the next keyframe (or forever)");
        }
        if worst_gap > recover && !ended_in_gap {
            println!("  - slow recovery: worst gap {worst_gap} > --recover {recover}");
        }
    }
    std::process::exit(if pass { 0 } else { 1 });
}
