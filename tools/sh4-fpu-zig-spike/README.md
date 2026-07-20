# SH4 FPU parity spike — Zig reproduces flycast's float ops bit-for-bit

Feasibility proof for a **thin from-snapshot SH4 executor** (for browser
client-side prediction — see `memory/project_thin_sh4_executor_scoped.md`). The
identified hard risk was **bit-exact FPU parity** with flycast; this settles it.

## Result

Zig reproduces flycast's SH4 FP ops **bit-for-bit, native AND wasm**:

| Ops | records × outputs | native | wasm |
|---|---|---|---|
| `ftrv`, `fipr` (`fpu*.zig` + `ref.c`) | 200k × 1M | 0 mismatch | 0 mismatch |
| `fadd fsub fmul fdiv fmac ftrc` (`fpu2*` + `ref2.c`) | 200k × 1.2M | 0 mismatch | 0 mismatch |

## The two load-bearing details

1. **`ftrv`/`fipr` accumulate in `double`, left-to-right, then round to `f32`**
   (flycast `core/hw/sh4/dyna/shil_canonical.h:144` and `interpr/sh4_fpu.cpp:407`).
   Zig: `@as(f64, a)*@as(f64, b) + …` then `@floatCast` to f32, **fp-contract off**
   so nothing fuses to `fma`. A naive f32 accumulation diverges (mantissa noise) —
   this is almost certainly why the earlier render-replica stage-prediction wasn't
   bit-exact.
2. **`ftrc`** (`sh4_fpu.cpp:513`) depends on x86's out-of-range float→int result
   (`0x80000000`) + a `0x7fffff80` clamp. A naive conversion **traps** in wasm, so
   Zig replicates flycast's clamp explicitly (`fpu2*.zig`). `fmac` = `std::fma` =
   Zig `@mulAdd`. flycast does no denormal-flush / rounding-mode emulation, so Zig's
   default IEEE matches.

## Run

Needs Zig (0.16.0 used here) and Node. From this dir:

```bash
ZIG=path/to/zig
# ftrv/fipr
$ZIG cc -O2 -ffp-contract=off ref.c -o ref.exe && ./ref.exe 200000 corpus.bin
$ZIG build-exe -OReleaseFast fpu.zig && ./fpu.exe
$ZIG build-exe -target wasm32-freestanding -O ReleaseFast -fno-entry -rdynamic fpu_wasm.zig -femit-bin=fpu.wasm && node run_wasm.mjs
# scalar FP ops
$ZIG cc -O2 -ffp-contract=off ref2.c -o ref2.exe && ./ref2.exe 200000 corpus2.bin
$ZIG build-exe -OReleaseFast fpu2.zig && ./fpu2.exe
$ZIG build-exe -target wasm32-freestanding -O ReleaseFast -fno-entry -rdynamic fpu2_wasm.zig -femit-bin=fpu2.wasm && node run_wasm2.mjs
```

The C reference is flycast's *extracted* algorithm. The gold-standard next check is
against `tools/render-replica-poc/realcore/` (flycast's actual compiled interpreter).
Remaining opcode risk = integer `div1`/`mac.l`+S-bit (deterministic int, trivially
portable). NEXT milestone = "Test A": one whole game-tick byte-exact via the
realcore runner (see the memory file).
