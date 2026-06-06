# State-Replica Client — Summary & Plan

> Branch `feat/state-replica-client`. The pixel-perfect, low-bandwidth path for
> **ROM-owning native clients**: the server runs the game (authoritative) and ships
> the per-frame state; the client **injects that state into its own flycast RAM and
> renders the game's real output**. No drift (the client never simulates), pixel-exact
> (it's the game's own render code), ~15 KB/s (ships the render *input*, not the output).

## Where we got with the state / STAF renderers (the journey)

| Approach | Bandwidth | Fidelity | Outcome |
|---|---|---|---|
| **Mirror** (stream TA + VRAM) | ~2.1 MB/s | 100% | works, too heavy |
| **STAF** (stripped-TA, custom WebGL renderer) | ~2–5 MB/s | — | **dropped**: reinvented flycast's renderer; per-triangle blew bandwidth; geometry is the floor |
| **State + rips (Option 6)** | ~15 KB/s | ~99% | works; `re-catalog` pool map supplies cape/effects (pool objects, not parts) |
| **Parts-assembly** (decode parts from RAM) | ~15 KB/s | ~100% | re-key SOLVED (live cell 0x154); small parts decode; **256×256 body never decoded** (composite/structural); shared-directory grabbed wrong sprites. The "hard middle." |

## The decisive numbers (live mirror log)
- TA geometry **~144 KB/frame**, changes nearly fully each frame → **not delta-able**.
- Textures ~100 KB/frame. Compressed total **~2.1 MB/s**.
- Geometry floor (textures content-cached): **~1.1 MB/s** — irreducible without re-deriving.
- State: **~253 B/frame = ~15 KB/s**.

## Why those failed / converged
- **Pixel-perfect requires the game's render code** (Stages 2–4: assembly → texture → TA). That code is the **game's SH4 code in the ROM**, not flycast's — flycast only *runs* it.
- A ROM-less client can't run it → must ship the rendered **output** (mirror ~2.1 MB/s) or **re-derive** it (parts = hard, rips = ~99%).
- The savestate-seed idea hit the **address-reuse / aliasing** wall (VRAM addresses get reused; address-keyed texture cache goes stale). Content-caching fixes the wall but only saves the ~40% texture half — the ~50% geometry half is irreducible.

## The chosen path (this branch)
**Server-authoritative state + client renders it via state injection.** Not lockstep:
- **Lockstep** = client *simulates* from inputs → drift risk.
- **This** = server simulates (authoritative), client **injects the server's state and only renders** → no simulation, no drift, pixel-exact, ~15 KB/s.
- Requires the **ROM + SH4 on the client** → ROM-owning native client (for testing, the operator copies the ROM locally — never shipped/committed).

## Build plan
1. `writeGameState()` — the **inverse** of `readGameState()` (`core/network/maplecast_gamestate.cpp`): write the GSTA fields back into RAM at the known addresses (char structs `0x8C268340` stride `0x5A4`; object pool `0x8C26AA54` stride `0x1D0`; globals `0x8C289000`).
2. `MAPLECAST_REPLICA=1` mode (render build, not headless): load ROM + the same savestate the server autoloads; connect to the server's GSTA stream; **each frame inject the state then render** (inject after the SH4 frame's logic, before render — or pause logic and render the injected state).
3. **Test:** run it locally next to the server stream — does it match, pixel-exact, no drift, at ~15 KB/s? Confirm how much state is actually needed (is 253 B sufficient, or does more RAM matter?).

## Reuse (don't reinvent)
- flycast's SH4 + native renderer (runs the ROM, renders) — unchanged.
- The GSTA wire (server already broadcasts it; `sprite-client.mjs onGSTA` + `maplecast_gamestate.cpp` define the layout).
- `MAPLECAST_MIRROR_CLIENT` as the reference for a native client connecting to the server.

## Open question the test answers
Is the ~253-byte GSTA state **sufficient** to reproduce the exact frame when injected into a savestate-baseline client, or does other RAM (the "other game state changing" concern) need to be included? The test measures the real minimum.
