#!/usr/bin/env python3
"""COMPLETE-transpile enumerator: emit EVERY ';===='-delimited code section in a set of
marvelous2 bank .asm files (and EVERY BEG_/FUN_ SPL function), in the SAME funclist format
gen_tick.py consumes. This is the generalization of _handoff/scan_tick.py / scan_spl.py:
those emit only the TRACE-reached functions (a worklist); this emits ALL of them, so every
`jsr @rN` / `jmp @rN` / `bsr` target inside the enumerated banks resolves to real transpiled
code instead of the mc_unknown_call NO-OP. That eliminates the missing-function divergence
class BY CONSTRUCTION for those banks; what remains is only genuine opcode-transpile bugs.

Pure-data sections (0 instructions: literal-pool blocks, jump tables) are SKIPPED so a jsr
into a data region still lands on mc_unknown_call (a real bug), not a silent empty return.

Usage:
  enumerate_funcs.py bank <out.txt> bank03 bank04 bank05 [bank12 ...]
  enumerate_funcs.py spl  <out.txt>            # both SPLs (Storm reloc 0, Cable reloc +0x8000)
"""
import re, sys, os, glob

BUILD = r"C:\Users\trist\projects\_marv_re\build"
SPLDIR = r"C:\Users\trist\projects\_marv_re\char_prg\code"

LABEL_RE = re.compile(r'^(loc_[0-9a-fA-F]+):$')
SEP_RE   = re.compile(r'^;=+')
DATA_RE  = re.compile(r'^#data\b')
# SPL entry labels (BEG_/FUN_ = function entry). LAB_ = internal branch target (stays inside).
SPL_ENTRY_RE = re.compile(r'^(BEG_|FUN_)([0-9a-fA-F]+):')

SPLS = [("S_PL2A.asm", 0x00000), ("S_PL17.asm", 0x08000)]   # Storm slot A, Cable slot B


def enum_bank(path):
    """Yield (runtime_addr, bank, first_line, last_line, data_field) for every CODE section."""
    bank = os.path.basename(path)
    lines = open(path, errors='replace').read().splitlines()
    n = len(lines)
    seps = [i for i, l in enumerate(lines) if SEP_RE.match(l.strip())]
    seps.append(n)
    out = []
    for k in range(len(seps) - 1):
        a, b = seps[k], seps[k + 1]         # 0-based: [a (sep line) .. b (next sep))
        region = lines[a:b]
        entry = None
        data_lines = []
        has_insn = False
        for off, raw in enumerate(region):
            lineno = a + off + 1            # 1-based file line
            s = raw.strip()
            if not s or s.startswith(';'):
                continue
            m = LABEL_RE.match(s)
            if m:
                try:
                    addr = int(m.group(1)[4:], 16) & 0x0FFFFFFF
                except ValueError:
                    continue
                if entry is None:
                    entry = addr
                continue
            if DATA_RE.match(s):
                data_lines.append(lineno)
                continue
            if s.startswith('#'):           # #align/#repeat/etc. — not an instruction
                continue
            has_insn = True                 # a real mnemonic
        if entry is None or not has_insn:    # skip label-less or pure-data sections
            continue
        drange = f"{data_lines[0]}-{data_lines[-1]}" if data_lines else "-"
        out.append((entry | 0x8C000000, bank, a + 1, b, drange))
    return out


def enum_spl(path, reloc):
    """Yield funclist rows for every BEG_/FUN_ SPL function. loc_<runtime> = src+reloc.
    body/data line numbers are SOURCE lines (gen_tick applies reloc via normalize_spl_text)."""
    bank = os.path.basename(path)
    lines = open(path, errors='replace').read().splitlines()
    ents = []
    for i, l in enumerate(lines):
        m = SPL_ENTRY_RE.match(l.strip())
        if m:
            ents.append((int(m.group(2), 16), i + 1))   # (src_addr, 1-based line)
    ents.sort()
    out = []
    for k, (src, ln) in enumerate(ents):
        last = (ents[k + 1][1] - 1) if k + 1 < len(ents) else len(lines)
        data_lines = []
        has_insn = False
        for j in range(ln - 1, last):
            s = lines[j].strip()
            if not s or s.startswith(';'):
                continue
            if DATA_RE.match(s):
                data_lines.append(j + 1)
                continue
            if s.startswith('#'):
                continue
            # a label line? (BEG_/FUN_/LAB_ define, no mnemonic)
            if re.match(r'^(BEG_|FUN_|LAB_|DAT_|PTR_)[0-9a-fA-F]+:$', s):
                continue
            has_insn = True
        if not has_insn:
            continue
        drange = f"{data_lines[0]}-{data_lines[-1]}" if data_lines else "-"
        out.append((src + reloc, bank, ln, last, drange))
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(2)
    mode, outp = sys.argv[1], sys.argv[2]
    rows = []
    if mode == "bank":
        for b in sys.argv[3:]:
            fn = b if b.endswith('.asm') else b + '.asm'
            rows += enum_bank(os.path.join(BUILD, fn))
    elif mode == "spl":
        for fname, reloc in SPLS:
            rows += enum_spl(os.path.join(SPLDIR, fname), reloc)
    else:
        print("mode must be 'bank' or 'spl'"); sys.exit(2)
    rows.sort(key=lambda r: r[0])
    with open(outp, 'w') as f:
        f.write(f"# COMPLETE enumeration ({mode}): {len(rows)} code sections\n")
        for addr, bank, first, last, drange in rows:
            f.write(f"loc_{addr:08x} {bank} body_lines={first}-{last} data={drange}\n")
    print(f"wrote {len(rows)} rows -> {outp}")


if __name__ == '__main__':
    main()
