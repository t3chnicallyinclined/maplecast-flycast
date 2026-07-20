#!/usr/bin/env python3
"""Map the ticktrace's executed PCs -> real functions delimited by ';====' in the
marvelous2 bank .asm files. Emits, per executed function: entry PC, bank file,
body line range, #data pool line ranges, whether it contains jsr/bsr/jmp/braf
(ROOT vs CLEAN-LEAF), and the entry_count/first_instr evidence for its entries."""
import re, sys, os, glob

BUILD = r"C:\Users\trist\projects\_marv_re\build"
TRACE_DIR = r"C:\Users\trist\projects\maplecast-flycast\tools\render-replica-poc\realcore"

LABEL_RE = re.compile(r'^(loc_[0-9a-fA-F]+):$')
DATA_RE  = re.compile(r'^#data\b')
SEP_RE   = re.compile(r'^;=+')

def load_banks():
    funcs = []            # list of dicts
    label_addr = {}       # 'loc_xxxx' -> addr
    for path in sorted(glob.glob(os.path.join(BUILD, 'bank*.asm'))):
        bankname = os.path.basename(path)
        lines = open(path, errors='replace').read().splitlines()
        n = len(lines)
        # find separator line indices (0-based)
        seps = [i for i,l in enumerate(lines) if SEP_RE.match(l.strip())]
        seps.append(n)
        # each function = region [seps[k]+1 .. seps[k+1]-1]
        for k in range(len(seps)-1):
            a = seps[k]
            b = seps[k+1]
            region = lines[a:b]   # includes the sep line at a
            # gather labels + data in region
            entry_pc = None
            entry_line = None
            labels = []           # (addr, lineno1based)
            data_lines = []       # 1-based line numbers that are #data
            has_call = False
            has_jmp = False
            has_braf = False
            has_jsr = False
            has_bsr = False
            has_rts = False
            has_rte = False
            cur_label = None
            for off, raw in enumerate(region):
                lineno = a + off + 1   # 1-based file line
                s = raw.strip()
                m = LABEL_RE.match(s)
                if m:
                    lab = m.group(1).lower()
                    try:
                        addr = int(lab[4:], 16) & 0x0FFFFFFF
                    except ValueError:
                        cur_label = lab; continue
                    label_addr[lab] = addr
                    labels.append((addr, lineno))
                    if entry_pc is None:
                        entry_pc = addr; entry_line = lineno
                    cur_label = lab
                    continue
                if DATA_RE.match(s):
                    data_lines.append(lineno)
                    continue
                # instruction mnemonic
                mn = s.split(None,1)[0] if s and not s.startswith(';') else ''
                if mn == 'jsr': has_jsr = True; has_call=True
                elif mn == 'bsr' or mn=='bsrf': has_bsr = True; has_call=True
                elif mn == 'jmp': has_jmp = True
                elif mn == 'braf': has_braf = True
                elif mn == 'rts': has_rts = True
                elif mn == 'rte': has_rte = True
            if entry_pc is None:
                continue
            funcs.append(dict(
                bank=bankname, entry=entry_pc, entry_line=entry_line,
                first_line=a+1, last_line=b,   # region incl sep..next
                labels=labels, data_lines=data_lines,
                has_jsr=has_jsr, has_bsr=has_bsr, has_jmp=has_jmp,
                has_braf=has_braf, has_rts=has_rts, has_rte=has_rte,
            ))
    funcs.sort(key=lambda f: f['entry'])
    return funcs, label_addr

def main():
    funcs, label_addr = load_banks()
    # load trace
    distinct = set()
    for ln in open(os.path.join(TRACE_DIR,'trace_distinct_pcs.txt')):
        ln=ln.strip()
        if ln: distinct.add(int(ln,16) & 0x0FFFFFFF)   # normalize P0/P1
    entries = []   # (pc, count, firstidx)
    for ln in open(os.path.join(TRACE_DIR,'trace_entries.txt')):
        ln=ln.strip()
        if not ln or ln.startswith('#'): continue
        p=ln.split()
        entries.append((int(p[0],16)&0x0FFFFFFF, int(p[1]), int(p[2])))

    # build func address ranges: [entry, next_entry)
    ranges = []
    for idx,f in enumerate(funcs):
        lo = f['entry']
        hi = funcs[idx+1]['entry'] if idx+1<len(funcs) else lo+0x100000
        ranges.append((lo,hi,idx))

    def find_func(pc):
        # binary-ish: largest entry <= pc
        lo,hi=0,len(ranges)
        best=None
        for l,h,idx in ranges:
            if l<=pc<h:
                return idx
        return None

    # map distinct PCs to funcs (bank-code only, i.e. 0x0.. area 8/0)
    exec_funcs = {}   # idx -> set of pcs
    unmapped = []
    for pc in sorted(distinct):
        # only bank-code region 0x00xxxxxx of area (mask already applied)
        if pc < 0x01000000 or pc >= 0x02000000:
            # not bankNN code (could be SPL 0x0E3xxxx, or data). skip here
            unmapped.append(pc); continue
        idx = find_func(pc)
        if idx is None:
            unmapped.append(pc); continue
        exec_funcs.setdefault(idx,set()).add(pc)

    # attach entry evidence to funcs
    entry_ev = {}   # idx -> list of (pc,count,firstidx)
    for pc,cnt,fi in entries:
        if pc < 0x01000000 or pc>=0x02000000: continue
        idx=find_func(pc)
        if idx is not None:
            entry_ev.setdefault(idx,[]).append((pc,cnt,fi))

    # print sorted by first execution (min firstidx among entries)
    def sort_key(idx):
        ev=entry_ev.get(idx)
        if ev: return min(e[2] for e in ev)
        return 10**9
    order = sorted(exec_funcs.keys(), key=sort_key)

    print(f"# executed bank functions: {len(order)}")
    print(f"# unmapped distinct PCs (SPL/other area): {len(unmapped)}")
    for idx in order:
        f=funcs[idx]
        ev=sorted(entry_ev.get(idx,[]), key=lambda e:e[2])
        firstidx = ev[0][2] if ev else -1
        kind = 'ROOT' if (f['has_jsr'] or f['has_bsr'] or f['has_braf']) else 'LEAF'
        if f['has_jmp']: kind += '+JMP'
        dl=f['data_lines']
        drange = f"{dl[0]}-{dl[-1]}" if dl else "-"
        loc=f"loc_{f['entry']|0x8C000000:08x}"
        print(f"{loc} {f['bank']} body_lines={f['first_line']}-{f['last_line']} "
              f"data={drange} nENTRIES={len(ev)} firstidx={firstidx} {kind} "
              f"jsr={int(f['has_jsr'])} bsr={int(f['has_bsr'])} jmp={int(f['has_jmp'])} braf={int(f['has_braf'])}")
    # dump unmapped area histogram
    areas={}
    for pc in unmapped:
        areas[pc>>20]=areas.get(pc>>20,0)+1
    print("\n# unmapped PC areas (pc>>20 : count):")
    for a in sorted(areas): print(f"  0x{a:03x}xxxxx : {areas[a]}")

if __name__=='__main__':
    main()
