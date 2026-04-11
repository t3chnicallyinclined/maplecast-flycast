/*
 * SH4Recomp — Statically recompiled blocks integrated into flycast
 *
 * When SH4RECOMP_BLOCKS is defined, this file includes the auto-generated
 * blocks.c and provides a lookup function. The interpreter mainloop calls
 * sh4recomp_find_block() before falling through to the normal interpreter.
 *
 * Build with: cmake -DSH4RECOMP=ON
 */

#ifdef SH4RECOMP_BLOCKS

#include "types.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/sh4_core.h"

// Pull in the generated blocks — they're all static functions + dispatch table
// The generated code uses Sh4Context* (which is the real flycast type) and
// calls OpPtr[opcode](ctx, opcode) for each instruction.
#include "generated_blocks.c"

static u64 _static_hits = 0;
static u64 _fallback_hits = 0;

typedef u32 (*RecompBlockFunc)(Sh4Context* ctx);

RecompBlockFunc sh4recomp_find_block(u32 pc) {
    // SH4 address aliasing: 0x8Cxxxxxx and 0xACxxxxxx map to same physical
    // RAM as 0x0Cxxxxxx. Our traced blocks use 0x0C addresses.
    if ((pc >> 24) == 0x8C || (pc >> 24) == 0xAC)
        pc = (pc & 0x00FFFFFF) | 0x0C000000;

    // Binary search in sorted block_table
    int lo = 0, hi = NUM_BLOCKS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (block_table[mid].addr == pc) {
            _static_hits++;
            return (RecompBlockFunc)block_table[mid].func;
        }
        if (block_table[mid].addr < pc) lo = mid + 1;
        else hi = mid - 1;
    }
    _fallback_hits++;
    return nullptr;
}

u64 sh4recomp_static_hits() { return _static_hits; }
u64 sh4recomp_fallback_hits() { return _fallback_hits; }

#endif // SH4RECOMP_BLOCKS
