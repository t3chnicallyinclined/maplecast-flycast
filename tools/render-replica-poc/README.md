# Option C PoC — lift MVC2's body walker SH4 → C, diff vs ground truth

Proof-of-concept for `docs/RENDER-REPLICA-PLAN.md` §8 "SMALLEST FIRST MILESTONE":
demonstrate the **lift-to-C transpiler** path by mechanically translating MVC2's
real per-frame body-render SH4 code to native C, running it, and diffing its output
against engine ground truth. This de-risks the whole "reproduce the render block
mechanically" thesis (Option C in the plan) — the FP/integer SH4 semantics, the
flat-RAM memory model, and the lifter — cheaply, as a **native C diff**, no wasm,
no Phase-0, no browser.

## Result — GO

| Validation | What it proves | Result |
|---|---|---|
| **Leaf `loc_8C11E460`** (bank11, 24 insns) | lifter + FP semantics on a self-contained function (zero input ambiguity) | **21/21 bit-exact** vs an independent `floorf` reference |
| **Transform core `loc_8c0347c8..loc_8c034864`** (the per-tile FP+integer screen math, simple path) | the lifted `float`/`fmul`/`fadd`/`exts.w` chain reproduces the engine's per-part `screenX/screenY` | **18/18 bit-exact** (X and Y) vs reference float; **within the ASMTRACE's own 0.01px logging quantization** vs the real engine output |
| **Full walker `loc_8c0344d4`** (bank03, 464 insns / 20 BBs — BOTH the simple and the scaled/rotated path, full record+tile loops, all 4 leaf dispatch sites) | the lifter produces a structurally correct, self-contained C unit for the entire function | **compiles, links, runs, terminates with a balanced stack (r15 delta = 0), leaf dispatch fires** |
| **Full walker NUMERIC `loc_8c0344d4`** (the COMPLETE pen + REAL tiling + transform, end-to-end, with INPUT-INDEPENDENT descriptors) | the entire walker reproduces every emitted tile's `screenX/screenY` when fed the REAL load-time tile-descriptor table read out of a live prod RAM dump (`_ryu_capture/mc_ram_dump.bin` @`0x8C1F9F9C`) | **9/9 tiles 0.00px-relative** (maxdX=0.0034px, maxdY=0.0043px — both under the trace's 0.01px logging ULP); negative control (descriptors zeroed) collapses to 4 wrong tiles → proves non-circular |

**Bottom line:** straight C `float` arithmetic is **bit-exact** for this render
routine's `+ − ×` chain; the single opcode that needed special-casing is `fmac`
(→ `fmaf`, fused single-rounding — 5 sites). The transpiler approach is **PROVEN**
for the body path.

## Run it

```
tools\render-replica-poc\run.cmd        REM Windows + MSVC (vcvars64 auto-invoked)
```
Outputs the three validations above. Requires Python 3 + numpy and MSVC (cl.exe).

## How it works (the pipeline)

```
SH4 disasm (marvelous2 bank03/bank11)
   │  lift.py        parse labels/insns/#data pools (the lifter front-end)
   │  codegen.py     per-opcode C emitter — semantics harvested from flycast's
   │                 determinism-validated interpreter (core/hw/sh4/interpr/*.cpp)
   ▼  gen_*.py       emit C: each SH4 fn -> one C fn over Sh4Ctx + flat ram[16MB];
                     each BB -> a C label/goto; SH4 delay slots emitted BEFORE the
                     branch effect; jsr -> resolved leaf dispatch.
gen_leaf.c / gen_walker.c / gen_transform.c   (AUTO-GENERATED — do not edit)
   │  sh4ctx.h       Sh4Ctx{r,fr,xf,fpscr,fpul,pr,macl,sr_t,gbr} + big-endian
   │                 area-3 RAM accessors (translate(a)=a&0x00FFFFFF)
   ▼  test_*.c       build the input image + run + diff
```

### Files
- `lift.py` — disasm parser (labels, operands incl `@(disp,Rn)` / `@(R0,Rn)` /
  `@Rn+` / `@-Rn` / PC-pool, paren-aware arg split).
- `codegen.py` — the per-mnemonic C emitter (~45 opcodes). Notable bit-exact choices:
  `fmac→fmaf` (FUSED, single rounding — matches flycast `sh4_fpu.cpp:559`),
  `ftrc→(u32)(s32)f` with the `0x7fffff80` overflow clamp (`sh4_fpu.cpp:522`),
  `mov #imm`/`add #imm` 8-bit **sign extension** (load-bearing — e.g. `add 0xE4,r0`
  is `+(-28)` and is how the walker computes `node+0x144` from `node+0x160`),
  `mov.w/mov.b` sign-extending loads, `muls.w` 16×16→32.
- `emit_func.py` — generic function-level control-flow emitter (delay-slot ordering).
- `gen_leaf.py` / `gen_walker.py` / `gen_transform.py` — drive the lifter over the
  three target ranges; resolve `jsr @rN` to the named leaves via pool-word tags.
- `sh4ctx.h` — runtime context + flat-RAM (area-3 only; no MMIO/MMU/virtmem, per the
  Option-C scope note "tree touches only area-3 RAM+VRAM").
- `test_leaf.c` / `test_transform.c` / `test_walker_compile.c` — harnesses.
- `make_transform_test.py` / `build_image.py` — recover inputs from the ASMTRACE.
- `leaves.c` — the scaled-path trig leaves (stubbed; unused on the simple path) +
  the bank12 submit stub.

## Ground truth & input reconstruction (honest scope)

Ground truth = `_ryu_capture/asm_angled_fist.log` — the ASMTRACE, which logs the
body walker's per-part output **at PC `0x8C034864`** (= `loc_8c034864`, the point
right after the simple-path transform writes `screenX@(0x30,r15)`/`screenY@(0x34,r15)`).
Columns: `frame sid slot cid sel dx dy accX accY screenX screenY pal row flip flags
r11 r13 node`. We use object **cid 23** (node `0c2688e4` = P2C1), **frame 10775**,
which renders 18 body tiles across 5 GFX2 records (one record tiles 2×4).

**What is independently recovered from the trace (no circularity):**
- Pen accumulation `accX -= dx`, `accY -= dy` per record — confirmed directly against
  the logged `accX/accY` and the disasm (`sub r5,r10` / the `@(0x14,r15)` accumulate).
- `scaleX = 1.666875 (≈5/3)`, `baseX = 533.00` — recovered by regressing the
  first-tile-of-record `screenX` on `accX`, **residual 0.000px** (clean).
- `scaleY = 2.142857 (=15/7)`, `baseY = 116.57` — recovered by the joint solve that
  forces every `Iy = (screenY−baseY)/scaleY` to an integer (max frac err 0.0027 =
  trace rounding).
- Per-tile integer indices `Ix = round((screenX−baseX)/scaleX)`,
  `Iy = round((screenY−baseY)/scaleY)` — exact integers.

The **transform-core test** feeds these recovered integers + scale/anchor into the
transpiled C and checks it reproduces `screenX/screenY`. Both the **bit-exact-vs-
reference-float** check (18/18, quantization-free) and the **vs-real-engine** check
(at/under the trace's 0.01px logging ULP) pass — so the lifted FP chain *is* the
engine's arithmetic.

**FULL NUMERIC WALKER — CLOSED 2026-06-12 (`test_walker_dump.c` + `build_image_dump.py`).**
The earlier gap — a **full numeric** run driving pen→tiling→transform end-to-end from
raw bytes — needed the **tiling descriptor table `0x8C1F9F9C`** the per-tile code reads
(`loc_8c0344d4` entry: `r13 = *(node+0xDC)*4 + 0x8C1F9F9C`; per tile `loc_8c03478e`
reads `m=byte[0]`, `pitchX=byte[2]`, `pitchY=byte[3]`, count `=byte[1]+1` from `r13`).
We now HAVE it: **`_ryu_capture/mc_ram_dump.bin`** is a 16MB main-RAM image from live
prod whose `0x8C1F9F9C` table holds 9 REAL load-time descriptors (idx 0..8).

Test object = **cid 23, frame 10766** (the ASMTRACE's only frame whose descriptors fall
entirely in the dump-resident idx 0..8 — `node+0xDC=0`, `r13` walks `0x8C1F9F9C..0x8C1F9FBC`):
9 GFX2 records' record-level data (`dx/dy/flags/sel`) come from the trace, the **tile
descriptors (count/pitch) are read straight out of the dump** (load-time-real, NOT
reconstructed), and anchor/scale are recovered from the trace (`scaleX=5/3`, `baseX=533`,
`scaleY=15/7`, `baseY=floor(node+0xE4)=333`). Running the transpiled `walker_0344d4` over
this image reproduces all **9 emitted tiles at 0.00px-relative** (maxdX 0.0034px / maxdY
0.0043px, both under the trace's 0.01px ULP).

**Non-circularity is proven two ways:** (1) `build_image_dump.py` prints the real
descriptor bytes and confirms each record's `count` (from descriptor `byte[1]+1`) equals
the trace's tile count BEFORE the diff runs; (2) the harness's `zerodesc` negative control
wipes the table → the walker collapses to 4 wrong tiles, so the pass genuinely depends on
the dump's load-time descriptors.

**Scope note for the OTHER objects:** every *other* trace frame (incl. the Sentinel
sid-0x131 rocket, 19 parts) uses `r13` at table idx ≥ 36, which is ZERO in this particular
dump — `0x8C1F9F9C` is a **rolling per-frame scratch table** the engine refills per object
via `node+0xDC`, and only the first object's descriptors (idx 0..8) survived at the base in
this static snapshot. So the rocket frame can't be diffed against THIS dump (its transient
descriptors aren't present); cid23 frame 10766 is the test whose REAL descriptors ARE in the
dump, and it closes the full-walker thesis: **pen + REAL tiling + FP transform reproduced
end-to-end through mechanically-transpiled C with fully independent input.**

## Opcode notes (FP-exactness — the plan's flagged risk)

- **`fmac` → `std::fmaf`** is the ONE opcode requiring special handling: flycast uses
  `std::fma` (single rounding); a naive `fr0*frm + frn` rounds twice and would diverge.
  5 sites in the walker. **This is the only place straight C float ops are not the
  literal translation.**
- All other FP (`fadd/fsub/fmul/fdiv/fabs/fneg/float/ftrc/fcmp/fldi0/fldi1`) lift to
  the obvious C `float` op and are **bit-exact** here. Compiled `/fp:precise` (no
  contraction, ordered eval). No FPSCR rounding-mode juggling was needed for this
  subtree (RM=0, single precision throughout — consistent with the §8 scope analysis).
- Integer subtleties that mattered: 8-bit immediate **sign extension** on `mov #imm`/
  `add #imm`; sign-extending `mov.w`/`mov.b` loads; `exts.w` before the `float`;
  `muls.w` as 16×16→32. All handled by the emitter; the leaf and transform results
  confirm them.

## Generalization

The lifter is a **reusable generator over the disasm** (operand parser + per-mnemonic
table + delay-slot-aware control-flow emitter), not a hand-port. Extending to the full
114-function / ~9,850-insn render tree (§8) means: add the remaining opcodes
(`ftrv`/`frchg`/`fsca`/`fschg` — the matrix/trig core), wire the 2 enumerable ROM jump
tables + the 1 RAM vtable as `switch`es, and transpile the trig leaves
(`loc_8c11e2e0`/`loc_8c11e860`, present in bank11 — sin/cos via the 2π/π-2 constants)
and the bank12 submit. No general indirect-jump resolver is required (§8 confirmed
~96% statically resolved).
