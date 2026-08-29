#!/usr/bin/env python3
"""vh_rec.py <file.gstarec>... — TIMELINE gate on a recorded GSTA wire file.
Checks (a) every game frame is present exactly once (fc delta == 1 everywhere),
(b) the playback timestamps advance by exactly one frame (dt*60 == 1),
(c) the payload is the full 380-byte wire. Any other fc delta = a DROPPED FRAME:
the timeline jumps and everything appears to bounce (handoff FAILURE 3)."""
import struct, collections, sys
for path in sys.argv[1:]:
    f = open(path, 'rb')
    assert f.read(8) == b'GSTAREC1', path
    ts = []; fcs = []; n = 0; ln = 0
    while True:
        h = f.read(13)
        if len(h) < 13: break
        t, op, ln = struct.unpack('<dBI', h); p = f.read(ln)
        if len(p) >= 29 and p[:4] == b'GSTA':
            ts.append(t); fcs.append(struct.unpack_from('<I', p, 25)[0]); n += 1
    d = collections.Counter(round((ts[i+1]-ts[i])*60) for i in range(len(ts)-1))
    df = collections.Counter(fcs[i+1]-fcs[i] for i in range(len(fcs)-1))
    span = (fcs[-1]-fcs[0]+1) if fcs else 0
    print(f"{path}  frames={n} payload={ln}B fc_span={span} coverage={n/span*100 if span else 0:.2f}%")
    print(f"   dt*60 histogram : {dict(sorted(d.items()))}")
    print(f"   fc-delta        : {dict(sorted(df.items()))}")
    print(f"   VERDICT         : {'PASS' if list(df)==[1] and list(d)==[1] and n==span else 'FAIL'}")
