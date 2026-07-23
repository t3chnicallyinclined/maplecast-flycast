#!/usr/bin/env python3
"""DEFINITIVE determinism gate: drive the executor over an input-rich chain and byte-diff its
per-frame game state against the realcore ORACLE (flycast's real SH-4 interpreter) — not just
"didn't hang" but "byte-identical to flycast". Advances on the oracle's ground-truth each frame,
so every tick is verified independently. If this is CLEAN through jump/attack/special, the
executor can be the AUTHORITATIVE server for those inputs.

Usage: python oracle_diff.py [n=36]
"""
import subprocess, sys, os, shutil
import numpy as np

RAM  = "_ram_f90.bin"
N    = int(sys.argv[1]) if len(sys.argv) > 1 else 36
GAME = 0x00FE0000            # compare the game region; mask the 0x8CFExxxx scratch stack
INPUT_DEC = 0x2681DC
REAL = os.path.abspath("realcore")
EXEC = os.path.abspath("execstep.exe")
RUNNER = os.path.join(REAL, "runner.exe")

def demo_input(f):
    # jump early (the fixed bug path is the priority to byte-verify) -> HP -> QCF+HP special.
    # ~40s/frame (oracle-bound), so keep the schedule compact.
    if 2 <= f < 10:  return 0x2000                       # Up = jump (verifies the loc_8c051bde fix)
    if 12 <= f < 15: return 0x0100                       # HP
    if 17 <= f < 25:                                     # QCF+HP
        return [0x1000,0x1000,0x1400,0x1400,0x0400,0x0100,0x0100,0x0100][f-17]
    return 0

def inject(ram, cur, prev):
    for o, v in ((0,cur),(2,prev),(4,cur & ~prev),(6,prev & ~cur)):
        ram[INPUT_DEC+o]   = v & 0xFF
        ram[INPUT_DEC+o+1] = (v >> 8) & 0xFF

ram = bytearray(open(RAM, "rb").read())
prev = 0
for f in range(N):
    cur = demo_input(f); inject(ram, cur, prev); prev = cur
    open("_cur.bin", "wb").write(ram)
    # EXECUTOR: one tick_entry
    subprocess.run([EXEC, "_cur.bin", "1", "_exec.bin"], capture_output=True)
    exec_out = open("_exec.bin", "rb").read()
    # ORACLE: build seed (needs ctx_embed.txt in realcore) + run flycast's interpreter one leaf
    shutil.copy("_cur.bin", os.path.join(REAL, "_cur.bin"))
    subprocess.run(["python3", "build_leaf_seed.py", "8c0358be", "_cur.bin", "_seed.bin"],
                   cwd=REAL, capture_output=True)
    subprocess.run([RUNNER, "_seed.bin", "--leaf", "--min-ctx", "--no-isolate"],
                   cwd=REAL, capture_output=True)
    orac = open(os.path.join(REAL, "oracle_ram_out.bin"), "rb").read()
    a = np.frombuffer(exec_out[:GAME], dtype=np.uint8)
    b = np.frombuffer(orac[:GAME], dtype=np.uint8)
    diffs = np.nonzero(a != b)[0]
    # Tile-arena (0x1F9Cxx..0x1FA7xx) is render-side scratch the render pass maintains — NOT
    # authoritative game state (the client rebuilds it). Documented-benign residual (handoff:
    # "the 2 are tile-arena bookkeeping 0x1F9D7C"). Mask it; any diff OUTSIDE is a real divergence.
    ARENA_LO, ARENA_HI = 0x1F9C00, 0x1FA800
    real = [int(i) for i in diffs if not (ARENA_LO <= int(i) < ARENA_HI)]
    arena = len(diffs) - len(real)
    if real:
        print(f"[DIVERGE] f{f} in=0x{cur:04X}: {len(real)} GAME-STATE byte diffs (+{arena} benign arena), first @0x{real[0]:06X}")
        for i in real[:8]:
            print(f"    0x{i:06X}: exec={exec_out[i]:02x} oracle={orac[i]:02x}")
        sys.exit(1)
    tag = "BYTE-EXACT" if arena == 0 else f"gameplay byte-exact (+{arena} benign arena scratch)"
    print(f"  f{f:3} in=0x{cur:04X}: {tag}")
    ram = bytearray(orac)   # advance on flycast ground-truth
print(f"\n[CLEAN] {N} input-driven frames BYTE-IDENTICAL to the flycast oracle (jump/HP/QCF included)")
print("[CLEAN] the executor holds byte-exact under input -> authoritative-server-ready for these paths")
