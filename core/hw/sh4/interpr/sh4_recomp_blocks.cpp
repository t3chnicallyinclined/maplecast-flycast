/*
 * SH4Recomp — Direct C blocks integrated into flycast
 *
 * The generated blocks are compiled separately as C (not included here).
 * This file provides the bridge between flycast's context and the
 * standalone block functions.
 *
 * Build with: cmake -DSH4RECOMP=ON
 */

#ifdef SH4RECOMP_BLOCKS

#include "types.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_core.h"
#include "hw/mem/addrspace.h"

// The generated blocks are compiled as C in a separate TU.
// They use their own Sh4Context struct. We bridge via extern "C".
extern "C" {

// Standalone Sh4Context (must match sh4_runtime.h exactly)
typedef struct {
    u32 r[16];
    u32 r_bank[8];
    u32 pc, pr, gbr, vbr, ssr, spc;
    u32 mach, macl;
    struct { u32 T:1, S:1, _pad:2, IMASK:4, Q:1, M:1; } sr;
    u32 fpscr, fpul;
    float fr[16], xf[16];
    s32 cycle_counter;
} RecompSh4Context;

typedef u32 (*RecompBlockFunc)(u8* mem, RecompSh4Context* ctx);

// Defined in generated_blocks_c.o (compiled separately as C)
RecompBlockFunc recomp_find_block(u32 pc);

} // extern "C"

// Bridge: copy flycast context → recomp context
static void flycast_to_recomp(RecompSh4Context* dst) {
    for (int i = 0; i < 16; i++) dst->r[i] = Sh4cntx.r[i];
    for (int i = 0; i < 8; i++) dst->r_bank[i] = Sh4cntx.r_bank[i];
    dst->pc = Sh4cntx.pc;
    dst->pr = Sh4cntx.pr;
    dst->gbr = Sh4cntx.gbr;
    dst->vbr = Sh4cntx.vbr;
    dst->ssr = Sh4cntx.ssr;
    dst->spc = Sh4cntx.spc;
    dst->mach = Sh4cntx.mac.full >> 32;
    dst->macl = Sh4cntx.mac.full & 0xFFFFFFFF;
    dst->sr.T = Sh4cntx.sr.T;
    dst->sr.S = Sh4cntx.sr.S;
    dst->sr.IMASK = Sh4cntx.sr.IMASK;
    dst->sr.Q = Sh4cntx.sr.Q;
    dst->sr.M = Sh4cntx.sr.M;
    dst->fpscr = Sh4cntx.fpscr.full;
    dst->fpul = Sh4cntx.fpul;
    for (int i = 0; i < 16; i++) dst->fr[i] = Sh4cntx.fr[i];
    for (int i = 0; i < 16; i++) dst->xf[i] = Sh4cntx.xf[i];
    dst->cycle_counter = Sh4cntx.cycle_counter;
}

static void recomp_to_flycast(const RecompSh4Context* src) {
    for (int i = 0; i < 16; i++) Sh4cntx.r[i] = src->r[i];
    for (int i = 0; i < 8; i++) Sh4cntx.r_bank[i] = src->r_bank[i];
    Sh4cntx.pc = src->pc;
    Sh4cntx.pr = src->pr;
    Sh4cntx.gbr = src->gbr;
    Sh4cntx.vbr = src->vbr;
    Sh4cntx.ssr = src->ssr;
    Sh4cntx.spc = src->spc;
    Sh4cntx.mac.full = ((u64)src->mach << 32) | src->macl;
    Sh4cntx.sr.T = src->sr.T;
    Sh4cntx.sr.S = src->sr.S;
    Sh4cntx.sr.IMASK = src->sr.IMASK;
    Sh4cntx.sr.Q = src->sr.Q;
    Sh4cntx.sr.M = src->sr.M;
    Sh4cntx.fpscr.full = src->fpscr;
    Sh4cntx.fpul = src->fpul;
    for (int i = 0; i < 16; i++) Sh4cntx.fr[i] = src->fr[i];
    for (int i = 0; i < 16; i++) Sh4cntx.xf[i] = src->xf[i];
    Sh4cntx.cycle_counter = src->cycle_counter;
}

static u64 _static_hits = 0;
static u64 _fallback_hits = 0;

bool sh4recomp_try_exec(u32 pc) {
    u32 lookup_pc = pc;
    if ((lookup_pc >> 24) == 0x8C || (lookup_pc >> 24) == 0xAC)
        lookup_pc = (lookup_pc & 0x00FFFFFF) | 0x0C000000;

    RecompBlockFunc block = recomp_find_block(lookup_pc);
    if (!block) {
        _fallback_hits++;
        return false;
    }

    _static_hits++;

    RecompSh4Context rctx;
    flycast_to_recomp(&rctx);

    u32 next_pc = block(addrspace::ram_base, &rctx);
    rctx.pc = next_pc;

    recomp_to_flycast(&rctx);
    return true;
}

u64 sh4recomp_static_hits() { return _static_hits; }
u64 sh4recomp_fallback_hits() { return _fallback_hits; }

#endif // SH4RECOMP_BLOCKS
