#!/usr/bin/env python3
"""For each executed bank function, enumerate jsr/braf/jmp sites and classify:
  STATIC  : target reg loaded from a fixed @(loc_pool,PC) pool -> resolvable now
  TABLE   : target reg loaded via @(rA,rB) indexed table  -> trace-needed
  COMPUTED: braf/other -> trace-needed
Emits site PC + classification + (for STATIC) the resolved pool target."""
import re, os, glob

BUILD = r"C:\Users\trist\projects\_marv_re\build"
TRACE_DIR = r"C:\Users\trist\projects\maplecast-flycast\tools\render-replica-poc\realcore"
LABEL_RE = re.compile(r'^(loc_[0-9a-fA-F]+):$')

# load executed distinct PCs (normalized low28)
distinct=set()
for ln in open(os.path.join(TRACE_DIR,'trace_distinct_pcs.txt')):
    ln=ln.strip()
    if ln: distinct.add(int(ln,16)&0x0FFFFFFF)

def bank_of(pc): return f"bank{(pc>>16)&0xff:02x}.asm"

# Build per-file line lists + pool #data map (label -> data string)
files={}
pool={}   # 'loc_xxxx' low28 -> resolved string
for path in sorted(glob.glob(os.path.join(BUILD,'bank*.asm'))):
    lines=open(path,errors='replace').read().splitlines()
    files[os.path.basename(path)]=lines
    cur=None
    for l in lines:
        s=l.strip()
        m=LABEL_RE.match(s)
        if m: cur=m.group(1).lower(); continue
        if s.startswith('#data') and cur:
            parts=s.split(None,1)
            if len(parts)>1: pool[cur]=parts[1].strip()
            cur=None

def resolve_pool(label):
    """label like loc_8c0359fc -> address low28 (int) or None."""
    v=pool.get(label.lower())
    if v is None: return None
    m=re.fullmatch(r'0x([0-9a-fA-F]+)',v)
    if m: return int(m.group(1),16)&0x0FFFFFFF
    m=re.fullmatch(r'(?:bank\d+\.)?loc_([0-9a-fA-F]{8})',v)
    if m: return int(m.group(1),16)&0x0FFFFFFF
    return None

# walk each file, track current PC by 2 bytes per instruction, but simpler:
# use the label addresses to anchor. We only need site addresses -> we walk
# instructions assigning addr = last_label_addr + 2*insns_since_label.
def scan_file(fname):
    lines=files[fname]
    addr=None
    prev_movl_pool={}   # reg -> pool label (most recent @(loc,PC) load in this BB)
    prev_table={}       # reg -> 'TABLE'
    results=[]          # (site_addr, kind, detail)
    for l in lines:
        s=l.strip()
        if not s or s.startswith(';') or s.startswith('#'):
            m=LABEL_RE.match(s)
            continue
        m=LABEL_RE.match(s)
        if m:
            lab=m.group(1).lower()
            try: addr=int(lab[4:],16)&0x0FFFFFFF
            except: addr=None
            prev_movl_pool={}; prev_table={}   # new BB boundary at label
            continue
        if addr is None: continue
        body=re.split(r'\s*;',s,1)[0].strip()
        parts=body.split(None,1)
        mn=parts[0]; args=parts[1] if len(parts)>1 else ''
        site=addr
        # track reg loads
        mp=re.match(r'mov\.l\s+@\((loc_[0-9a-fA-F]+),PC\),(r\d+)',body)
        if mp:
            prev_movl_pool[mp.group(2)]=mp.group(1)
            prev_table.pop(mp.group(2),None)
        mt=re.match(r'mov\.l\s+@\((r\d+),(r\d+)\),(r\d+)',body)
        if mt:
            prev_table[mt.group(3)]='TABLE'
            prev_movl_pool.pop(mt.group(3),None)
        # other writes clobber classification (mov rX,rY etc.)
        mm=re.match(r'mov\s+(r\d+),(r\d+)',body)
        if mm:
            prev_movl_pool.pop(mm.group(2),None)
            # inherit table-ness
            if mm.group(1) in prev_table: prev_table[mm.group(2)]='TABLE'
        if mn=='jsr':
            reg=args.strip().lstrip('@')
            if reg in prev_movl_pool:
                tgt=resolve_pool(prev_movl_pool[reg])
                results.append((site,'STATIC',f"{prev_movl_pool[reg]}->{'loc_8c%06x'%(tgt&0xffffff) if tgt else '?'}"))
            elif reg in prev_table:
                results.append((site,'TABLE',reg))
            else:
                results.append((site,'DYN',reg))
        elif mn=='braf':
            results.append((site,'BRAF',args.strip()))
        addr = (addr+2)&0x0FFFFFFF
    return results

# only report sites that are in executed distinct PCs
banks=sorted({bank_of(pc) for pc in distinct if 0x0C010000<=pc<0x0C200000})
allsites=[]
for b in banks:
    if b not in files: continue
    for site,kind,det in scan_file(b):
        if site in distinct:
            allsites.append((site,kind,det))
allsites.sort()
from collections import Counter
c=Counter(k for _,k,_ in allsites)
print("# executed indirect sites by kind:", dict(c))
print("\n# TABLE (indexed jsr) + BRAF (computed) sites — TRACE-NEEDED:")
for site,kind,det in allsites:
    if kind in ('TABLE','BRAF','DYN'):
        print(f"  loc_8c{site&0xffffff:06x} {kind} reg/tgt={det}")
