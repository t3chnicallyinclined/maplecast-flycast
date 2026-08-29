#!/usr/bin/env python3
"""Decompose a .mirror.zcst capture (framed [len u32 LE][msg]) into the wire byte-split.

Inner ZCST frame layout (after zstd decompress):
  frameSize(4) frameNum(4) pvr_snapshot(64) taSize(4) deltaPayloadSize(4)
  [payload: keyframe if deltaPayloadSize==taSize else runs offset(u32)+runLen(u16)+bytes, 0xFFFFFFFF term]
  checksum(4) dirtyCount(4) [regionId(1)+pageIdx(4)+page(4096)]*N
SYNC frame: "SYNC"(4) vramSize(4) vram pvrSize(4) pvr
"""
import struct, sys, json, collections
import zstandard as zstd

CAP = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\trist\projects\maplecast-flycast\_bwlab\cap.mirror.zcst"

def read_framed(path):
    data = open(path, "rb").read()
    off, msgs = 0, []
    while off + 4 <= len(data):
        n = struct.unpack_from("<I", data, off)[0]; off += 4
        msgs.append(data[off:off+n]); off += n
    return msgs

def main():
    msgs = read_framed(CAP)
    dctx = zstd.ZstdDecompressor()
    other = collections.Counter(); other_bytes = collections.Counter()
    frames = []      # dicts of parsed delta frames
    syncs = []
    zcst_comp_total = 0
    for m in msgs:
        if len(m) >= 8 and m[:4] == b"ZCST":
            usize = struct.unpack_from("<I", m, 4)[0]
            raw = dctx.decompress(m[8:], max_output_size=usize)
            zcst_comp_total += len(m)
            if raw[:4] == b"SYNC":
                syncs.append({"comp": len(m), "uncomp": len(raw)})
                continue
            frameSize, frameNum = struct.unpack_from("<II", raw, 0)
            taSize, dps = struct.unpack_from("<II", raw, 72)
            p = 80
            keyframe = (dps == taSize)
            run_sizes = []
            if keyframe:
                p += dps
                data_bytes, run_ovh = dps, 0
            else:
                end = p + dps
                data_bytes = 0; run_ovh = 0
                q = p
                while q + 4 <= end:
                    o = struct.unpack_from("<I", raw, q)[0]
                    if o == 0xFFFFFFFF: run_ovh += 4; q += 4; break
                    rl = struct.unpack_from("<H", raw, q+4)[0]
                    run_sizes.append(rl)
                    data_bytes += rl; run_ovh += 6; q += 6 + rl
                p = end
            checksum, dirty = struct.unpack_from("<II", raw, p); p += 8
            vram_pages = pvr_pages = 0
            for _ in range(dirty):
                rid = raw[p]
                if rid == 1: vram_pages += 1
                elif rid == 3: pvr_pages += 1
                p += 5 + 4096
            frames.append({
                "comp": len(m), "uncomp": len(raw), "frameNum": frameNum,
                "taSize": taSize, "dps": dps, "keyframe": keyframe,
                "runs": run_sizes, "run_data": data_bytes, "run_ovh": run_ovh,
                "vram_pages": vram_pages, "pvr_pages": pvr_pages,
                "trailing": len(raw) - p,
            })
        else:
            tag = m[:4].decode("latin1") if len(m) >= 4 else "<short>"
            other[tag] += 1; other_bytes[tag] += len(m)

    n = len(frames)
    span_frames = frames[-1]["frameNum"] - frames[0]["frameNum"] + 1
    dur_s = span_frames / 60.0
    kf = [f for f in frames if f["keyframe"]]
    df = [f for f in frames if not f["keyframe"]]

    tot = lambda fs, k: sum(f[k] for f in fs)
    comp_total = tot(frames, "comp")
    uncomp_total = tot(frames, "uncomp")

    # uncompressed byte split
    delta_data   = tot(df, "run_data")
    delta_ovh    = tot(df, "run_ovh")
    key_data     = tot(kf, "run_data")
    vram_bytes   = tot(frames, "vram_pages") * 4096
    pvr_bytes    = tot(frames, "pvr_pages") * 4096
    page_ovh     = (tot(frames, "vram_pages") + tot(frames, "pvr_pages")) * 5
    fixed_hdr    = n * 88          # frameSize+frameNum+pvr64+taSize+dps+checksum+dirtyCount
    envelope     = n * 8           # ZCST + uncompSize (compressed side, but count it)
    trailing     = tot(frames, "trailing")

    mbps = lambda b: b / dur_s * 8 / 1e6

    print(f"capture: {CAP}")
    print(f"msgs total={len(msgs)}  ZCST-delta={n}  SYNC={len(syncs)}  other={sum(other.values())}")
    print(f"frameNum span {frames[0]['frameNum']}..{frames[-1]['frameNum']} = {span_frames} frames = {dur_s:.2f}s @60fps")
    print(f"other msg types: " + ", ".join(f"{k}:{v} ({other_bytes[k]}B)" for k, v in other.most_common()))
    if syncs:
        print(f"SYNC frames: comp={sum(s['comp'] for s in syncs)}B uncomp={sum(s['uncomp'] for s in syncs)}B")
    print()
    print(f"== WIRE (compressed) ==")
    print(f"ZCST delta-frames: {comp_total} B = {mbps(comp_total):.3f} Mbps   avg {comp_total/n:.0f} B/frame")
    print(f"ALL mirror msgs:   {sum(len(m) for m in msgs)} B = {mbps(sum(len(m) for m in msgs)):.3f} Mbps")
    kf_comp = tot(kf, "comp"); df_comp = tot(df, "comp")
    print(f"  keyframes ({len(kf)}): {kf_comp} B = {mbps(kf_comp):.3f} Mbps   avg {kf_comp/max(1,len(kf)):.0f} B")
    print(f"  deltas    ({len(df)}): {df_comp} B = {mbps(df_comp):.3f} Mbps   avg {df_comp/max(1,len(df)):.0f} B")
    print()
    print(f"== UNCOMPRESSED split ({uncomp_total} B total, {mbps(uncomp_total):.2f} Mbps raw) ==")
    rows = [
        ("(a) TA delta run data (non-key)", delta_data),
        ("    TA delta run overhead(6B/run+term)", delta_ovh),
        ("(b) TA keyframes (full buffer)", key_data),
        ("(c) VRAM dirty pages (region1)", vram_bytes),
        ("(d) PVR/palette dirty pages (region3)", pvr_bytes),
        ("    page headers (5B/page)", page_ovh),
        ("(e) fixed frame headers (88B/frame)", fixed_hdr),
        ("    ZCST envelope (8B/frame)", envelope),
        ("    trailing/unparsed", trailing),
    ]
    for name, b in rows:
        print(f"  {name:42s} {b:>12,} B  {100.0*b/uncomp_total:6.2f}%  {mbps(b):7.3f} Mbps-raw")
    acc = sum(b for _, b in rows) - envelope
    print(f"  parse check: accounted {acc} vs uncomp {uncomp_total} (diff {uncomp_total-acc})")
    print()
    # per-second compressed
    print("== per-second compressed B/s (ZCST delta frames, bucketed by frameNum/60) ==")
    buckets = collections.Counter()
    for f in frames: buckets[(f["frameNum"] - frames[0]["frameNum"]) // 60] += f["comp"]
    for s in sorted(buckets):
        print(f"  s{s:02d}: {buckets[s]:>9,} B/s = {buckets[s]*8/1e6:.3f} Mbps")
    print()
    # run histogram
    all_runs = [r for f in df for r in f["runs"]]
    print(f"== TA delta run histogram ({len(all_runs)} runs across {len(df)} delta frames, avg {len(all_runs)/max(1,len(df)):.1f} runs/frame) ==")
    hbuckets = [(1,8),(9,32),(33,128),(129,512),(513,2048),(2049,8192),(8193,65535)]
    for lo, hi in hbuckets:
        rs = [r for r in all_runs if lo <= r <= hi]
        print(f"  {lo:>5}-{hi:<5}: {len(rs):>7} runs, {sum(rs):>12,} B ({100.0*sum(rs)/max(1,sum(all_runs)):5.1f}% of run data)")
    import statistics
    if all_runs:
        print(f"  run size: median={statistics.median(all_runs):.0f} mean={statistics.mean(all_runs):.1f} max={max(all_runs)}")
    print()
    # keyframe-vs-keyframe byte identity
    print("== consecutive keyframe byte-diff ==")
    kf_payloads = []
    # re-extract keyframe payloads
    dctx2 = zstd.ZstdDecompressor()
    for m in msgs:
        if len(m) >= 8 and m[:4] == b"ZCST":
            usize = struct.unpack_from("<I", m, 4)[0]
            raw = dctx2.decompress(m[8:], max_output_size=usize)
            if raw[:4] == b"SYNC": continue
            taSize, dps = struct.unpack_from("<II", raw, 72)
            if dps == taSize:
                kf_payloads.append(raw[80:80+dps])
    idents = []
    for a, b in zip(kf_payloads, kf_payloads[1:]):
        L = min(len(a), len(b))
        same = sum(1 for i in range(0, L, 1) if a[i] == b[i]) if L < 300000 else None
        # fast path with numpy-free: use int compare via bytes
        if same is None:
            same = sum(x == y for x, y in zip(a, b))
        idents.append((same, L, len(a), len(b)))
    if idents:
        avg_same = sum(s for s, *_ in idents) / len(idents)
        avg_L = sum(L for _, L, *_ in idents) / len(idents)
        print(f"  {len(idents)} consecutive pairs: avg identical bytes {avg_same:,.0f} / {avg_L:,.0f} overlap = {100*avg_same/avg_L:.1f}%")
        sizes = [x[2] for x in idents] + [idents[-1][3]]
        print(f"  keyframe sizes: min={min(sizes):,} max={max(sizes):,} mean={sum(sizes)/len(sizes):,.0f}")
    # dump parsed index for the experiment script
    idx = [{k: f[k] for k in ("comp","uncomp","frameNum","taSize","dps","keyframe","vram_pages","pvr_pages")} for f in frames]
    json.dump({"dur_s": dur_s, "n": n, "frames": idx}, open(r"C:\Users\trist\projects\maplecast-flycast\_bwlab\index.json", "w"))

if __name__ == "__main__":
    main()
