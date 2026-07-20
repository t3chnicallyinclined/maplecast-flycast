# mvc2-executor — the thin SH4 game-tick executor

Runs MVC2's per-frame game-tick from a RAM snapshot, byte-exact vs flycast, as a
**thin portable executor** (Zig → wasm) so the browser can do client-side
prediction/rollback. This is the payoff of the whole feasibility investigation
(see `memory/project_thin_sh4_executor_scoped.md`): all four gates are green —
opcode parity (Zig FP+integer bit-exact native+wasm), determinism (RNG pure-RAM),
completeness (R2 action-verified), and timer inertness (R1: gameplay
byte-identical under a forced-constant timer, masking 3 isolated timing values).

## Architecture

- A flat **little-endian** 16 MB guest RAM + a minimal SH4 context (`r[16]`, `macl`,
  `pr`, …). Guest area-3 addresses (`0x8C……`) mask to the RAM offset.
- Game-logic routines transpiled from the marvelous2 disassembly, each mirroring
  flycast's **determinism-validated interpreter** semantics — the exact recipe
  proven bit-exact in `tools/sh4-fpu-zig-spike/` (f64-accumulate FP, fp-contract
  off; `fmac`→`@mulAdd`; `ftrc` clamp; wrapping integer ops). Hand-transpiled to
  start; the `tools/render-replica-poc/` `lift.py`/`codegen.py` pipeline (already
  byte-exact for the render subtree) automates the ~11K-instruction tick.
- Input injected at `0x8C2681DC` (`Input_DEC`, 20 B/player × 4). The 3 timing
  values (`0x8C2D5748`, `0x8C32DBAC`, `0x8C268250`) are masked — proven
  non-cascading by R1.

## Milestones

1. **DONE** — `Rng_function` (`loc_8c11e730`) runs byte-exact from a real snapshot.
   `executor.zig`: reads the seed `0x8C16BC2C` from `_ram_f90.bin`, runs the LCG,
   matches flycast (`seed 0xF52F6415 → 0x92EA332A, ret 0x12EA`). Proves the
   scaffold: load snapshot → run a real MVC2 routine → byte-exact.
2. NEXT — `Input_Translate` (`loc_8c010080`): pad RAM → `Input_DEC` (uses `fmac`).
3. Transpile the game-tick call tree (shared engine + loaded SPL) via `lift.py`.
4. **Test A** — one whole tick byte-exact vs flycast (the `_ram_f9x.bin` triples).
5. Chain ticks → whole match → **wasm** → browser prediction.

## Build / run (milestone 1)

`seed_page.bin` is a 4 KB slice of MVC2 RAM (ROM-derived → **gitignored, never
commit**). Extract it, then build:

```bash
# extract the seed page from a full-RAM dump (tools/render-replica-poc/_ram_f90.bin)
node -e 'const fs=require("fs");const p=Buffer.alloc(0x1000);fs.readSync(fs.openSync("../render-replica-poc/_ram_f90.bin","r"),p,0,0x1000,0x16B000);fs.writeFileSync("seed_page.bin",p)'
zig build-exe -OReleaseFast executor.zig && ./executor   # -> "byte-exact from snapshot"
```

The routines are pure Zig with no allocations/threads, so the same source compiles
to `wasm32-freestanding` for the browser (as the opcode-parity spike proved).
