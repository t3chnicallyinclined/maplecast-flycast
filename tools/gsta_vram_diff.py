#!/usr/bin/env python3
"""GSTA-vs-engine VRAM byte-diff for the body-texture band [0x400000..0x480000].

Both the GSTA client (reconstruction) and the mirror client (engine ground truth)
dump &vram[0x400000] for 0x80000 bytes per frame. The two streams run against the
SAME deterministic headless slot-0, but use different frame counters, so we align by
CONTENT: for each GSTA frame, find the real frame with the minimum byte-diff (the
same engine state), then report the residual divergence per 512-byte PAL4 tile.

A residual > 0 after best-content alignment is a RECONSTRUCTION DEFECT (carve / twiddle
/ palette-index), located at vram offset 0x400000 + tile*512 (TCW addr).

Usage:
  python tools/gsta_vram_diff.py --gsta <gsta_dir> --real <real_dir> [--window N] [--top K]
"""
import sys, os, glob, argparse

BAND_BASE = 0x400000
BAND_LEN  = 0x80000   # 512 KB

def load(path):
    with open(path, 'rb') as f:
        return f.read()

def key_of(p, pfx):
    b = os.path.basename(p)
    return int(b[len(pfx):-4])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gsta', required=True)
    ap.add_argument('--real', required=True)
    ap.add_argument('--top', type=int, default=20)
    ap.add_argument('--frames', type=int, default=8, help='how many gsta frames to diff')
    a = ap.parse_args()

    greal = sorted(glob.glob(os.path.join(a.real, 'real_vram_*.bin')), key=lambda p: key_of(p, 'real_vram_'))
    ggsta = sorted(glob.glob(os.path.join(a.gsta, 'gsta_vram_*.bin')), key=lambda p: key_of(p, 'gsta_vram_'))
    if not greal or not ggsta:
        sys.exit('missing dumps (real=%d gsta=%d)' % (len(greal), len(ggsta)))

    real = [(key_of(p, 'real_vram_'), load(p)) for p in greal]
    print('loaded %d real frames, %d gsta frames' % (len(real), len(ggsta)))

    # diff a spread of gsta frames against their best-content-matched real frame
    step = max(1, len(ggsta) // a.frames)
    for gi in range(0, len(ggsta), step):
        gpath = ggsta[gi]; gkey = key_of(gpath, 'gsta_vram_'); g = load(gpath)
        # find best real match by total byte-diff over the band
        best = None
        for rk, r in real:
            n = min(len(g), len(r))
            d = sum(1 for i in range(0, n, 64) if g[i:i+64] != r[i:i+64])  # coarse 64B stride scan
            if best is None or d < best[0]:
                best = (d, rk, r)
        _, rk, r = best
        n = min(len(g), len(r))
        # per-512B tile residual (full precision on the matched pair)
        tiles = []
        ndiff = 0
        for t in range(0, n, 512):
            gt = g[t:t+512]; rt = r[t:t+512]
            db = sum(1 for j in range(len(gt)) if gt[j] != rt[j])
            if db:
                tiles.append((db, BAND_BASE + t))
                ndiff += 1
        tiles.sort(reverse=True)
        total = sum(t[0] for t in tiles)
        print('\n=== GSTA vframe %d  ->  REAL frame %d ===' % (gkey, rk))
        print('   divergent 512B tiles: %d / %d   total byte-diff: %d' % (ndiff, n // 512, total))
        for db, addr in tiles[:a.top]:
            print('     tile @vram 0x%06X  (tcw addr 0x%06X)  %d/512 bytes differ' % (addr, addr, db))

if __name__ == '__main__':
    main()
