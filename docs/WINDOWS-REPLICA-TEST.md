# State-Replica — Windows Test Handoff

> Read this first if you're a fresh agent session on the Windows box. This is the
> handoff for testing the **native state-replica client** on a real display.
> Branch: `feat/state-replica-client`. Full design: `docs/STATE-REPLICA-PLAN.md`.

## What this is
The state-replica client: a native flycast that **runs the game's own render code on
server-injected state**. The prod server (authoritative) runs MVC2 and broadcasts a
~265 B/frame GSTA state; this client loads the same savestate, connects, and **injects
that state into its own SH4 RAM every frame, then renders** — no drift (it renders the
server's truth, never simulates inputs), pixel-exact, ~16 KB/s. NOT lockstep. The browser
cannot do this (no SH4 in the wasm) — native only.

## Status (what's proven, what's open)
Proven on Linux (headless): builds, connects to prod, loads the matching savestate,
auto-starts, and **the inject works** — logs show `FIRST GSTA injected` then
`HEARTBEAT frame=N injected=M` tracking the live server frame-by-frame, **no crash**
(after the `c06165abe` single-threaded fix for the `p_sh4rcb` init race).

**OPEN QUESTION (why we're on Windows):** the Linux *headless* screenshot came back
**black** (just the X cursor, no window). Can't tell if that's (a) the headless Xvfb
capture not grabbing the window, or (b) the game genuinely rendering black (e.g. the
savestate's VRAM/textures don't match the server's *current* match). **A real display
settles it instantly.** That's the whole reason for this Windows run.

## The kit (copy to the Windows flycast data dir)
`state-replica-kit/` (gitignored, ROM-derived — copy manually, never commit):
- `mvc2.state` — prod's savestate (slot 0)
- `emu.cfg` — prod's machine config (Region/Language/etc. must match)
- `dc_boot.bin`, `dc_flash.bin`, `dc_nvmem.bin` — Dreamcast BIOS/flash/nvmem

## ⚠️ Savestate naming gotcha
flycast names the savestate after the ROM's basename: `<rom-basename>.state` (slot 0).
The operator's ROM is `C:\roms\mvc2_us\Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].gdi`.
**Easiest fix: rename that `.gdi` file to `mvc2.gdi`** (leave the track files untouched —
the .gdi's content references them relatively), so it matches `mvc2.state`. Otherwise
rename `mvc2.state` → `Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].state`.
(ROM must be the same MVC2 US dump as prod, game ID `T1212N`, or the savestate won't load.)

## Build (Windows)
Needs Visual Studio (MSVC) + CMake. From the repo on `feat/state-replica-client`:
```
cmake -S . -B build-win -G "Visual Studio 17 2022" -DMAPLECAST_CLIENT_ONLY=ON
cmake --build build-win --config Release --target flycast
```
(Adjust the generator to the installed VS. Output: `build-win\Release\flycast.exe`.)

## Run (from a console, so logs are visible)
Place `mvc2.state` + BIOS in flycast's data dir, `emu.cfg` in its config dir
(`%LOCALAPPDATA%\flycast\` typically; confirm where the build looks). Then:
```
set MAPLECAST_STATE_REPLICA=15.204.141.58:7201
set MAPLECAST_HEADLESS_AUTOLOAD=1
flycast.exe "C:\roms\mvc2_us\mvc2.gdi"
```
Add `> log.txt 2>&1` to capture logs.

## What to watch
- Console: `[state-replica] FIRST GSTA injected …` then `HEARTBEAT frame=N injected=M …`
  every second (proves inject — already confirmed working).
- **Window:** the live match, characters tracking the prod server. **This is the answer
  we need** — clean render = success; garbled/black = render or texture-parity issue.

## Architecture notes for debugging
- FREEZE-only: local inputs pinned neutral; the inject is the sole driver of visible state.
- **Chars-only by default.** Pool objects (cape/projectiles/effects) are gated behind
  `MAPLECAST_STATE_REPLICA_OBJECTS=1` — leave OFF until chars render, and prod ships no
  OBJF yet anyway (it's still the old server binary).
- Inject point: top of frame, before `runInternal()`, in the non-threaded run loop
  (`ThreadedRendering` forced off in replica mode to avoid the init race).
- `MAPLECAST_STATE_REPLICA_FREEZE` is automatic; `_NO_FREEZE=1` is a future opt-out.
- If it crashes, build `-DCMAKE_BUILD_TYPE=RelWithDebInfo` (or the dbg target) for symbols.

## Most likely remaining issue
If the window is black/garbled (not a capture artifact), it's **VRAM/texture parity**:
the local savestate's loaded textures vs the server's *current* match. The savestate is
prod's boot match; if prod has cycled to a different match/characters, the local VRAM
won't have the right sprites. Confirm the server's current characters match the
savestate's, or capture a fresh savestate from the current prod state.
