// SHADOW EXECUTOR — live server-side validation of the transpiled MVC2 game-tick executor.
//
// Gated (MAPLECAST_SHADOW_EXEC env), READ-ONLY, off the game state: each rendered frame we
// run the standalone transpiled game-logic tick on the PREVIOUS frame's guest-RAM snapshot
// and diff the game-state regions vs flycast's own authoritative mem_b. Expected: 0 bytes.
// Any divergence is logged (frame, address, situation) — a lead pointing at the exact frame /
// SPL function the executor doesn't yet reproduce. This is the broad-corpus validation gate:
// play real matches (supers / tag / KO / all 56 chars) and every frame confirms or refutes the
// executor against ground truth. Same discipline as the determinism-proven .mctele tap.
//
// Requires the transpiled executor (gen_tick_all.c) + shadow_exec_runner.c linked in
// (headless build, MAPLECAST_SHADOW_EXEC CMake option). Never on the input->sim latency path.
#pragma once

namespace maplecast_shadow_exec {

// Call once per composited frame from serverPublish (after the .mctele tap, on the headless
// path). No-op unless the MAPLECAST_SHADOW_EXEC env var is set. Never mutates guest state.
void onFrame();

// EXECUTOR DRIVE (MAPLECAST_EXECUTOR) — the transpiled game-tick REPLACES the SH-4. Called
// from Emulator::runInternal() in place of getSh4Executor()->Run(). When it returns true it
// has advanced the AUTHORITATIVE mem_b one game-logic frame (input inject -> tick -> compose
// render projection) and broadcast it via /replica-live; the caller must NOT run the SH-4.
// Returns false (caller runs the SH-4 normally) when: MAPLECAST_EXECUTOR is unset, the build
// lacks the linked executor, not in-match, or the /replica-live static prefix isn't ready yet
// (the SH-4 must render the first in-match frames so VRAM/GFX tables are captured before the
// tick takes over). No-op returning false in a non-MAPLECAST_SHADOW_EXEC build.
bool driveFrame();

}
