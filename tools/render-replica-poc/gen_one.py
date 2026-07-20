#!/usr/bin/env python3
"""Transpile ONE self-contained MVC2 game-tick leaf via lift.py/codegen.py.

Usage:
  gen_one.py <bankN.asm> <first_line> <last_line> <fn_name> [cFirst:cLast ...]

  <first_line>..<last_line>  the function body line range (label .. delay slot)
  [cFirst:cLast ...]         extra line ranges holding the function's #data pools

Emits gen_<fn_name>.c with `void <fn_name>(Sh4Ctx*c)`, then verify with:
  zig cc -O2 -DMC_WRITELOG -DLEAF_FN=<fn_name> -I. gen_<fn_name>.c verify_leaf.c -o v.exe
  ./v.exe _ram_f90.bin

This is the per-leaf crank for M3b. Self-contained leaves only (no register args;
a jsr aborts — those are roots, handled later)."""
import re, sys
from lift import parse_asm, extract_block, slurp_function
from emit_func import emit_function


WORK_ASM = r"C:\Users\trist\projects\_marv_re\memory\work.asm"
_work_syms = None

def load_work_symbols():
    """Parse work.asm '#symbol <name> <hex>' lines -> {name: '0x<hex>'}."""
    global _work_syms
    if _work_syms is None:
        _work_syms = {}
        try:
            for line in open(WORK_ASM, errors='replace'):
                m = re.match(r'#symbol\s+(\S+)\s+(0x[0-9a-fA-F]+|\d+)', line)
                if m:
                    val = int(m.group(2), 16) if m.group(2).lower().startswith('0x') else int(m.group(2))
                    _work_syms[m.group(1)] = f'0x{val:x}'
        except FileNotFoundError:
            pass
    return _work_syms

def normalize_data_pointers(data):
    """Resolve #data pool references to numeric constants so codegen emits addresses
    instead of leaf-tagging them as code pointers:
      - bankNN.loc_<hex>  -> 0x<hex>  (marvelous2 label == address; e.g. RNG seed var)
      - work.<Name>       -> the #symbol address from work.asm, e.g.
                             work.GameGlobalPointer -> 0x8c26823c"""
    syms = load_work_symbols()
    for k, v in list(data.items()):
        m = re.fullmatch(r'bank\d+\.loc_([0-9a-fA-F]{8})', v)
        if m:
            data[k] = '0x' + m.group(1)
            continue
        m = re.fullmatch(r'work\.(\S+)', v)
        if m and m.group(1) in syms:
            data[k] = syms[m.group(1)]


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        sys.exit(2)
    path = sys.argv[1]
    first = int(sys.argv[2])
    last = int(sys.argv[3])
    fn = sys.argv[4]
    text = slurp_function(path, None)
    blk = extract_block(text, first, last)
    for spec in sys.argv[5:]:
        a, b = spec.split(':')
        blk += "\n" + extract_block(text, int(a), int(b))
    insns, data = parse_asm(blk)
    normalize_data_pointers(data)

    def noleaf(reg):
        raise RuntimeError(f"{fn}: unexpected jsr/bsr (not a self-contained leaf) — reg {reg}")

    body = emit_function(insns, data, fn, noleaf)
    # forward-declare any bsr callees (sub_<hex>) the body references
    callees = sorted(set(re.findall(r'\b(sub_[0-9a-fA-F]+)\(c\);', body)))
    out = f"gen_{fn}.c"
    with open(out, "w") as f:
        f.write('#include "sh4ctx.h"\n')
        f.write(f'/* AUTO-GENERATED from {path} lines {first}-{last} via lift.py/codegen.py */\n')
        for cal in callees:
            f.write(f'void {cal}(Sh4Ctx*c);\n')
        f.write(f'void {fn}(Sh4Ctx*c){{\n')
        f.write(body)
        f.write('\n}\n')
    if callees:
        print("bsr callees (link a matching sub_<hex>):", ", ".join(callees))
    print(f"emitted {out}")
    print("DATA:", data)


if __name__ == '__main__':
    main()
