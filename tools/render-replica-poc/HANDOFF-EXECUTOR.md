# HANDOFF — Thin SH-4 Game-Tick Executor (MvC2)

**★ STATUS: TEST A COMPLETE. The whole game-logic tick transpiles + runs, reproducing flycast
BYTE-EXACT — 0 bytes / 16 MB differ (masking only 3 known hw-timer values). The executor IS the
game-tick. NEXT PHASE = chain tick -> render_frame (input -> next-frame TA), then WASM.**

The 52-byte residual was the dropped back-bank XMTRX loader `loc_8c120220` (a `bra`-only target the
worklist scanner missed); adding it (bank12:1-20 in gen_tick.py EXTRA_FUNCS) drove the diff to 0.
See §8 for the full root-cause writeup.

Last updated 2026-07-20. Branch `feat/dataset-exporter`. Latest exec commit `4f93dc647`.
Companion memory: `~/.claude/.../memory/project_thin_sh4_executor_scoped.md` (linked facts).

---

## 1. Goal & thesis

Transpile MvC2's per-frame **game-logic tick** (the SH-4 code between two frames) to C, run it
from a RAM snapshot **byte-exact vs flycast**, then compile to WASM for **browser client-side
prediction/rollback**. This is NOT a full emulator — it's the CPU's game-logic subset only.
Render is a separate, already-byte-exact pipeline (`render_frame`); PVR2 rasterization is the
browser (WebGPU). Endgame: `input → tick(executor) → render_frame → next-frame TA`, off flycast,
in the browser. See memory for the "why not emulate every chip" architecture note.

## 2. Where we are (trajectory)

Executor-vs-flycast game-region diff over this build:
`6,700,000 → 25,565 → 2,612 → 113 → 52 → 0 bytes`. Each drop was a **general, source-verified** fix
(never a patch-over). Test A (flycast form) is PROVEN, and now the OUR-EXECUTOR form passes too:
seeding flycast's own interpreter at the scene-FSM root reproduces the next real frame, AND our
standalone C executor reproduces flycast's tick byte-for-byte — so `_ram_f90` + scene-entry IS a
complete deterministic game-tick domain, and we can now RUN it without flycast.

**271 functions transpile** (250 bank + 21 roster SPL). The executor compiles, runs the whole tick
in ~2,153 dispatches, and diffs **0 bytes** vs the flycast ground truth (masking 3 hw-timer values).
The lone `unknown_call` (0x89889011, from loc_8c17D0C0's jump table) is a non-RAM peripheral dispatch,
dropped by `mc_is_ram` and proven benign (full game RAM byte-exact with it dropped).

## 3. Architecture (the transpile pipeline)

```
marvelous2 disasm (bankNN.asm, label==PC)  +  ticktrace (which fns run, resolved indirects)
        │
   lift.py        parse asm -> Insn[] (+ins.pc), pool #data map. Numeric @(0xNN,PC) pools,
        │         #repeat unroll, named labels, PC anchor from loc_/braf_/<name>_<hex>.
   codegen.py     one C per SH-4 opcode, mirroring flycast's interpreter (determinism-proven).
        │         SZ-aware fmov, div idiom, stc/ldc, etc.
   emit_func.py   control flow: delay-slot LATCH for jsr/jmp/braf/bsrf, branch arms, jmp tail-call.
        │
   gen_tick.py    batch-transpile the WORKLIST -> gen_tick_all.c:
                    - every fn -> fn_<addr>(Sh4Ctx*c, u32 _e)  (RE-ENTERABLE: entry-switch over
                      every basic block, so computed jumps to mid-function addrs dispatch)
                    - global {addr -> fn} dispatch: call_addr(c,a) switch over EVERY block addr
                    - jsr/jmp/bsr/braf ALL route through call_addr (the live engine's RAM-fnptr
                      dispatch; the pointers are in _ram_f90) — NO static jump-table resolution
```

Memory model (`sh4ctx.h`): flat 16 MB little-endian guest RAM; `float fr[16]`, `float xf[16]`
(two FP banks); `mc_is_ram(a)=(a&0x1C000000)==0x0C000000` — only SH-4 **area 3** is main RAM;
**VRAM/TA/MMIO writes are DROPPED** (a memcpy uploading a texture to VRAM must NOT corrupt RAM).

Entry state (self-contained, PROVEN): minimal ctx, `r15=0x8CFF0000` scratch stack, everything
else 0. No captured entry registers needed (verified via `--min-ctx` reproducing the same result).

## 4. Toolchain & build

- **zig** (has `zig cc` for native + wasm): `<session-scratchpad>/zig-x86_64-windows-0.16.0/zig.exe`.
  (Any Zig 0.16 works; it's the C compiler here. Native gcc/clang also fine for the native harness.)
- **MSVC** for the realcore oracle: `realcore/dobuild.bat` (calls vcvars64; NOT `build.cmd` — it
  misses the `core/deps` include → md5.h fail).
- **Disasm**: `C:/Users/trist/projects/_marv_re/build/bankNN.asm` (build box: rise3 `ubuntu@15.204.141.58`; dev0ps `tris@65.109.77.178` has an older copy).
- **SPL**: `C:/Users/trist/projects/_marv_re/char_prg/code/{S_PL2A.asm=Storm,S_PL17.asm=Cable}`.
- **work symbols**: `C:/Users/trist/projects/_marv_re/memory/work.asm` (`#symbol Name 0xADDR`).

### Build + run the whole-tick executor (the main loop)
```bash
cd tools/render-replica-poc
python3 gen_tick.py                                   # -> gen_tick_all.c (270 fns + dispatch)
ZIG cc -O1 -I. gen_tick_all.c test_tick.c -lm -o test_tick.exe
./test_tick.exe _ram_f90.bin _tick_truth.bin          # -> "game-region diff ... 52 bytes"
```
Add `-DMC_WTRAP` to enable the write-address trap in `test_tick.c` (shadow call-stack + reg
capture on first bad write / first unknown dispatch — how every residual was localized).

## 5. Ground-truth files (ALL gitignored — ROM-derived; regenerate on the dev box)

| File | What | How made |
|---|---|---|
| `_ram_f90.bin`..`f93.bin` | 4 consecutive full-16MB RAM captures (frames 46549-46552, in_match) | flycast headless capture |
| `_tick_truth.bin` | flycast's game-logic tick result from `_ram_f90` | `realcore/runner.exe` `--leaf` (below) |
| `realcore/ctx_embed.txt` | a valid 512B Sh4Context (entry mode: fpscr=0x00240000 FR=1, SR=0x60000100) | committed (register state only) |
| `_handoff/funclist.txt` | 249 bank fns: `loc_ bank.asm body_lines=A-B data=C-D class jsr/bsr/jmp/braf` | `scan_tick.py` over the trace |
| `_handoff/spl_funclist.txt` | 21 SPL fns (Storm reloc 0, Cable +0x8000) | `scan_spl.py` |
| `_handoff/trace_*.txt` | the ticktrace (executed PCs / entry targets) | `runner.exe --trace` (below) |

### Regenerate the ground truth + trace (needs `_ram_f90.bin` + built `runner.exe`)
```bash
cd tools/render-replica-poc/realcore
python3 build_leaf_seed.py 8c0358be ../_ram_f90.bin s_scene.bin     # scene-FSM root, entry ctx
./runner.exe s_scene.bin --leaf --no-isolate --trace               # runs flycast's REAL interpreter
#   -> oracle_ram_out.bin (== _tick_truth.bin), trace_out.bin (PC trace)
cp oracle_ram_out.bin ../_tick_truth.bin
# then regenerate worklists (needs the disasm + a python that reads trace_out.bin):
python3 ../_handoff/scan_tick.py     # -> funclist.txt   (RE-expert scanner: ;==== boundaries)
python3 ../_handoff/scan_spl.py      # -> spl_funclist.txt
```
The `--leaf` mode: forces `pr=retPc sentinel + r15=spEntry` so the tick's final `rts` returns to
the stop point; on reaching it, dumps `oracle_ctx.txt` + `oracle_ram_out.bin` = flycast ground truth.

## 6. Verification stack (how to gate any fix)

1. **Leaf byte-exactness** (`verify_leaf.c`): compile one transpiled fn with `-DLEAF_FN=<fn>`, run
   vs a snapshot + optional next-frame truth. Used to prove 18 individual leaves byte-exact and as
   a REGRESSION gate (RNG `seed→0x92EA332A`, nrand `fr0=0.14776611328125`) — run after any codegen
   change. The realcore `--leaf` oracle (Sec 5) verifies conditional/pure/reg-arg fns (`--setr`/`--setfr`).
2. **Whole-tick diff** (`test_tick.c`): the executor vs `_tick_truth.bin`. Masks the 3 timing
   values (0x8C2D5748, 0x8C32DBAC, 0x8C268250 — TCNT0-derived, proven non-cascading) + stack. Target: 0.
3. **Discipline (STRICT, user-mandated): NO GUESSING.** Every fix must be evidence-based (a trap,
   a call-chain, a source citation). Every SH-4 semantic subtlety was VERIFIED against flycast source
   (`core/hw/sh4/interpr/sh4_fpu.cpp`, `sh4_opcodes.cpp`) or the disasm, NOT assumed. Use the RE-expert
   subagents for disasm-grounded analysis (see Sec 9).

## 7. Fix history / changelog (every commit + what it fixed)

| Commit | Fix | Effect |
|---|---|---|
| 32e6f702c | codegen.py opcode gap (div idiom, mul.l, dt, cmp/pz, shld, addc/subc, movt, shlr16, sts/lds macl) | unblocked tick opcodes |
| 735443aec | RNG transpiled byte-exact through the full pipeline (gen_rng.py) | pipeline proven |
| b885db93c | verify_leaf.c + MC_WRITELOG write-set backbone | verification tooling |
| 3a9307e1d | gen_one.py one-command per-leaf transpile (bankNN.loc pool norm) | crank tooling |
| d16f7e627 | emit_func.py **bsr call-tree arm** | subroutine calls |
| ae0f9d5b5 | gen_one.py **work.* pool resolution** | GameGlobalPointer etc. |
| 8d95a19a2 | realcore/runner.cpp **--leaf oracle** (flycast ground truth, local) | verify conditional/pure fns |
| a78ce5981 | runner.cpp **--setr/--setfr** entry-register seeding | verify reg-arg fns (18 total byte-exact) |
| 350628180 | runner.cpp **--trace** + TEST A PASSES (flycast form) | tick enumerated: 8,962 distinct instrs |
| 96fb189f1 | **gen_tick.py whole-tick executor** (dispatch table, numeric pools, SPL) | 212/249; compiles + runs |
| 055f2c8fa | **VRAM/MMIO write-drop** (mc_is_ram) | diff 25K→2.6K (memcpy→VRAM was corrupting RAM) |
| 7989bb1ef | **DELAY-SLOT LATCH** for jsr/jmp/braf/bsrf | diff 2.6K→113 (delay slot clobbered the jump target reg) |
| 56f905ef0 | **SZ-aware fmov** (pair/XD bank, source-verified) | diff 113→52 (fschg SZ=1 → 8-byte pairs into xf[]) |

Also in gen_tick.py (not separately committed): re-enterable functions (entry-switch per block),
label-every-instruction (arbitrary computed-jump targets), braf/bsrf via call_addr, mova
table-address, swap.w/b, ocbi, #repeat, named-label graceful, cross-;==== goto→call_addr tail,
inside-body data-range skip. Transpiled `gen_*.c` + `gen_tick_all.c` are **gitignored** (derived
game code) — regenerate with `gen_tick.py`.

## 8. RESOLVED — the last 52 bytes (root cause + fix)

**Diff is now 0. The 52-byte residual was a DROPPED matrix loader, not an FP-bank bug.**

- **Symptom**: my executor wrote stale matrices where flycast leaves 0 — `0x8C2152E0` (19 B) and
  `0x8C2D6AD8..` (33 B), both matrix-stack storage under the camera/projection subtree `loc_8c02e1a4`.
- **Root cause (proven, no guessing)**: the back-bank XMTRX loader **`loc_8c120220`** (bank12:1-20 —
  `frchg; 16× fmov @r4+,frN; rts; frchg` = loads 16 floats from `@r4` into the FP **back bank** xf[])
  is reached ONLY via a static `bra bank12.loc_8c120220` from `loc_8c1201e0`. It has NO `#data`
  reference, so the `scan_tick.py` function-boundary scanner never emitted it as a worklist entry
  (it sits at bank12 line 1 with no preceding `;====` marker) — **even though its PCs WERE in the
  execution trace**. Result: its 6 loads were silently DROPPED (hit the `call_addr` no-op default),
  leaving the back bank **stale** at two matrix-stack saves (`loc_8c11fb80`→0x2D6AD8; the descriptor-1
  store→0x2152E0).
- **Proof**: an `MC_DHOOK` dispatch hook (test_tick.c) captured, per loader call, `r4` + the 16
  source floats from RAM, plus an ordered load/save interleave. Decisive frame:
  `L 120220@0x2152E0` loads a **zero** matrix into xf[] immediately before `S 11FB80@0x2D6AD8` saves
  it. With the load dropped, the save shipped a stale projection+translation matrix — which then
  poisoned `0x2152E0` for the *next* read-back (why both residuals shared one root).
- **`r4` was correct all along**: `loc_8c1201e0` computes `r4 = descriptor.top_ptr` (`@0x8C2D68E8+0x8`,
  confirmed bank12:465 by the expert). The earlier "52→75 worse" attempt failed only because it
  predated the SZ-aware fmov fix — not because `r4` diverged.
- **THE FIX**: add `(0x8c120220, 'bank12.asm', 1, 20, '-')` to `gen_tick.py` `EXTRA_FUNCS`. Diff 52→0.
- **Lone remaining `unknown_call` = 0x89889011** (benign): `loc_8c17D0C0` (bank17:25794) reads a
  fnptr from a jump table `[0x8C1C95BC + r5<<2]`; that table entry is `0x89889011`, a **non-RAM area-2
  peripheral address**. flycast reads the same snapshot RAM so it computes the same target; the
  executor drops it via `mc_is_ram` (area-3 only). Proven benign — full game RAM is byte-exact with
  it dropped. It is a peripheral/DMA dispatch orthogonal to the game-state domain.

**General lesson for future frames**: `bra bankNN.loc_XXXX` targets that lack a `#data` ref can be
missed by the boundary scanner even when traced. If a future frame (super/tag/different camera) shows
a residual, first check the runtime `unknown_call` census (test_tick prints distinct targets + first
chain) for a dropped static-`bra` loader like this one. Every reached call must resolve (or be a
proven non-RAM peripheral drop).

## 9. The team model (how the last fixes were made — use it)

Dispatch RE-expert subagents for disasm/flycast-source-grounded analysis, in parallel, with the
exact-match/no-guessing preamble. Proven productive:
- `senior-re-generalist` → reads flycast source (sh4_fpu.cpp etc.), gives byte-exact opcode semantics
  (nailed the SZ/XD-bank fmov fix + caught the XD-bank bug).
- `mvc2-sh4-re-expert` → disasm flow, function boundaries (scan_tick.py), the matrix-stack map, SPL.
Give them: the evidence (the residual bytes/values, the call chain), the specific question, and the
files. They cite bank:line / flycast file:line and tag CONFIRMED vs INFERRED. Integrate their
findings + verify against `test_tick.c`'s instant byte-diff.

## 10. After 0-diff (the remaining roadmap)

1. **Executor Test A complete** (0-diff) → the executor reproduces the game-tick.
2. **Chain into `render_frame`** (already byte-exact for the render subtree) = `input → next-frame TA`
   entirely off flycast — answers "the executor does the server's per-frame job".
3. **Compile to WASM** (`zig cc -target wasm32-freestanding`, like `exec_demo.wasm` already does for
   the leaves) → browser prediction/rollback. Interactive demo already live (Artifact 75c3b686).

## 11. File map (tools/render-replica-poc/)

| File | Role | Committed? |
|---|---|---|
| lift.py, codegen.py, emit_func.py | the transpiler (opcode + control flow) | yes |
| gen_one.py | single-leaf transpile + pool normalization | yes |
| gen_tick.py | whole-tick batch transpile + dispatch generation | yes |
| sh4ctx.h | memory model (fr/xf banks, mc_is_ram, MC_WRITELOG/WTRAP) | yes |
| verify_leaf.c | single-leaf verifier (write-set / truth-snapshot / oracle) | yes |
| test_tick.c | whole-tick harness + diff + debug traps | yes |
| realcore/runner.cpp | flycast interpreter oracle (--leaf/--trace/--setr) | yes |
| realcore/build_leaf_seed.py, dobuild.bat | seed builder + MSVC build | yes |
| _handoff/{scan_tick,scan_spl,classify_calls}.py | worklist scanners (RE) | yes |
| _handoff/{funclist,spl_funclist,trace_*}.txt | worklists + trace (derived) | GITIGNORED |
| gen_*.c, gen_tick_all.c, *.wasm, _ram_f9*.bin, _tick_truth.bin | transpiled game code / ROM-derived | GITIGNORED |
| exec_demo.c, WASM-DEMO.md | browser WASM demo (leaves) | yes |

Rules: NEVER commit transpiled game code (gen_*.c) or ROM-derived RAM (*.bin). Regenerate locally.
