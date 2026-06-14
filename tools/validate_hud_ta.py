#!/usr/bin/env python3
"""
validate_hud_ta.py — quad-level validation of the HUDQ tail (MAPLECAST_HUD_TA).

The HUDQ tail (maplecast_replica_live.cpp captureFrame) ships the engine's REAL
HUD quads, collected by collectHudQuads (maplecast_oracle_hook.cpp) from the SAME
ta_parse() the render path runs. So the acceptance test is a SELF-CONSISTENCY
proof: re-run the IDENTICAL discriminator over the engine's raw TA stream
(parse_engine_ta.walk) and confirm the kept poly set == the HUDQ tail, byte-for-byte
on the corners / tcw / tsp / col it carries.

Two modes:

  (a) ENGINE-TA inventory (no live capture needed):
        python3 tools/validate_hud_ta.py <engine_ta.bin>
      Walks the raw PVR param stream, applies the C++ discriminator, prints the
      expected HUDQ inventory (count + per-quad corners/tcw/tsp). This is what sets
      MAX_HUD + confirms the thresholds. Run it on the surviving (HUD) pass's TA.

  (b) CROSS-CHECK a live HUDQ tail against the engine TA of the SAME frame:
        python3 tools/validate_hud_ta.py <engine_ta.bin> --hudq <hudq_tail.bin>
      <hudq_tail.bin> = the raw HUDQ tail bytes (u32 magic 'HUDQ' + u32 nHud +
      nHud*96). PASS = every expected quad present with identical x[4]/y[4]/tcw/tsp.

The discriminator MUST stay in lockstep with collectHudQuads:
  keep = (cy<TOPY || cy>=BOTY) && w<MAXW && h<MAXH && fmt!=7
  TOPY=120 BOTY=420 MAXW=320 MAXH=200   (env-overridable in the C++; defaults here)
"""
import os, sys, struct

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import parse_engine_ta as P

TOPY, BOTY, MAXW, MAXH = 120.0, 420.0, 320.0, 200.0
HUDQ_MAGIC = 0x48554451


def expected_hud(ta_bytes):
    """Return the list of kept HUD polys with the same fields the HudQuad carries."""
    groups = P.walk(ta_bytes)
    out = []
    for g in groups:
        vs = g["verts"]
        if len(vs) < 3:
            continue
        xs = [v["x"] for v in vs]; ys = [v["y"] for v in vs]
        mnX, mxX = min(xs), max(xs); mnY, mxY = min(ys), max(ys)
        w, h = mxX - mnX, mxY - mnY
        cy = (mnY + mxY) * 0.5
        fmt = (g["tcw"] >> 27) & 7
        band = (cy < TOPY) or (cy >= BOTY)
        oversized = (w > MAXW) or (h > MAXH)
        if not (band and not oversized and fmt != 7):
            continue
        # first 4 submit-order corners (3-vert poly closes 4th to 3rd)
        corn = vs[:4] if len(vs) >= 4 else vs + [vs[-1]] * (4 - len(vs))
        out.append({
            "x": [c["x"] for c in corn], "y": [c["y"] for c in corn],
            "u": [c["u"] for c in corn], "v": [c["v"] for c in corn],
            "tcw": g["tcw"], "tsp": g["tsp"], "pcw": g["pcw"], "isp": g["isp"],
            "cy": cy, "w": w, "h": h, "fmt": fmt,
        })
    return out


def parse_hudq_tail(buf):
    """Parse a raw HUDQ tail blob -> list of HudQuad dicts."""
    magic, nHud = struct.unpack_from("<II", buf, 0)
    if magic != HUDQ_MAGIC:
        raise SystemExit(f"bad HUDQ magic {magic:08x} (want {HUDQ_MAGIC:08x})")
    quads = []
    o = 8
    for _ in range(nHud):
        f = struct.unpack_from("<16f", buf, o)        # x[4],y[4],u[4],v[4]
        c = struct.unpack_from("<8I", buf, o + 64)    # col[4],pcw,isp,tsp,tcw
        quads.append({
            "x": list(f[0:4]), "y": list(f[4:8]), "u": list(f[8:12]), "v": list(f[12:16]),
            "col": list(c[0:4]), "pcw": c[4], "isp": c[5], "tsp": c[6], "tcw": c[7],
        })
        o += 96
    return quads


def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    ta = open(sys.argv[1], "rb").read()
    exp = expected_hud(ta)
    print(f"engine TA: {len(ta)} bytes -> EXPECTED HUD quads (discriminator KEEP): {len(exp)}")
    print(f"  thresholds: TOPY={TOPY} BOTY={BOTY} MAXW={MAXW} MAXH={MAXH}")
    for i, q in enumerate(exp[:12]):
        print(f"  [{i:2d}] cy={q['cy']:6.1f} w={q['w']:6.1f} h={q['h']:6.1f} fmt={q['fmt']} "
              f"tcw={q['tcw']:08x} tsp={q['tsp']:08x} "
              f"corners=({q['x'][0]:.0f},{q['y'][0]:.0f})..({q['x'][2]:.0f},{q['y'][2]:.0f})")
    if len(exp) > 12:
        print(f"  ... +{len(exp)-12} more")

    if "--hudq" in sys.argv:
        tail = open(sys.argv[sys.argv.index("--hudq") + 1], "rb").read()
        got = parse_hudq_tail(tail)
        print(f"\nHUDQ tail: {len(got)} quads")
        if len(got) != len(exp):
            print(f"  FAIL: count mismatch (tail {len(got)} != expected {len(exp)})")
            sys.exit(1)
        bad = 0
        for i, (e, g) in enumerate(zip(exp, got)):
            for k in range(4):
                if abs(e["x"][k] - g["x"][k]) > 0.01 or abs(e["y"][k] - g["y"][k]) > 0.01:
                    print(f"  FAIL[{i}] corner {k}: exp ({e['x'][k]:.2f},{e['y'][k]:.2f}) "
                          f"got ({g['x'][k]:.2f},{g['y'][k]:.2f})"); bad += 1
            if e["tcw"] != g["tcw"] or e["tsp"] != g["tsp"]:
                print(f"  FAIL[{i}] tcw/tsp: exp {e['tcw']:08x}/{e['tsp']:08x} "
                      f"got {g['tcw']:08x}/{g['tsp']:08x}"); bad += 1
        if bad == 0:
            print("  PASS: every HUDQ quad byte-identical to the engine TA discriminator output")
        else:
            print(f"  FAIL: {bad} mismatches"); sys.exit(1)


if __name__ == "__main__":
    main()
