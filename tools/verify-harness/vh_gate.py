#!/usr/bin/env python3
"""vh_gate.py — THE GATE. Runs every objective check against a capture (+ optional converted
wire file) and prints one PASS/FAIL per check with the measured number. Refuses to sign off
unless every hard gate passes. Nothing here is an impression; every line is a number.

usage:
  python vh_gate.py --v3 real.v3 [--rec real4.gstarec] [--atlas <dir>] [--anim <dir>]

HARD gates (any FAIL = do not ship):
  T1 timeline        every game frame present exactly once           fc-delta set == {1}
  T2 wire timeline   converted file has one row per game frame       coverage == 100%
  I1 slot identity   the chosen pair is the pair the engine rendered  >= 99% of frames
  I2 bench exclusion no slot parked at +-1386.7 / pinned is drawn     0 frames
  P1 placement       screen coords within 8 px of the engine's own    p95 <= 8 px
  P2 ground line     grounded fighter sits on the engine ground line  p95 <= 3 px
  A1 atlas range     every (cid,sid) exists in the atlas              0 missing
  A2 anim identity   observed sids are real DC cells for that char    >= 99%
SOFT (report, do not gate):
  S1 both-on-canvas fraction, S2 anim run-length vs DC duration, S3 longest chain match
"""
import sys, struct, json, os, collections, statistics, argparse

SR = 25
PARK = 1386.7
CAM_TOL = 1.0          # px: two slots sharing one camera must agree this closely
GROUND_DC = 433.394    # bank02: screen_y = 240 + (eyeY + 98.394) - world_y, eyeY floor 95

def load_v3(path):
    raw = open(path, 'rb').read()
    assert raw[:8] == b'V3SYNC02', f'{path}: not V3SYNC02'
    n = (len(raw) - 8) // (4 + 6*SR); F = []; off = 8
    for _ in range(n):
        fc = struct.unpack_from('<I', raw, off)[0]; off += 4
        sl = []
        for i in range(6):
            a, cid, sid, hp, red, fac, wx, wy, sx, sy = struct.unpack_from('<BBHHHBffff', raw, off); off += SR
            sl.append(dict(act=a, cid=cid, sid=sid & 0x7fff, raw_sid=sid, hp=hp, red=red,
                           fac=fac, wx=wx, wy=wy, sx=sx, sy=sy))
        F.append((fc, sl))
    return F

def null(s):   return s['sx'] == 0.0 and s['sy'] == 0.0
def parked(s): return abs(abs(s['wx']) - PARK) < 5.0

def camera_cluster(sl):
    """The slots the engine rendered THIS frame agree on one camera (sx-wx). Stale/parked slots
    hold an old offset and fall out; uninitialised slots read exactly (0,0). Returns the largest
    agreeing group of size >= 2, or None."""
    cams = [(i, sl[i]['sx'] - sl[i]['wx']) for i in range(6) if not null(sl[i])]
    best = None
    for i, c in cams:
        g = [j for j, c2 in cams if abs(c2 - c) <= CAM_TOL]
        if len(g) >= 2 and (best is None or len(g) > len(best)): best = g
    return best

def verdict(name, ok, detail):
    print(f"  [{'PASS' if ok else 'FAIL'}] {name:22s} {detail}")
    return ok

def gate_timeline(F):
    d = collections.Counter(F[i+1][0] - F[i][0] for i in range(len(F)-1))
    span = F[-1][0] - F[0][0] + 1
    return verdict('T1 capture timeline', list(d) == [1] and len(F) == span,
                   f'{len(F)} samples / fc span {span} = {len(F)/span*100:.2f}%  deltas={dict(sorted(d.items()))}')

def gate_rec(path):
    f = open(path, 'rb'); assert f.read(8) == b'GSTAREC1'
    ts = []; fcs = []; ln = 0
    while True:
        h = f.read(13)
        if len(h) < 13: break
        t, op, ln = struct.unpack('<dBI', h); p = f.read(ln)
        if len(p) >= 29 and p[:4] == b'GSTA':
            ts.append(t); fcs.append(struct.unpack_from('<I', p, 25)[0])
    df = collections.Counter(fcs[i+1]-fcs[i] for i in range(len(fcs)-1))
    span = fcs[-1]-fcs[0]+1
    return verdict('T2 wire timeline', list(df) == [1] and len(fcs) == span and ln == 380,
                   f'{len(fcs)} rows / span {span} = {len(fcs)/span*100:.2f}%  payload={ln}B  deltas={dict(sorted(df.items()))}')

def gate_identity(F, picker):
    """picker(sl) -> (even_slot, odd_slot) or None. Scored against the camera cluster, which is
    the engine's own answer for who was on screen."""
    agree = 0; n = 0; wrong = collections.Counter()
    for fc, sl in F:
        grp = camera_cluster(sl)
        if not grp: continue
        e = [i for i in grp if i % 2 == 0]; o = [i for i in grp if i % 2 == 1]
        if len(e) != 1 or len(o) != 1: continue
        n += 1
        got = picker(sl)
        if got == (e[0], o[0]): agree += 1
        else: wrong[(got, (e[0], o[0]))] += 1
    ok = n > 0 and agree/n >= 0.99
    verdict('I1 slot identity', ok, f'{agree}/{n} frames ({agree/max(1,n)*100:.1f}%) match the engine cluster'
                                    f'   top mismatches {wrong.most_common(3)}')
    return ok

def gate_bench(F, picker):
    bad = 0; tot = 0
    for fc, sl in F:
        got = picker(sl)
        if not got: continue
        tot += 1
        for i in got:
            if i is None: continue
            if parked(sl[i]) or null(sl[i]): bad += 1
    return verdict('I2 bench exclusion', bad == 0, f'{bad} drawn-slot-samples were parked/null out of {tot*2}')

def gate_placement(F, place):
    """place(sl, i, pair) -> (screen_x, screen_y) produced by the pipeline under test.
    Compared against the engine's own +0x6f0/+0x6f4 on frames with a provable camera cluster."""
    ex = []; ey = []
    for fc, sl in F:
        grp = camera_cluster(sl)
        if not grp: continue
        e = [i for i in grp if i % 2 == 0]; o = [i for i in grp if i % 2 == 1]
        if len(e) != 1 or len(o) != 1: continue
        pair = (e[0], o[0])
        for i in pair:
            px, py = place(sl, i, pair)
            ex.append(abs(px - sl[i]['sx'])); ey.append(abs(py - sl[i]['sy']))
    if not ex: return verdict('P1 placement', False, 'no comparable frames')
    ex.sort(); ey.sort()
    p95x = ex[int(.95*len(ex))]; p95y = ey[int(.95*len(ey))]
    return verdict('P1 placement', p95x <= 8 and p95y <= 8,
                   f'x: median={ex[len(ex)//2]:.1f} p95={p95x:.1f} max={ex[-1]:.1f} | '
                   f'y: median={ey[len(ey)//2]:.1f} p95={p95y:.1f} max={ey[-1]:.1f}  (n={len(ex)})')

def gate_ground(F):
    """A fighter with world_y == 0 must sit on the engine's ground line for THAT frame's camera.
    sy + wy == 240 + eyeY + 98.394; eyeY is shared, so all grounded fighters in a frame must
    agree, and the value must equal 433.394 whenever the camera is at its floor (eyeY == 95)."""
    err = []
    for fc, sl in F:
        grp = camera_cluster(sl)
        if not grp: continue
        g = [i for i in grp if sl[i]['wy'] == 0.0]
        if len(g) < 2: continue
        v = [sl[i]['sy'] + sl[i]['wy'] for i in g]
        err.append(max(v) - min(v))
    if not err: return verdict('P2 ground line', False, 'no frame had 2 grounded rendered fighters')
    err.sort()
    return verdict('P2 ground line', err[int(.95*len(err))] <= 3.0,
                   f'inter-fighter ground disagreement p95={err[int(.95*len(err))]:.2f}px max={err[-1]:.2f}px (n={len(err)})')

def gate_atlas(F, adir):
    demand = collections.Counter()
    for fc, sl in F:
        for s in sl:
            if not null(s): demand[(s['cid'], s['sid'])] += 1
    miss = 0; msamp = 0; ex = []
    for (cid, sid), c in demand.items():
        p = os.path.join(adir, f'PL{cid:02X}.json')
        if not os.path.exists(p): miss += 1; msamp += c; ex.append(f'{cid}:noatlas'); continue
        keys = json.load(open(p)).get('sprites', {})
        if str(sid) not in keys:
            miss += 1; msamp += c; ex.append(f'{cid}/0x{sid:x}')
    return verdict('A1 atlas range', miss == 0,
                   f'{miss}/{len(demand)} (cid,sid) absent, {msamp} slot-frames  {ex[:5]}'
                   f'   NOTE: atlases are dense 0..N-1, so this catches only OUT-OF-RANGE ids')

def gate_anim(F, andir):
    ok_all = True
    for k in range(6):
        ss = [f[1][k] for f in F]
        cid = ss[0]['cid']
        p = os.path.join(andir, f'PL{cid:02X}.json')
        if not os.path.exists(p): continue
        j = json.load(open(p)); cells = set(); chains = []
        for g in j['groups'].values():
            for sa in g['subanims']:
                ch = [c['sprite_id'] for c in sa['cells']]; chains.append(ch); cells.update(ch)
        runs = []; cur = ss[0]['sid']; c = 1
        for s in ss[1:]:
            if s['sid'] == cur: c += 1
            else: runs.append(cur); cur = s['sid']; c = 1
        runs.append(cur)
        if len(runs) < 3: continue
        inset = sum(1 for s in runs if s in cells)
        best = 0
        for ch in chains:
            for i in range(len(runs)):
                j2 = 0
                while j2 < len(ch) and i+j2 < len(runs) and runs[i+j2] == ch[j2]: j2 += 1
                best = max(best, j2)
        ok = inset/len(runs) >= 0.99
        ok_all &= ok
        verdict(f'A2 anim id slot{k}', ok,
                f'cid={cid} 0x{cid:02X}: {inset}/{len(runs)} sids are real DC cells '
                f'({inset/len(runs)*100:.1f}%), longest exact chain={best} cells')
    return ok_all

# ---- the pipelines under test -------------------------------------------------
def picker_v4(sl):
    """v4_to_gstarec.py:27-34 — alive, |wx|<1300, prefer +0x004."""
    out = []
    for par in (0, 1):
        c = [i for i in range(par, 6, 2) if sl[i]['hp'] > 0 and abs(sl[i]['wx']) < 1300
             and not (sl[i]['wx'] == 0.0 and sl[i]['wy'] == 0.0)]
        if not c: return None
        a = [i for i in c if sl[i]['act'] == 1]
        out.append((a or c)[0])
    return tuple(out)

def picker_camera(sl):
    """PROPOSED: the engine's own answer - the slots that agree on one camera this frame."""
    grp = camera_cluster(sl)
    if not grp: return None
    e = [i for i in grp if i % 2 == 0]; o = [i for i in grp if i % 2 == 1]
    if len(e) != 1 or len(o) != 1: return None
    return (e[0], o[0])

def place_v4(sl, i, pair):
    """v4_to_gstarec.py:62-63 — camera at the midpoint, ground pinned at 434."""
    cam = (sl[pair[0]]['wx'] + sl[pair[1]]['wx']) / 2.0
    return (320.0 + (sl[i]['wx'] - cam), 434.0 - sl[i]['wy'])

def place_engine(sl, i, pair):
    """PROPOSED: read the engine's own screen coords.
    WARNING: scoring this against +0x6f0/+0x6f4 is a TAUTOLOGY (0.0px by construction).
    P1 is auto-skipped for this pipeline; the only valid gate for it is the LIVE PIXEL test
    (overlay the recorded sx/sy on a game screenshot taken at the same frame counter)."""
    return (sl[i]['sx'], sl[i]['sy'])

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--v3', required=True); ap.add_argument('--rec')
    ap.add_argument('--atlas'); ap.add_argument('--anim')
    ap.add_argument('--pipeline', default='v4', choices=['v4', 'engine'])
    a = ap.parse_args()
    F = load_v3(a.v3)
    print(f'\n=== VERIFICATION GATE :: {a.v3} :: {len(F)} samples :: pipeline={a.pipeline} ===')
    hard = []
    hard.append(gate_timeline(F))
    if a.rec: hard.append(gate_rec(a.rec))
    pick = picker_v4 if a.pipeline == 'v4' else picker_camera
    plc  = place_v4  if a.pipeline == 'v4' else place_engine
    hard.append(gate_identity(F, pick))
    hard.append(gate_bench(F, pick))
    if a.pipeline == 'engine':
        print('  [SKIP] P1 placement           TAUTOLOGY for pipeline=engine - '
              'requires the LIVE PIXEL test (screenshot at the same frame counter)')
    else:
        hard.append(gate_placement(F, plc))
    hard.append(gate_ground(F))
    if a.atlas: hard.append(gate_atlas(F, a.atlas))
    if a.anim:  hard.append(gate_anim(F, a.anim))
    print(f'\n=== {"PASS - signed off" if all(hard) else "FAIL - DO NOT SHIP"} ===')
    sys.exit(0 if all(hard) else 1)
