// gsta_charpass.h — Phase 2a: run MVC2's REAL char-pass render driver
// (loc_8c030858 -> retPc 0x8C039648) in-process on a 16MB RAM image via flycast's
// SH4 interpreter core, capturing the store-queue-emitted TA parcels. This is the
// same driver-run the standalone runner proved byte-exact (engine_ta == runner_ta,
// md5 be1377d28b3d4bf624c18590dae21ce5). Replaces the transpile (render_frame) for
// the bodies+effects portion of the client's GSTA render.
#pragma once
#include "types.h"
#include <vector>
#include <cstdint>

// --- interpreter Run-loop hook (gated; inert unless a char-pass run is active) ---
// Declared here, defined in gsta_charpass.cpp; called from sh4_interpreter.cpp Run().
extern bool gsta_charpass_active;
void gsta_charpass_onpc(u32 pc);

namespace gsta_charpass {

// Driver-call constants (stable across frames — captured from the frame-90 seed).
static const u32 ENTRY_PC = 0x8C030858u;
static const u32 SP_ENTRY  = 0x8C00F3ECu;   // r15 at entry
static const u32 RET_PC    = 0x8C039648u;   // caller return (pr at entry)

// Run the char-pass driver on a COPY of ram16 (16MB, area-3 image), seeded with the
// entry CPU context ctx512 (raw 512B Sh4Context). Captures the SQ-emitted TA parcels
// into outTa (32B each). ccn72 (optional, 18*u32) seeds on-chip CCN regs incl QACR;
// pass nullptr to leave them zero (capture is unaffected — proven). Returns true iff
// the driver reached RET_PC (no watchdog/fault). *wallMs = wall-clock if non-null.
bool run(const uint8_t* ram16, const uint8_t* ctx512, const uint8_t* ccn72,
         std::vector<uint8_t>& outTa, double* wallMs);

// One-shot self-test (MAPLECAST_CHARPASS_SELFTEST=<seed2.bin>): load an RTSEED02 seed,
// run(), print parcel count + md5, write charpass_ta.bin. Returns true if it ran.
bool selftest_from_env();

// LIVE per-frame char-pass: run the driver on the client's current _gstaRam using the
// STABLE driver-entry Sh4Context (lazy-loaded once from MAPLECAST_GSTA_CHARPASS_SEED,
// an RTSEED02 dump). Returns true + fills outTa (SQ parcels) iff it reached retPc.
// Returns false (caller falls back to the transpile) if no context is available.
bool run_live(const uint8_t* ram16, std::vector<uint8_t>& outTa, double* wallMs);

} // namespace gsta_charpass
