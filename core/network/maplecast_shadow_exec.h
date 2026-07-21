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

}
