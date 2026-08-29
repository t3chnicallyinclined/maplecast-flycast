#!/usr/bin/env python3
"""Compression experiments on the captured inner (uncompressed) mirror frames.

Every experiment reports: projected steady-state Mbps @60fps + server-side ms/frame.
Mbps = (total_compressed_bytes / n_frames) * 60 * 8 / 1e6
"""
import struct, sys, time, collections
import numpy as np
import zstandard as zstd

CAP = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\trist\projects\maplecast-flycast\_bwlab\cap.mirror.zcst"

def read_frames(path):
    data = open(path, "rb").read()
    off = 0; raws = []; wire_comp = []
    dctx = zstd.ZstdDecompressor()
    while off + 4 <= len(data):
        n = struct.unpack_from("<I", data, off)[0]; off += 4
        m = data[off:off+n]; off += n
        if len(m) >= 8 and m[:4] == b"ZCST":
            usize = struct.unpack_from("<I", m, 4)[0]
            raw = dctx.decompress(m[8:], max_output_size=usize)
            if raw[:4] == b"SYNC": continue
            raws.append(raw); wire_comp.append(len(m) - 8)
    return raws, wire_comp

def parse_sections(raw):
    """returns (payload_start, payload_len, taSize, keyframe, pages_start)"""
    taSize, dps = struct.unpack_from("<II", raw, 72)
    return 80, dps, taSize, dps == taSize, 80 + dps + 8

def mbps(total_comp, n):
    return total_comp / n * 60 * 8 / 1e6

def run_percomp(name, raws, compress_fn, results):
    t0 = time.perf_counter()
    total = sum(len(compress_fn(i, r)) for i, r in enumerate(raws))
    dt = time.perf_counter() - t0
    n = len(raws)
    results.append((name, mbps(total, n), dt / n * 1000, total))
    print(f"  {name:48s} {mbps(total,n):7.3f} Mbps  {dt/n*1000:7.3f} ms/frame  ({total:,} B)")

def soa_transform(raw):
    """Transpose the TA payload region into 32-byte-parcel byte planes; rest untouched."""
    ps, plen, taSize, kf, _ = parse_sections(raw)
    pay = np.frombuffer(raw, dtype=np.uint8, count=plen, offset=ps)
    np_parcels = plen // 32
    head = pay[:np_parcels*32].reshape(np_parcels, 32).T.tobytes()  # byte-plane
    tail = pay[np_parcels*32:].tobytes()
    return raw[:ps] + head + tail + raw[ps+plen:]

def main():
    print("loading + decompressing capture ...")
    raws, wire_comp = read_frames(CAP)
    n = len(raws)
    total_wire = sum(wire_comp) + 8 * n
    print(f"{n} frames, uncompressed total {sum(len(r) for r in raws):,} B")
    print(f"WIRE AS CAPTURED (zstd-1 server): {mbps(total_wire, n):.3f} Mbps  avg {total_wire/n:.0f} B/frame")
    print()
    results = []

    # ---- (a) baseline re-zstd level 1 ----
    print("(a) baseline per-frame zstd:")
    c1 = zstd.ZstdCompressor(level=1)
    run_percomp("zstd-1 per frame (baseline)", raws, lambda i, r: c1.compress(r), results)

    # ---- (b) levels 3/9/19 ----
    print("(b) higher levels per frame:")
    for lvl in (3, 9, 19):
        c = zstd.ZstdCompressor(level=lvl)
        run_percomp(f"zstd-{lvl} per frame", raws, lambda i, r, c=c: c.compress(r), results)

    # ---- component attribution at zstd-1 (where does the COMPRESSED bandwidth go) ----
    print("(x) component attribution: compress each section separately, zstd-1:")
    sec_tot = collections.Counter()
    for r in raws:
        ps, plen, taSize, kf, pgs = parse_sections(r)
        head = r[:ps]; pay = r[ps:ps+plen]; pages = r[ps+plen:]
        sec_tot["hdr88"] += len(c1.compress(head))
        sec_tot["ta_keyframe" if kf else "ta_delta_payload"] += len(c1.compress(pay))
        # split pages by region
        p = ps + plen + 8; vram = bytearray(); pvr = bytearray()
        cs, dirty = struct.unpack_from("<II", r, ps + plen)
        for _ in range(dirty):
            (vram if r[p] == 1 else pvr).extend(r[p:p+5+4096]); p += 5 + 4096
        sec_tot["vram_pages"] += len(c1.compress(bytes(vram)))
        sec_tot["pvr_pages"] += len(c1.compress(bytes(pvr)))
    for k, v in sec_tot.most_common():
        print(f"  {k:20s} {v:>12,} B compressed = {mbps(v, n):7.3f} Mbps")
    print(f"  (sum {sum(sec_tot.values()):,} B = {mbps(sum(sec_tot.values()), n):.3f} Mbps; separate-context overhead vs (a) expected)")

    # ---- (c) trained dictionary ----
    print("(c) trained dictionary (112640 B, ~200 samples):")
    samples = raws[::max(1, n // 200)][:200]
    t0 = time.perf_counter()
    d = zstd.train_dictionary(112640, samples)
    print(f"  [train time {time.perf_counter()-t0:.1f}s, dict {len(d.as_bytes()):,} B]")
    for lvl in (1, 3):
        d.precompute_compress(level=lvl)
        cd = zstd.ZstdCompressor(level=lvl, dict_data=d)
        run_percomp(f"zstd-{lvl} + trained dict", raws, lambda i, r, cd=cd: cd.compress(r), results)

    # ---- (d) SoA transform ----
    print("(d) structure transform (TA payload -> 32B-parcel byte planes) then zstd:")
    t0 = time.perf_counter()
    soa = [soa_transform(r) for r in raws]
    tf = (time.perf_counter() - t0) / n * 1000
    print(f"  [transform cost {tf:.3f} ms/frame, add to compress time]")
    for lvl in (1, 3):
        c = zstd.ZstdCompressor(level=lvl)
        run_percomp(f"SoA(TA) + zstd-{lvl}  (+{tf:.2f}ms transform)", soa, lambda i, r, c=c: c.compress(r), results)
    del soa

    # ---- (e) delta keyframes ----
    print("(e) keyframes shipped as XOR-delta vs previous keyframe (zstd-3):")
    kf_pay = []
    for r in raws:
        ps, plen, taSize, kf, _ = parse_sections(r)
        if kf: kf_pay.append(np.frombuffer(r, np.uint8, plen, ps))
    c3 = zstd.ZstdCompressor(level=3)
    raw_kf_comp = sum(len(c3.compress(p.tobytes())) for p in kf_pay)
    t0 = time.perf_counter(); diff_comp = 0
    for a, b in zip(kf_pay, kf_pay[1:]):
        L = min(len(a), len(b))
        x = np.bitwise_xor(a[:L], b[:L])
        diff_comp += len(c3.compress(x.tobytes() + b[L:].tobytes()))
    dt = (time.perf_counter() - t0) / max(1, len(kf_pay) - 1) * 1000
    print(f"  {len(kf_pay)} keyframes: raw zstd-3 avg {raw_kf_comp/len(kf_pay):,.0f} B  ->  XOR-delta zstd-3 avg {diff_comp/max(1,len(kf_pay)-1):,.0f} B  ({dt:.2f} ms/keyframe)")
    print(f"  keyframe wire share: {mbps(raw_kf_comp, n):.3f} Mbps -> {mbps(diff_comp * len(kf_pay)/max(1,len(kf_pay)-1), n):.3f} Mbps")

    # ---- (f) previous-frame prefix dictionary ----
    print("(f) previous frame as raw-content prefix dictionary:")
    for lvl in (1, 3):
        def comp_prev(i, r, lvl=lvl):
            if i == 0: return zstd.ZstdCompressor(level=lvl).compress(r)
            d = zstd.ZstdCompressionDict(raws[i-1], dict_type=zstd.DICT_TYPE_RAWCONTENT)
            return zstd.ZstdCompressor(level=lvl, dict_data=d).compress(r)
        run_percomp(f"zstd-{lvl} + prev-frame dict", raws, comp_prev, results)

    # ---- (g) streaming compressor, shared window, flush per frame ----
    print("(g) single streaming zstd (shared window), flush per frame -- realistic server change:")
    for lvl, wlog in ((1, 0), (3, 0), (1, 24), (3, 24)):
        if wlog:
            params = zstd.ZstdCompressionParameters.from_level(lvl, window_log=wlog)
            c = zstd.ZstdCompressor(compression_params=params)
            name = f"streaming zstd-{lvl} wlog={wlog} flush/frame"
        else:
            c = zstd.ZstdCompressor(level=lvl)
            name = f"streaming zstd-{lvl} flush/frame"
        obj = c.compressobj()
        t0 = time.perf_counter()
        total = 0
        for r in raws:
            total += len(obj.compress(r)) + len(obj.flush(zstd.COMPRESSOBJ_FLUSH_BLOCK))
        dt = time.perf_counter() - t0
        results.append((name, mbps(total, n), dt / n * 1000, total))
        print(f"  {name:48s} {mbps(total,n):7.3f} Mbps  {dt/n*1000:7.3f} ms/frame  ({total:,} B)")

    # ---- summary ----
    print()
    print("== SUMMARY (sorted by Mbps) ==")
    print(f"{'experiment':50s} {'Mbps':>8s} {'ms/frame':>9s}")
    for name, m, ms_, _ in sorted(results, key=lambda x: x[1]):
        print(f"{name:50s} {m:8.3f} {ms_:9.3f}")

if __name__ == "__main__":
    main()
