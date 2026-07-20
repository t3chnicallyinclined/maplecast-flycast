#!/usr/bin/env python3
"""Map the ticktrace's executed SPL PCs (0x0CE3xxxx) -> real SPL functions in the
per-character move-program disassembly (char_prg/code/S_PLxx.asm), emitting a
worklist in the SAME format as funclist.txt so gen_tick.py can batch-transpile them.

SPL functions are NOT ';===='-delimited (S_PL17 has zero separators), so we use the
marvelous2 SPL naming convention as the boundary:
    BEG_<hex> / FUN_<hex>  = function entry (jsr/bsr/jmp-through-PTR target)
    LAB_<hex>              = internal branch target (bt/bf/bra)  -> stays inside the fn
    DAT_<hex> / PTR_<hex>  = literal-pool word (#data)           -> the fn's pools

Per-slot relocation: the SPL is position-dependent (buildSPL.sh assembles a separate
.BIN per slot A..F). Storm loads at slot A (base 0xce30000, reloc 0); Cable at slot B
(base 0xce38000, reloc +0x8000). Source labels in BOTH files are written ce30xxx, so
the RUNTIME address = source_label + reloc. Executed-PC ranges confirm the slots."""
import re, os

CODE = r"C:\Users\trist\projects\_marv_re\char_prg\code"
REAL = r"C:\Users\trist\projects\maplecast-flycast\tools\render-replica-poc\realcore"
OUT  = r"C:\Users\trist\AppData\Local\Temp\claude\c--Users-trist-projects-maplecast-flycast\ffa8329c-44cf-4fbe-a9d0-0eebc4f87c12\scratchpad\spl_funclist.txt"

# (file, runtime_base, reloc, name)
SPLS = [
    ("S_PL2A.asm", 0x0CE30000, 0x00000, "Storm"),
    ("S_PL17.asm", 0x0CE38000, 0x08000, "Cable"),
]

CODE_LABEL = re.compile(r'^(BEG_|FUN_)([0-9a-fA-F]+):')
DATA_RE    = re.compile(r'^#data\b')

def load_distinct():
    d = set()
    for ln in open(os.path.join(REAL, 'trace_distinct_pcs.txt')):
        ln = ln.strip()
        if ln:
            d.add(int(ln, 16) & 0x0FFFFFFF)
    return d

def load_entries():
    ev = []   # (masked_pc, count, firstidx)
    for ln in open(os.path.join(REAL, 'trace_entries.txt')):
        ln = ln.strip()
        if not ln or ln.startswith('#'):
            continue
        p = ln.split()
        ev.append((int(p[0], 16) & 0x0FFFFFFF, int(p[1]), int(p[2])))
    return ev

def scan(path, reloc):
    lines = open(path, errors='replace').read().splitlines()
    ents = []
    for i, l in enumerate(lines):
        m = CODE_LABEL.match(l.strip())
        if m:
            ents.append((int(m.group(2), 16), i + 1))   # (src_addr, 1-based line)
    ents.sort()
    funcs = []
    for k, (src, ln) in enumerate(ents):
        last = (ents[k + 1][1] - 1) if k + 1 < len(ents) else len(lines)
        dl = [j + 1 for j in range(ln - 1, last) if DATA_RE.match(lines[j].strip())]
        j_, b_, p_, r_ = False, False, False, False
        for j in range(ln - 1, last):
            s = lines[j].strip()
            mn = s.split(None, 1)[0] if s and not s.startswith(';') else ''
            if mn == 'jsr': j_ = True
            elif mn in ('bsr', 'bsrf'): b_ = True
            elif mn == 'jmp': p_ = True
            elif mn == 'braf': r_ = True
        funcs.append(dict(runtime=src + reloc, entry_line=ln, last_line=last,
                          data=(dl[0], dl[-1]) if dl else None,
                          jsr=j_, bsr=b_, jmp=p_, braf=r_))
    funcs.sort(key=lambda f: f['runtime'])
    return funcs

def main():
    distinct = load_distinct()
    entries = load_entries()
    rows = []
    for fname, base, reloc, cname in SPLS:
        funcs = scan(os.path.join(CODE, fname), reloc)
        hi_end = base + 0x8000
        runtimes = [f['runtime'] for f in funcs]

        def find(pc):
            best = None
            for idx, f in enumerate(funcs):
                if f['runtime'] <= pc:
                    best = idx
                else:
                    break
            return best

        exec_idx = {}
        for pc in sorted(distinct):
            if base <= pc < hi_end:
                idx = find(pc)
                if idx is not None:
                    exec_idx.setdefault(idx, set()).add(pc)
        # entry evidence for ordering + nENTRIES
        ev = {}
        for pc, cnt, fi in entries:
            if base <= pc < hi_end:
                idx = find(pc)
                if idx is not None:
                    ev.setdefault(idx, []).append((pc, cnt, fi))

        n_exec = len(exec_idx)
        print(f"# {cname} {fname}: {len(funcs)} functions, {n_exec} executed  "
              f"(reloc +0x{reloc:x}, range 0x{base:07x}-0x{hi_end:07x})")

        for idx in exec_idx:
            f = funcs[idx]
            e = sorted(ev.get(idx, []), key=lambda x: x[2])
            firstidx = e[0][2] if e else 10 ** 9
            kind = 'ROOT' if (f['jsr'] or f['bsr'] or f['braf']) else 'LEAF'
            if f['jmp']:
                kind += '+JMP'
            drange = f"{f['data'][0]}-{f['data'][1]}" if f['data'] else "-"
            rows.append((firstidx, cname,
                f"loc_{f['runtime']:08x} {fname} body_lines={f['entry_line']}-{f['last_line']} "
                f"data={drange} nENTRIES={len(e)} firstidx={firstidx} {kind} "
                f"jsr={int(f['jsr'])} bsr={int(f['bsr'])} jmp={int(f['jmp'])} braf={int(f['braf'])}"))

    rows.sort(key=lambda r: (r[1], r[0]))   # group by char, then first-exec order
    with open(OUT, 'w') as fo:
        fo.write(f"# executed SPL functions: {len(rows)}  (Storm S_PL2A reloc 0 | Cable S_PL17 reloc +0x8000)\n")
        fo.write("# NOTE: loc_<hex> is the RUNTIME address (source label + reloc). Body/data line\n")
        fo.write("# numbers are SOURCE lines in char_prg/code/<file>. gen_tick must apply the per-file\n")
        fo.write("# reloc when generating labels/branch-targets/internal-#data-pointers (NOT loc_8c.. imports).\n")
        for _, _, line in rows:
            fo.write(line + "\n")
    print(f"\nwrote {len(rows)} rows -> {OUT}")

if __name__ == '__main__':
    main()
