#!/usr/bin/env python3
"""Round 2: push under 1 Mbps.
 (g2) streaming with context RESET at each keyframe (late-join realistic)
 (i)  streaming level sweep at wlog=24
 (j)  streaming window sweep at level 3
 (h)  VRAM/PVR page content-dedup (client page cache) + streaming
 (l)  TA delta run-list SoA restructure (offsets/lens/data separated) + streaming
 (m)  combined: dedup + run-SoA + streaming zstd-3/9 wlog=24
"""
import struct, sys, time, collections, hashlib
import numpy as np
import zstandard as zstd

CAP = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\trist\projects\maplecast-flycast\_bwlab\cap.mirror.zcst"

def read_frames(path):
    data = open(path, "rb").read()
    off = 0; raws = []
    dctx = zstd.ZstdDecompressor()
    while off + 4 <= len(data):
        n = struct.unpack_from("<I", data, off)[0]; off += 4
        m = data[off:off+n]; off += n
        if len(m) >= 8 and m[:4] == b"ZCST":
            usize = struct.unpack_from("<I", m, 4)[0]
            raw = dctx.decompress(m[8:], max_output_size=usize)
            if raw[:4] != b"SYNC": raws.append(raw)
    return raws

def mbps(total, n): return total / n * 60 * 8 / 1e6

def stream_compress(frames, level, wlog=0, reset_every=0):
    if wlog:
        params = zstd.ZstdCompressionParameters.from_level(level, window_log=wlog)
        mk = lambda: zstd.ZstdCompressor(compression_params=params)
    else:
        mk = lambda: zstd.ZstdCompressor(level=level)
    obj = mk().compressobj()
    t0 = time.perf_counter(); total = 0
    for i, r in enumerate(frames):
        if reset_every and i and i % reset_every == 0:
            total += len(obj.flush())  # end-of-frame flush
            obj = mk().compressobj()
        total += len(obj.compress(r)) + len(obj.flush(zstd.COMPRESSOBJ_FLUSH_BLOCK))
    dt = time.perf_counter() - t0
    return total, dt

def report(name, total, dt, n, results):
    results.append((name, mbps(total, n), dt / n * 1000))
    print(f"  {name:56s} {mbps(total,n):7.3f} Mbps  {dt/n*1000:7.3f} ms/frame")

def split_frame(raw):
    """-> (header88, payload, keyframe, [(rid,pidx,pagebytes)...])"""
    taSize, dps = struct.unpack_from("<II", raw, 72)
    ps = 80; pay = raw[ps:ps+dps]
    p = ps + dps
    cs, dirty = struct.unpack_from("<II", raw, p); p += 8
    pages = []
    for _ in range(dirty):
        rid = raw[p]; pidx = struct.unpack_from("<I", raw, p+1)[0]
        pages.append((rid, pidx, raw[p+5:p+5+4096])); p += 5 + 4096
    return raw[:80], pay, dps == taSize, pages, cs

def rebuild_run_soa(pay, keyframe):
    """delta payload -> [nRuns u32][offsets u32...][lens u16...][data]"""
    if keyframe: return pay
    offs = []; lens = []; datas = []
    q = 0
    while q + 4 <= len(pay):
        o = struct.unpack_from("<I", pay, q)[0]
        if o == 0xFFFFFFFF: break
        rl = struct.unpack_from("<H", pay, q+4)[0]
        offs.append(o); lens.append(rl); datas.append(pay[q+6:q+6+rl]); q += 6 + rl
    # delta-encode offsets (gap from previous run end) as u32 still, but monotonic gaps compress well
    gaps = [offs[0]] + [offs[i] - (offs[i-1] + lens[i-1]) for i in range(1, len(offs))]
    return (struct.pack("<I", len(offs))
            + np.array(gaps, dtype=np.uint32).tobytes()
            + np.array(lens, dtype=np.uint16).tobytes()
            + b"".join(datas))

def main():
    print("loading ...")
    raws = read_frames(CAP)
    n = len(raws)
    results = []

    print("(g2) streaming with context reset every 60 frames (joinable at keyframes):")
    for lvl in (1, 3, 9):
        total, dt = stream_compress(raws, lvl, wlog=24, reset_every=60)
        report(f"streaming zstd-{lvl} wlog=24, reset@60", total, dt, n, results)

    print("(i) streaming level sweep, wlog=24, no reset:")
    for lvl in (6, 9, 12):
        total, dt = stream_compress(raws, lvl, wlog=24)
        report(f"streaming zstd-{lvl} wlog=24", total, dt, n, results)

    print("(j) streaming window sweep, level 3:")
    for wl in (22, 25, 26):
        total, dt = stream_compress(raws, 3, wlog=wl)
        report(f"streaming zstd-3 wlog={wl}", total, dt, n, results)

    # ---- (h) page content dedup ----
    print("(h) page content-dedup (client-side content-addressed page cache):")
    seen = set(); same_slot = {}
    tot_pages = dup_any = dup_same_slot = 0
    dedup_frames = []; runsoa_frames = []; combo_frames = []
    t_dedup = 0.0; t_soa = 0.0
    for raw in raws:
        hdr, pay, kf, pages, cs = split_frame(raw)
        t0 = time.perf_counter()
        kept = []; refs = 0
        for rid, pidx, pg in pages:
            tot_pages += 1
            h = hashlib.blake2b(pg, digest_size=8).digest()
            if same_slot.get((rid, pidx)) == h: dup_same_slot += 1
            same_slot[(rid, pidx)] = h
            if h in seen:
                dup_any += 1; refs += 1
                kept.append(struct.pack("<BIQ", rid | 0x80, pidx, int.from_bytes(h, "little")))
            else:
                seen.add(h)
                kept.append(struct.pack("<BI", rid, pidx) + pg)
        page_blob = struct.pack("<II", cs, len(pages)) + b"".join(kept)
        dd = hdr + pay + page_blob
        t_dedup += time.perf_counter() - t0
        dedup_frames.append(dd)
        t0 = time.perf_counter()
        soa_pay = rebuild_run_soa(pay, kf)
        t_soa += time.perf_counter() - t0
        runsoa_frames.append(hdr + soa_pay + raw[80+len(pay):])
        combo_frames.append(hdr + soa_pay + page_blob)
    print(f"  pages shipped: {tot_pages:,}  dup-vs-same-slot-last: {dup_same_slot:,} ({100*dup_same_slot/tot_pages:.1f}%)  dup-vs-any-seen: {dup_any:,} ({100*dup_any/tot_pages:.1f}%)")
    print(f"  unique page contents over capture: {len(seen):,}  (dedup transform {t_dedup/n*1000:.3f} ms/frame)")
    total, dt = stream_compress(dedup_frames, 3, wlog=24)
    report("dedup-pages + streaming zstd-3 wlog=24", total, dt + t_dedup, n, results)

    print(f"(l) TA delta run-list SoA (gap-offsets u32 | lens u16 | data)  [{t_soa/n*1000:.3f} ms/frame]:")
    c3 = zstd.ZstdCompressor(level=3)
    t0 = time.perf_counter(); tot_pf = sum(len(c3.compress(r)) for r in runsoa_frames); dt_pf = time.perf_counter() - t0
    report("runSoA + per-frame zstd-3", tot_pf, dt_pf + t_soa, n, results)
    total, dt = stream_compress(runsoa_frames, 3, wlog=24)
    report("runSoA + streaming zstd-3 wlog=24", total, dt + t_soa, n, results)

    print("(m) combined dedup + runSoA + streaming:")
    for lvl in (3, 9):
        total, dt = stream_compress(combo_frames, lvl, wlog=24)
        report(f"dedup+runSoA + streaming zstd-{lvl} wlog=24", total, dt + t_dedup + t_soa, n, results)
    total, dt = stream_compress(combo_frames, 3, wlog=24, reset_every=60)
    report("dedup+runSoA + streaming zstd-3 wlog=24 reset@60", total, dt + t_dedup + t_soa, n, results)

    print()
    print("== ROUND-2 SUMMARY (sorted by Mbps) ==")
    print(f"{'experiment':58s} {'Mbps':>8s} {'ms/frame':>9s}")
    for name, m, ms_ in sorted(results, key=lambda x: x[1]):
        print(f"{name:58s} {m:8.3f} {ms_:9.3f}")

if __name__ == "__main__":
    main()
