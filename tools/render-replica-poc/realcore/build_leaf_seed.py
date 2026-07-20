#!/usr/bin/env python3
"""Build an RTSEED02 seed to run ONE self-contained game-tick leaf through the
realcore oracle (flycast's real interpreter, runner.exe --leaf) from a RAM
snapshot. The oracle's final ctx/RAM = flycast GROUND TRUTH to diff the transpiled
executor against.

  build_leaf_seed.py <entryPC hex> <ram.bin> <out.seed>
  runner.exe <out.seed> --leaf --min-ctx --no-isolate

Entry ctx = the captured 512B Sh4Context in ctx_embed.txt (valid SR/FPSCR mode);
runner --min-ctx zeroes r0..r14 and --leaf forces r15=spEntry, pr=retPc sentinel
so the leaf's rts returns to the stop point. Self-contained leaves take no register
args, so zeroed GP regs + a scratch SP is a faithful entry state."""
import sys, struct, re

SP_ENTRY = 0x8CFF0000   # scratch stack pointer
RET_PC   = 0x8C000000   # rts return sentinel (onPc stops here; != any leaf entry)

def parse_ctx_embed(path):
    hexes = re.findall(r'0x([0-9a-fA-F]{2})', open(path).read())
    b = bytes(int(h, 16) for h in hexes)
    assert len(b) >= 512, f"ctx_embed only {len(b)} bytes (need 512)"
    return b[:512]

def main():
    if len(sys.argv) != 4:
        print(__doc__); sys.exit(2)
    entryPC = int(sys.argv[1], 16)
    ram = open(sys.argv[2], 'rb').read()
    assert len(ram) == 0x1000000, f"ram is {len(ram)} bytes (need 16MB)"
    ctx = parse_ctx_embed("ctx_embed.txt")
    ccn = b'\x00' * 72   # sizeof(g_ccn)=18*4; leaf touches no MMIO/store-queue
    with open(sys.argv[3], 'wb') as f:
        f.write(b'RTSEED02')
        f.write(struct.pack('<IIIII', entryPC, SP_ENTRY, RET_PC, 512, 0x1000000))
        f.write(struct.pack('<I', len(ccn))); f.write(ccn)
        f.write(ctx)
        f.write(ram)
    print(f"wrote {sys.argv[3]}: entryPC=0x{entryPC:08X} spEntry=0x{SP_ENTRY:08X} retPc=0x{RET_PC:08X}")

if __name__ == '__main__':
    main()
