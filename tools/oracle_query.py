#!/usr/bin/env python3
"""
oracle_query.py — read the live Frame Oracle ground truth on demand.

The Frame Oracle (persistently armed on prod 149.28.44.118) logs MVC2's authoritative
per-object screen placement to /dev/shm/mc_oracle_hook.jsonl. This tool pulls the latest
slice (or reads a local snapshot) and answers structured queries so we don't hand-write
scp+python every time. See memory reference_frame_oracle + project_atlas_coverage_and_bit15.

Each JSONL record: { frame, objects:[ { node, sprite_id, kind:body|satellite,
  owner_cid, owner_slot, screen_xy:[x,y], scale:[sx,sy], facing, category,
  tex_src:{gfx1_ptr,pal_ptr,region}, screen_quads:[...] } ] }

USAGE
  # pull a fresh slice from prod and summarize who/what is on screen:
  python3 tools/oracle_query.py --pull

  # filter (cockpit shows MASKED sid = engine_sid & 0x7fff; pass either, we match both):
  python3 tools/oracle_query.py --pull --cid 52 --kind satellite
  python3 tools/oracle_query.py --cid 52 --sid 0x8001        # reads last pulled snapshot
  python3 tools/oracle_query.py --file _ryu_capture/oracle_sentinel_rocket_20260612.jsonl --cid 52

FLAGS
  --pull            scp the live jsonl from prod into _ryu_capture/oracle_latest.jsonl first
  --file PATH       read this snapshot instead of the default _ryu_capture/oracle_latest.jsonl
  --cid N           filter by owner_cid (decimal or 0xNN)
  --sid S           filter by sprite_id; matches raw AND (raw & 0x7fff) so 0x8001==0x1
  --kind K          body | satellite
  --frames A-B      only frames in [A,B]
  --traj            print per-frame screen_xy trajectory (default summarizes distinct sids)
  --host H          prod host (default root@149.28.44.118)
"""
import json, sys, os, subprocess, argparse
from collections import Counter, defaultdict

DEFAULT_HOST = 'root@149.28.44.118'
REMOTE_PATH  = '/dev/shm/mc_oracle_hook.jsonl'
LOCAL_LATEST = '_ryu_capture/oracle_latest.jsonl'

def parse_int(s):
    if s is None: return None
    return int(s, 16) if str(s).lower().startswith('0x') else int(s)

def pull(host):
    os.makedirs('_ryu_capture', exist_ok=True)
    print(f"pulling {host}:{REMOTE_PATH} -> {LOCAL_LATEST} ...", file=sys.stderr)
    r = subprocess.run(['scp', '-o', 'StrictHostKeyChecking=no', '-o', 'ConnectTimeout=25',
                        f'{host}:{REMOTE_PATH}', LOCAL_LATEST])
    if r.returncode != 0:
        sys.exit(f"scp failed (rc={r.returncode})")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pull', action='store_true')
    ap.add_argument('--file', default=LOCAL_LATEST)
    ap.add_argument('--cid')
    ap.add_argument('--sid')
    ap.add_argument('--kind')
    ap.add_argument('--frames')
    ap.add_argument('--traj', action='store_true')
    ap.add_argument('--host', default=DEFAULT_HOST)
    a = ap.parse_args()

    if a.pull: pull(a.host)
    if not os.path.exists(a.file): sys.exit(f"no such file: {a.file} (use --pull)")

    cid = parse_int(a.cid); sid = parse_int(a.sid)
    sidm = (sid & 0x7fff) if sid is not None else None
    fa = fb = None
    if a.frames:
        fa, fb = (int(x) for x in a.frames.split('-'))

    recs = []
    with open(a.file) as f:
        for ln in f:
            try: recs.append(json.loads(ln))
            except: pass
    if not recs: sys.exit("empty/unparseable file")
    print(f"{len(recs)} frames  [{recs[0]['frame']}..{recs[-1]['frame']}]  file={a.file}\n", file=sys.stderr)

    def keep(o):
        if cid is not None and o.get('owner_cid') != cid: return False
        if a.kind and o.get('kind') != a.kind: return False
        if sid is not None:
            s = o.get('sprite_id', -1)
            if s != sid and (s & 0x7fff) != sidm: return False
        return True

    if a.traj:
        for r in recs:
            if fa is not None and not (fa <= r['frame'] <= fb): continue
            line = [f"0x{o['sprite_id']:x}@({o['screen_xy'][0]:.0f},{o['screen_xy'][1]:.0f})"
                    f"s{o['scale'][0]:.2f}f{o['facing']}" for o in r['objects'] if keep(o)]
            if line: print(f"f{r['frame']}: " + "  ".join(line))
        return

    # summary: distinct (cid:sid:kind) -> count, and bbox of screen_xy
    seen = Counter(); box = defaultdict(lambda: [9e9,9e9,-9e9,-9e9])
    for r in recs:
        if fa is not None and not (fa <= r['frame'] <= fb): continue
        for o in r['objects']:
            if not keep(o): continue
            k = f"cid{o['owner_cid']}:0x{o['sprite_id']:x}:{o['kind'][:3]}"
            seen[k] += 1
            x, y = o['screen_xy']; b = box[k]
            b[0]=min(b[0],x); b[1]=min(b[1],y); b[2]=max(b[2],x); b[3]=max(b[3],y)
    if not seen: print("(no objects match the filter)"); return
    for k, c in seen.most_common():
        b = box[k]
        print(f"  {k:24s} x{c:<4d} screen bbox=({b[0]:.0f},{b[1]:.0f})-({b[2]:.0f},{b[3]:.0f})")

if __name__ == '__main__':
    main()
