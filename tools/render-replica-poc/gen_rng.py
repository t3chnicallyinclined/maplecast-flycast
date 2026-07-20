#!/usr/bin/env python3
"""Transpile the MVC2 RNG (loc_8c11e730, bank11) through the FULL lift.py/codegen.py
pipeline and emit gen_rng.c. This is the M3a->M3b bridge: proving the extended
transpiler (the game-tick opcode set) produces byte-exact C for a REAL MVC2
function that has a known-good golden reference (seed 0xF52F6415 -> 0x92EA332A,
ret 0x12EA — from the hand-Zig milestone-1 executor and flycast).

Pool-pointer normalization: a #data of the form `bankNN.loc_<hex>` denotes the
ADDRESS 0x<hex> (marvelous2 labels ARE addresses). The RNG's loc_8C11E7AC pool
is `bank16.loc_8c16BC2c` = the seed variable at 0x8C16BC2C — a DATA pointer, not
a jsr target — so we resolve it to its numeric address before codegen (otherwise
codegen would leaf-tag it as a code pointer)."""
import re
from lift import parse_asm, extract_block, slurp_function
from emit_func import emit_function

BANK11 = r"C:\Users\trist\projects\_marv_re\build\bank11.asm"

def main():
    text = slurp_function(BANK11, None)
    body   = extract_block(text, 35173, 35188)   # loc_8c11e730 .. lds.l @r15+,macl
    consts = extract_block(text, 35250, 35259)   # the 4 pool #data words
    insns, data = parse_asm(body + "\n" + consts)

    # marvelous2 label == address: normalize data-pointer pools bankNN.loc_<hex> -> 0x<hex>
    for k, v in list(data.items()):
        m = re.fullmatch(r'bank\d+\.loc_([0-9a-fA-F]{8})', v)
        if m:
            data[k] = '0x' + m.group(1)

    def noleaf(reg):
        raise RuntimeError("RNG is a leaf — no jsr expected")

    cbody = emit_function(insns, data, "rng_e730", noleaf)
    # Functions communicate via the ctx (regs + RAM), not a C return value:
    # the RNG result lands in c->r[0] and the new seed is written to RAM.
    with open("gen_rng.c", "w") as f:
        f.write('#include "sh4ctx.h"\n')
        f.write('/* AUTO-GENERATED from bank11 loc_8c11e730 via lift.py/codegen.py */\n')
        f.write('void rng_e730(Sh4Ctx*c){\n')
        f.write(cbody)
        f.write('\n}\n')
    print("emitted gen_rng.c")
    print("DATA:", data)

if __name__ == '__main__':
    main()
