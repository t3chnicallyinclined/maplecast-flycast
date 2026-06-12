#!/usr/bin/env python3
"""
scan_atlas_coverage.py — "scan every character for every move so we don't miss anything."

For every PLxx character atlas, cross-check the ASSEMBLY table (every sprite_id /
move-pose the engine can select) against the baked PART cells. Reports, per character:
  - assemblies whose referenced part-cell is MISSING from parts.json (would draw a hole)
  - assemblies present in the table but with ZERO parts (empty pose)
  - total assemblies / parts covered

This is the STATIC, exhaustive coverage pass: an assembly index == a sprite_id the
engine resolves via (sid & 0x7FFF) -> GFX2 cell table (ROM rule, loc_8c0344d4 /
loc_8c0348c8). If every assembly's cells are baked, NO move can show a hole — we don't
have to fire each move to find gaps. Behavioral cross-check (which sids each move
actually references) comes from char_prg/code/S_PLxx.asm in a companion pass.

Usage:  python3 tools/scan_atlas_coverage.py [atlas_dir]
        default atlas_dir = web/test-atlas/chars
"""
import json, sys, glob, os, re

ATLAS_DIR = sys.argv[1] if len(sys.argv) > 1 else 'web/test-atlas/chars'

def load(p):
    try:
        with open(p) as f: return json.load(f)
    except Exception:
        return None

def parts_index(parts_json):
    """Return the set of cell-ids present in the parts atlas."""
    if parts_json is None: return None
    pp = parts_json.get('parts', parts_json) if isinstance(parts_json, dict) else parts_json
    if isinstance(pp, dict):
        return set(str(k) for k in pp.keys())
    if isinstance(pp, list):
        # list form: index == cell id
        return set(str(i) for i in range(len(pp)))
    return set()

def asm_table(asm_json):
    if asm_json is None: return None
    return asm_json.get('assemblies', asm_json)

def scan_char(asm_path):
    base = asm_path[:-len('_asm.json')]
    name = os.path.basename(base)
    A = asm_table(load(asm_path))
    cells = parts_index(load(base + '_parts.json'))
    if A is None:
        return name, {'error': 'no _asm.json'}
    if cells is None:
        return name, {'error': 'no _parts.json'}
    missing = {}     # sid -> [missing cell ids]
    empty = []       # sids with 0 parts
    total_parts = 0
    for sid, parts in A.items():
        pl = parts if isinstance(parts, list) else parts.get('parts', [])
        if not pl:
            empty.append(sid); continue
        miss = []
        for q in pl:
            total_parts += 1
            cell = str(q.get('part', q.get('sel', q.get('cell'))))
            if cell not in cells:
                miss.append(cell)
        if miss:
            missing[sid] = miss
    return name, {
        'assemblies': len(A), 'cells': len(cells), 'parts_refs': total_parts,
        'empty': empty, 'missing': missing,
    }

def main():
    paths = sorted(glob.glob(os.path.join(ATLAS_DIR, 'PL*_asm.json')))
    if not paths:
        print(f"no PL*_asm.json in {ATLAS_DIR}"); return
    print(f"scanning {len(paths)} characters in {ATLAS_DIR}\n")
    tot_missing = 0; tot_empty = 0; bad_chars = []
    for p in paths:
        name, r = scan_char(p)
        if 'error' in r:
            print(f"  {name:8s}  ERROR: {r['error']}"); bad_chars.append(name); continue
        nmiss = sum(len(v) for v in r['missing'].values())
        tot_missing += nmiss; tot_empty += len(r['empty'])
        flag = 'OK' if not r['missing'] else f"!! {len(r['missing'])} asm w/ missing cells ({nmiss} refs)"
        print(f"  {name:8s}  asm={r['assemblies']:4d} cells={r['cells']:4d} parts={r['parts_refs']:5d} empty={len(r['empty']):3d}  {flag}")
        if r['missing']:
            bad_chars.append(name)
            for sid, miss in list(r['missing'].items())[:8]:
                print(f"            sid {sid} (0x{int(sid):x}) -> missing cells {miss[:6]}")
    print(f"\nSUMMARY: {len(paths)} chars, {tot_missing} missing part-refs, {tot_empty} empty assemblies")
    print(f"chars needing attention: {bad_chars if bad_chars else 'NONE — full coverage'}")

if __name__ == '__main__':
    main()
