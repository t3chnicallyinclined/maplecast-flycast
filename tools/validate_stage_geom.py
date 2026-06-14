#!/usr/bin/env python3
"""
validate_stage_geom.py — numeric acceptance gate for the world-space stage bake.

Projects rip_stage.py's WORLD-assembled stage geometry (atlas/stages/STGxx.json,
PLACED models only) through the engine's OWN live camera XMTRX = M1·M2 and diffs the
result, per screen vertex, against the engine's ACTUAL post-transform stage TA
(_stage_gt/engine_ta.bin). PASS == a few px (target 0.00px for world-authored models).

This is the stage analogue of tools/validate_emitter_geom.py (cardinal rule 3:
validate render math numerically vs ground truth BEFORE deploy, never by eyeball).

Ground truth (all local, gitignored, captured live MAPLECAST_DUMP_RAM on prod):
  _stage_gt/ram.bin        16MB main RAM @0x8C000000 (camera M1@0x8C2D6B18, M2@0x8C2D6AD8)
  _stage_gt/engine_ta.bin  the engine's raw PowerVR param stream for the stage (tctx->tad)

The captured RAM was in-match stage_id=0x11 -> STG0B (re_kb 26).

Usage:
  python3 tools/validate_stage_geom.py 0B
"""
import os, sys, json, struct
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
GT = os.path.join(REPO, "_stage_gt")

ADDR_M1 = 0x8C2D6B18
ADDR_M2 = 0x8C2D6AD8
RAM_BASE = 0x8C000000


def f(u):
    return struct.unpack("<f", struct.pack("<I", u))[0]


def mat16(ram, addr):
    o = addr - RAM_BASE
    return [struct.unpack_from("<f", ram, o + 4 * i)[0] for i in range(16)]


def to44(m):
    A = np.zeros((4, 4))
    for col in range(4):
        for row in range(4):
            A[row, col] = m[col * 4 + row]
    return A


def build_xmtrx(ram):
    M1 = to44(mat16(ram, ADDR_M1))     # viewport
    M2 = to44(mat16(ram, ADDR_M2))     # projection
    return M1 @ M2                     # column-major mat·mat (loc_8c120540): M1·M2


def engine_screen_verts(ta):
    """Parse the engine TA param stream -> screen-space vertices (sx, sy, 1/w)."""
    n = len(ta) // 4
    w = list(struct.unpack_from(f"<{n}I", ta, 0))
    ev = []
    i = 0
    while i + 8 <= n:
        c = w[i]
        if (c >> 29) & 7 == 7:         # vertex param (0xe0../0xf0..)
            ev.append((f(w[i + 1]), f(w[i + 2]), f(w[i + 3])))
        i += 8
    ev = np.array(ev)
    ok = (ev[:, 2] > 0) & (ev[:, 2] < 50) & (np.abs(ev[:, 0]) < 3000) & (np.abs(ev[:, 1]) < 3000)
    return ev[ok]


def project_placed(stage_json, X):
    """Project the WORLD-space placed-model verts through XMTRX -> screen px."""
    J = json.load(open(stage_json))
    out = []
    for m in J["meshes"]:
        if not m.get("placed", True):
            continue
        for tri in m["tris"]:
            for v in tri:
                p = v["pos"]
                fx = X[0, 0]*p[0] + X[0, 1]*p[1] + X[0, 2]*p[2] + X[0, 3]
                fy = X[1, 0]*p[0] + X[1, 1]*p[1] + X[1, 2]*p[2] + X[1, 3]
                fw = X[3, 0]*p[0] + X[3, 1]*p[1] + X[3, 2]*p[2] + X[3, 3]
                inv = 1.0 / (fw if fw else 1e-6)
                sx, sy = fx * inv, fy * inv
                if 0 < inv < 50 and abs(sx) < 3000 and abs(sy) < 3000:
                    out.append((sx, sy))
    return np.array(out)


def nn_resid(A, B):
    return np.array([np.hypot(B[:, 0] - p[0], B[:, 1] - p[1]).min() for p in A])


def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    sid = sys.argv[1].upper().zfill(2)
    ram = open(os.path.join(GT, "ram.bin"), "rb").read()
    ta = open(os.path.join(GT, "engine_ta.bin"), "rb").read()
    X = build_xmtrx(ram)
    ev = engine_screen_verts(ta)
    cv = project_placed(os.path.join(REPO, "atlas", "stages", f"STG{sid}.json"), X)
    r = nn_resid(cv, ev[:, :2])
    print(f"STG{sid}: placed verts={len(cv)}  engine verts={len(ev)}")
    print(f"  client->engine NN residual px: "
          f"median={np.median(r):.3f} mean={np.mean(r):.3f} "
          f"p90={np.percentile(r, 90):.3f} max={r.max():.3f}")
    gate = np.percentile(r, 90)
    print("  GATE p90 < 2.0px:", "PASS" if gate < 2.0 else "FAIL")
    sys.exit(0 if gate < 2.0 else 1)


if __name__ == "__main__":
    main()
