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


def normalize_data_pointers(data):
    """marvelous2 label == address: a #data of the form bankNN.loc_<hex> that is a
    DATA pointer (e.g. the RNG seed var) denotes the numeric address 0x<hex>. Resolve
    it so codegen emits the address instead of leaf-tagging it as a code pointer."""
    for k, v in list(data.items()):
        m = re.fullmatch(r'bank\d+\.loc_([0-9a-fA-F]{8})', v)
        if m:
            data[k] = '0x' + m.group(1)


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
    out = f"gen_{fn}.c"
    with open(out, "w") as f:
        f.write('#include "sh4ctx.h"\n')
        f.write(f'/* AUTO-GENERATED from {path} lines {first}-{last} via lift.py/codegen.py */\n')
        f.write(f'void {fn}(Sh4Ctx*c){{\n')
        f.write(body)
        f.write('\n}\n')
    print(f"emitted {out}")
    print("DATA:", data)


if __name__ == '__main__':
    main()
