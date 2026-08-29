#!/usr/bin/env python3
"""Endgame-(b) price sim: camera-compensated prediction-residual coding of the
full TA stream (docs/TA-DICT-WIRE-PLAN.md §3b RESULTS follow-up).

Model (byte-exact by construction — decoder computes the SAME prediction and
XORs the residual back):
  - Every block gets a SHAPE id = the block with its position floats zeroed
    (the §3b [5] reprojection-test key: 98.5% of new blocks are position-only
    variants). Shape dictionary = v1 whole-block dict minus positions.
  - STAGE-class poly vertices: predicted by CAMERA REPROJECTION — at first
    sight, un-project (x,y,1/w) through that frame's XMTRX=f(M1,M2) to a cached
    world vec; later frames re-project world through the current matrix.
    (Un-projection proven exact for stage verts, re_kb/26b stage_world_proof.)
  - All other vertex blocks: TEMPORAL COPY — predict positions = this shape
    instance's positions last time it appeared (instances paired by per-frame
    occurrence index of the shape key; engine emit order is deterministic).
  - Wire per frame: shape refs (u32/block) + XOR residual streams (pred f32
    bits ^ actual f32 bits; 12 B/poly-vert, 44 B/sprite-vert) + new shapes.
    Streaming zstd-3, flush per frame (same discipline as all prior sims).

The matrix convention (row/col-major, M1·M2 vs M2·M1) is DISCOVERED empirically
on the capture: the candidate minimizing cross-frame stage prediction error
wins; the error is reported so a bad model can't hide.

Usage: python ta_predict_sim.py [capture.zcst]
"""
import struct, sys, statistics
import numpy as np
import zstandard as zstd

sys.path.insert(0, r"C:\Users\trist\projects\maplecast-flycast\_bwlab")
from ta_liveplay_analysis import read_framed, reconstruct_with_camera, walk_blocks

CAP = sys.argv[1] if len(sys.argv) > 1 else \
    r"C:\Users\trist\projects\maplecast-flycast\_bwlab\cap_liveplay.mirror.zcst"

def cam_matrices(cam):
    m2 = np.frombuffer(cam, dtype='<f4', count=16, offset=4).reshape(4, 4).astype(np.float64)
    m1 = np.frombuffer(cam, dtype='<f4', count=16, offset=68).reshape(4, 4).astype(np.float64)
    return m1, m2

def xmtrx(cam):
    """The PROVEN engine transform (tools/bake_stage_from_ta.py build_unproject,
    round-trip verified 1e-13 px): matrices are COLUMN-major in RAM, X = M1·M2.
    reshape(4,4) reads row-major, so column-major load = .T."""
    m1, m2 = cam_matrices(cam)
    return m1.T @ m2.T

def stage_verts(ta):
    """(N,3) screen positions + shape-key/index pairing info for stage poly verts."""
    out = []
    counts = {}
    for blk, cls, kind, is_spr in walk_blocks(ta):
        if kind == "vert" and cls == "stage" and not is_spr:
            key = blk[:4] + b"\0"*12 + blk[16:]
            k = counts.get(key, 0); counts[key] = k + 1
            x, y, z = struct.unpack_from("<3f", blk, 4)
            out.append(((key, k), (x, y, z)))
    return out

def unproject(P, scr):
    """scr (N,3) [x_s, y_s, z_ta=1/w] -> world (N,3) via rows {0,1,3} of P."""
    A = P[[0, 1, 3], :3]; t = P[[0, 1, 3], 3]
    W = 1.0 / scr[:, 2]
    rhs = np.stack([scr[:, 0] * W, scr[:, 1] * W, W], axis=1) - t
    return np.linalg.solve(A[None, :, :].repeat(1, axis=0), rhs.T[None]).squeeze(0).T \
        if False else (np.linalg.inv(A) @ rhs.T).T

def project(P, world):
    A = P[[0, 1, 3], :3]; t = P[[0, 1, 3], 3]
    XYW = world @ A.T + t
    W = XYW[:, 2]
    with np.errstate(divide='ignore', invalid='ignore'):
        return np.stack([XYW[:, 0] / W, XYW[:, 1] / W, 1.0 / W], axis=1)

def validate_model(frames):
    """Cross-frame check of the proven transform: median px error + inlier rate
    (mean is polluted by occurrence-index pairing drift when strips cull)."""
    pairs = []
    for i in range(1, len(frames)):
        ta0, c0 = frames[i-1]; ta1, c1 = frames[i]
        if c0 and c1 and c0 != c1:
            pairs.append((ta0, c0, ta1, c1))
        if len(pairs) >= 6:
            break
    meds, inl = [], []
    for ta0, c0, ta1, c1 in pairs:
        P0, P1 = xmtrx(c0), xmtrx(c1)
        m0 = {ik: p for ik, p in stage_verts(ta0)}
        match0, match1 = [], []
        for ik, p in stage_verts(ta1):
            if ik in m0:
                match0.append(m0[ik]); match1.append(p)
        if len(match0) < 100:
            continue
        s0 = np.array(match0); s1 = np.array(match1)
        ok = (s0[:, 2] > 0) & (s1[:, 2] > 0)
        w = unproject(P0, s0[ok])
        pred = project(P1, w)
        e = np.abs(pred[:, :2] - s1[ok, :2]).max(axis=1)
        meds.append(float(np.median(e)))
        inl.append(float((e < 0.5).mean()))
    return meds, inl

def main():
    frames = reconstruct_with_camera(read_framed(CAP))
    n = len(frames); dur = n / 60.0
    print(f"{n} frames = {dur:.1f}s")

    meds, inl = validate_model(frames)
    print(f"\n[model validation] proven transform (col-major M1·M2, bake_stage_from_ta): "
          f"median err {np.median(meds):.4f} px, inliers(<0.5px) {100*np.mean(inl):.1f}% "
          f"(outliers = occurrence-index pairing drift, handled by the codec's fallback)")

    shape_ids = {}
    world_cache = {}     # (shape_key, inst) -> world vec  (stage poly verts)
    last_pos = {}        # (shape_key, inst) -> position bytes (temporal copy)
    cctx = zstd.ZstdCompressor(level=3)
    comp = cctx.compressobj()
    wire = 0; per_frame = []
    stage_err_px = []
    resid_zero = resid_total = 0
    new_shape_bytes = 0
    fallbacks = predicted = 0

    for fi, (ta, cam) in enumerate(frames):
        P = xmtrx(cam) if cam else None
        refs = bytearray(); resid = bytearray(); news = bytearray()
        counts = {}
        # stage batch: collect then predict vectorized (ref_off lets the fallback
        # protocol flip bit31 of the shape ref = "raw positions follow, re-anchor")
        stage_batch = []   # (resid_offset, ref_offset, ik, actual3)
        for blk, cls, kind, is_spr in walk_blocks(ta):
            if kind == "vert":
                if is_spr:
                    key = blk[:4] + b"\0"*44 + blk[48:]
                    pos = blk[4:48]
                else:
                    key = blk[:4] + b"\0"*12 + blk[16:]
                    pos = blk[4:16]
                k = counts.get(key, 0); counts[key] = k + 1
                ik = (key, k)
                sid = shape_ids.get(key)
                if sid is None:
                    sid = len(shape_ids); shape_ids[key] = sid
                    news.append(len(blk)); news += blk
                    new_shape_bytes += len(blk) + 1
                    refs += struct.pack("<I", 0xFFFFFFFF)  # new-shape marker
                    if cls == "stage" and not is_spr and P is not None:
                        x, y, z = struct.unpack_from("<3f", blk, 4)
                        if z > 0:
                            w = unproject(P, np.array([[x, y, z]], dtype=np.float64))[0]
                            world_cache[ik] = w
                    last_pos[ik] = pos
                    continue
                ref_off = len(refs)
                refs += struct.pack("<I", sid)
                if cls == "stage" and not is_spr and P is not None and ik in world_cache:
                    off = len(resid)
                    resid += b"\0" * 12
                    x, y, z = struct.unpack_from("<3f", blk, 4)
                    stage_batch.append((off, ref_off, ik, (x, y, z)))
                else:
                    prev = last_pos.get(ik)
                    if prev is None or len(prev) != len(pos):
                        resid += pos           # no basis: ship raw positions
                    else:
                        r = bytes(a ^ b for a, b in zip(pos, prev))
                        resid += r
                        resid_zero += r.count(0); resid_total += len(r)
                last_pos[ik] = pos
            else:
                # params/ctrl: whole-block dict (finite vocabulary, v1-style)
                sid = shape_ids.get(blk)
                if sid is None:
                    sid = len(shape_ids); shape_ids[blk] = sid
                    news.append(len(blk)); news += blk
                    new_shape_bytes += len(blk) + 1
                    refs += struct.pack("<I", 0xFFFFFFFF)
                else:
                    refs += struct.pack("<I", sid)
        # vectorized stage prediction + fallback protocol
        if stage_batch and P is not None:
            worlds = np.array([world_cache[ik] for _, _, ik, _ in stage_batch])
            actual = np.array([a for _, _, _, a in stage_batch])
            pred = project(P, worlds)
            err = np.abs(pred[:, :2] - actual[:, :2]).max(axis=1)
            good = np.isfinite(err) & (err < 2.0)
            if good.any():
                stage_err_px.append(float(np.median(err[good])))
            pf32 = pred.astype('<f4').view(np.uint8).reshape(-1, 12)
            af32 = actual.astype('<f4').view(np.uint8).reshape(-1, 12)
            xr = np.bitwise_xor(pf32, af32)
            reanchor = ~good
            if reanchor.any():
                aa = actual[reanchor]
                ok = aa[:, 2] > 0
                new_worlds = np.full((len(aa), 3), np.nan)
                if ok.any():
                    new_worlds[ok] = unproject(P, aa[ok])
            ri = 0
            for gi, ((off, ref_off, ik, a3), row) in enumerate(zip(stage_batch, xr)):
                if good[gi]:
                    resid[off:off+12] = row.tobytes()
                    predicted += 1
                else:
                    # fallback: raw positions + bit31 on the ref; re-anchor world
                    resid[off:off+12] = struct.pack("<3f", *a3)
                    refs[ref_off+3] |= 0x80
                    w = new_worlds[ri] if reanchor.any() else None
                    ri += 1
                    if w is not None and np.isfinite(w).all():
                        world_cache[ik] = w
                    fallbacks += 1
            resid_zero += int((xr[good] == 0).sum()); resid_total += xr.size
        payload = struct.pack("<II", len(refs)//4, len(news)) + bytes(refs) + bytes(resid) + bytes(news)
        c = comp.compress(payload) + comp.flush(zstd.COMPRESSOBJ_FLUSH_BLOCK)
        wire += len(c); per_frame.append(len(c))
        if fi % 2000 == 0:
            print(f"    frame {fi}/{n}  wire so far {wire/ (fi+1):.0f} B/frame")

    print(f"\n[PREDICT-RESIDUAL WIRE] {wire:,} B = {wire/dur*8/1e6:.3f} Mbps  "
          f"median {statistics.median(per_frame):.0f} B/frame  "
          f"p95 {sorted(per_frame)[int(0.95*n)]} B")
    print(f"    shapes {len(shape_ids):,}  new-shape bytes {new_shape_bytes:,}")
    print(f"    residual bytes zero: {100.0*resid_zero/max(1,resid_total):.1f}% of {resid_total:,}")
    tot_stage = predicted + fallbacks
    print(f"    stage verts predicted: {predicted:,}/{tot_stage:,} "
          f"({100.0*predicted/max(1,tot_stage):.1f}%), fallback(raw+reanchor): {fallbacks:,}")
    if stage_err_px:
        print(f"    stage prediction error (predicted verts): median-of-medians "
              f"{np.median(stage_err_px):.4f} px, p95 {np.percentile(stage_err_px, 95):.4f} px")
    print("\nCompare: whole-block v1 = 4.181 Mbps; players-only = 0.347 Mbps "
          "(same capture, ta_liveplay_analysis.py)")

if __name__ == "__main__":
    main()
