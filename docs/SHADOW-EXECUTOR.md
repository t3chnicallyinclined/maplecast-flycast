# Shadow Executor — live game-tick validation on the flycast server

The transpiled MVC2 game-tick executor (`tools/render-replica-poc`, byte-exact vs flycast on
the offline captures) is shimmed into the headless server as a **read-only, gated, per-frame
validation harness**. Each rendered frame it runs the standalone executor on the *previous*
frame's guest-RAM snapshot and diffs the game-state regions vs flycast's own authoritative
`mem_b`. Expected: **0 bytes**. Any divergence is logged — a lead pointing at the exact frame /
SPL function the executor doesn't yet reproduce.

This is the **broad-corpus validation gate** the project has lacked: play real matches (supers,
tag-ins, KOs, all 56 characters) and every frame confirms or refutes the executor against ground
truth. Same discipline as the determinism-proven `.mctele` tap — off the input→sim latency path,
never mutates guest state.

## Why it matters

Test A proved the executor byte-exact on **one** calm 1v1 frame. `validate_multiframe.c` extended
that to **3 consecutive live transitions** (`f90→f91→f92→f93`, all 0-diff game state). The shadow
extends it to *millions* of live frames, and every divergence auto-builds a regression corpus. It's
also the stepping stone to promotion: once the shadow holds across broad play, the executor can
*run* the ticks (input → next-frame state → TA), replacing the SH-4 core — and the same executor,
compiled to WASM, does client-side rollback.

## Pieces

| File | Role |
|------|------|
| `tools/render-replica-poc/gen_tick_all.c` | the transpiled game-tick (**derived game code — gitignored**; regenerate with `gen_tick.py`) |
| `tools/render-replica-poc/shadow_exec_runner.c` | C bridge: `mc_shadow_run_tick(u8* ram)` runs one tick in place; isolates the `RAM_SIZE` macro from flycast's |
| `core/network/maplecast_shadow_exec.{h,cpp}` | flycast-side shim: prev snapshot, run executor, diff game-state vs `mem_b`, log. Always compiles (no-op unless the executor is linked) |
| `core/network/maplecast_mirror.cpp` | calls `maplecast_shadow_exec::onFrame()` in `serverPublish`, right after the `.mctele` tap |
| `core/network/CMakeLists.txt` | `-DMAPLECAST_SHADOW_EXEC=ON` links the executor + defines `MAPLECAST_SHADOW_EXEC_BUILD` |

## Build (dev box — Linux headless + ROM)

`gen_tick_all.c` is transpiled from the `_marv_re` disassembly on the Windows dev machine and is
gitignored, so it must be produced there and copied over:

```bash
# 1) On the Windows machine (has the disasm), regenerate the transpiled executor:
python C:/Users/trist/projects/maplecast-flycast/tools/render-replica-poc/gen_tick.py
#    -> tools/render-replica-poc/gen_tick_all.c  (271 fns, ~byte-exact)

# 2) Copy the 3 executor files to the build box (gitignored, so out-of-band).
#    Build box = rise3 (also prod, and it holds ~/roms/mvc2.gdi) since the 2026-09-01 cutover.
#    dev0ps tris@65.109.77.178 still has a checkout + a ROM copy and can build, but it runs no
#    maplecast services and is now the frozen forgily rollback standby.
scp -i ~/.ssh/ovh_maplecast tools/render-replica-poc/{gen_tick_all.c,shadow_exec_runner.c,sh4ctx.h} \
    ubuntu@15.204.141.58:/home/ubuntu/src/maplecast-flycast/tools/render-replica-poc/

# 3) On the dev box, configure + build the headless server WITH the option:
cmake -S . -B build-shadow -DMAPLECAST_HEADLESS=ON -DMAPLECAST_SHADOW_EXEC=ON
cmake --build build-shadow -j
#    (CMake FATAL_ERRORs early if gen_tick_all.c is missing.)
```

A stock build (`-DMAPLECAST_SHADOW_EXEC` unset) is byte-identical to today: `onFrame()` compiles to
a no-op and the executor is never linked.

## Run + read the log

```bash
MAPLECAST_SHADOW_EXEC=1 ./build-shadow/flycast   # + the usual headless env/ROM args
```

Then drive a real match (or feed a recording). The shim logs to stdout:

- `[SHADOW] ENABLED …` once at first frame.
- `[SHADOW] f=<n> OK — <k> byte-exact frames, <d> diverged (disp=<n>)` — heartbeat every 300 clean frames.
- `[SHADOW] f=<n> DIVERGE <region> <k> bytes first@0x8C…… (exec=.. live=..)` — a real divergence.

### Interpreting a divergence

Every divergence is one of two things — classify it, never dismiss it (exact-match discipline):

1. **A render-SUBTREE field the executor legitimately doesn't own.** The game-logic tick excludes the
   render walk, so these differ by design and are *masked* in `masked()` (kept in sync with
   `validate_multiframe.c`): char `+0xE0/E4/E8` screen x/y/z, char `+0x502` render anim, slot-table
   counts `0x2895E0[16]` (draw list), `frame_counter 0x3496B0` (vsync), the 3 hw-timer mirrors. If a
   *new* render field shows up, confirm it's render-deposited (disasm/docs) and add it to `masked()`.
2. **A real executor gap.** A genuine game-state byte the executor got wrong — almost always a
   character whose SPL code isn't transpiled yet (the executor currently carries only the two chars in
   the capture), or an untranspiled function reached by a new situation. Capture the frame (pair the
   shadow with a `MAPLECAST_DUMP_RAM_TRIGGER` dump) and add it to the offline regression set, then
   transpile the missing function.

The goal is a long match with **0 real divergences**. That's the broad-corpus proof; then promotion.
