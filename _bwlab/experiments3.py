#!/usr/bin/env python3
"""Round 3: the server-only fix (skip pages whose content == last-shipped for that slot,
i.e. hash-gate the DMA force-dirty ships) and final stacked combos.
"""
import struct, sys, time, hashlib
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

def stream_compress(frames, level, wlog=24):
    params = zstd.ZstdCompressionParameters.from_level(level, window_log=wlog)
    obj = zstd.ZstdCompressor(compression_params=params).compressobj()
    t0 = time.perf_counter(); total = 0
    for r in frames:
        total += len(obj.compress(r)) + len(obj.flush(zstd.COMPRESSOBJ_FLUSH_BLOCK))
    return total, time.perf_counter() - t0

def split_frame(raw):
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
    if keyframe: return pay
    offs = []; lens = []; datas = []
    q = 0
    while q + 4 <= len(pay):
        o = struct.unpack_from("<I", pay, q)[0]
        if o == 0xFFFFFFFF: break
        rl = struct.unpack_from("<H", pay, q+4)[0]
        offs.append(o); lens.append(rl); datas.append(pay[q+6:q+6+rl]); q += 6 + rl
    gaps = [offs[0]] + [offs[i] - (offs[i-1] + lens[i-1]) for i in range(1, len(offs))] if offs else []
    return (struct.pack("<I", len(offs))
            + np.array(gaps, dtype=np.uint32).tobytes()
            + np.array(lens, dtype=np.uint16).tobytes()
            + b"".join(datas))

def report(name, total, dt, n, results):
    results.append((name, mbps(total, n), dt / n * 1000))
    print(f"  {name:58s} {mbps(total,n):7.3f} Mbps  {dt/n*1000:7.3f} ms/frame")

def main():
    raws = read_frames(CAP)
    n = len(raws)
    results = []

    # build the three transformed corpora
    slot_hash = {}; seen = set()
    skip_frames = []       # server-only: drop pages identical to last-shipped same slot
    skip_soa = []          # + runSoA
    vcache_soa = []        # VCACHE-style any-content refs + runSoA
    skipped_pages = kept_pages = 0
    t_build = time.perf_counter()
    for raw in raws:
        hdr, pay, kf, pages, cs = split_frame(raw)
        kept = []; vkept = []
        for rid, pidx, pg in pages:
            h = hashlib.blake2b(pg, digest_size=8).digest()
            if slot_hash.get((rid, pidx)) == h:
                skipped_pages += 1
            else:
                slot_hash[(rid, pidx)] = h
                kept_pages += 1
                kept.append(struct.pack("<BI", rid, pidx) + pg)
                if h in seen:
                    vkept.append(struct.pack("<BIQ", rid | 0x80, pidx, int.from_bytes(h, "little")))
                else:
                    seen.add(h); vkept.append(struct.pack("<BI", rid, pidx) + pg)
        blob = struct.pack("<II", cs, len(kept)) + b"".join(kept)
        vblob = struct.pack("<II", cs, len(vkept)) + b"".join(vkept)
        soa = rebuild_run_soa(pay, kf)
        skip_frames.append(hdr + pay + blob)
        skip_soa.append(hdr + soa + blob)
        vcache_soa.append(hdr + soa + vblob)
    t_build = time.perf_counter() - t_build
    print(f"pages: shipped-today={skipped_pages+kept_pages:,}  hash-gate-skipped={skipped_pages:,} ({100*skipped_pages/(skipped_pages+kept_pages):.1f}%)  kept={kept_pages:,}")
    print(f"[corpus build {t_build/n*1000:.2f} ms/frame python; C++ server cost ~= 1 hash/4KB page]")
    print()

    print("(n1) SERVER-ONLY hash-gate (no wire change), per-frame zstd as today:")
    for lvl in (1, 3):
        c = zstd.ZstdCompressor(level=lvl)
        t0 = time.perf_counter(); tot = sum(len(c.compress(r)) for r in skip_frames)
        report(f"hash-gate pages + per-frame zstd-{lvl}", tot, time.perf_counter()-t0, n, results)

    print("(n2) hash-gate + streaming:")
    for lvl in (3, 9):
        tot, dt = stream_compress(skip_frames, lvl)
        report(f"hash-gate + streaming zstd-{lvl} wlog=24", tot, dt, n, results)

    print("(n3) hash-gate + runSoA + streaming:")
    for lvl in (3, 6, 9):
        tot, dt = stream_compress(skip_soa, lvl)
        report(f"hash-gate + runSoA + streaming zstd-{lvl} wlog=24", tot, dt, n, results)

    print("(n4) VCACHE-refs (any-content dedup) + runSoA + streaming:")
    for lvl, wl in ((3, 24), (6, 24), (9, 24), (9, 25)):
        tot, dt = stream_compress(vcache_soa, lvl, wlog=wl)
        report(f"vcache-refs + runSoA + streaming zstd-{lvl} wlog={wl}", tot, dt, n, results)

    print()
    print("== ROUND-3 SUMMARY (sorted by Mbps) ==")
    print(f"{'experiment':60s} {'Mbps':>8s} {'zstd ms/frame':>13s}")
    for name, m, ms_ in sorted(results, key=lambda x: x[1]):
        print(f"{name:60s} {m:8.3f} {ms_:13.3f}")

if __name__ == "__main__":
    main()
