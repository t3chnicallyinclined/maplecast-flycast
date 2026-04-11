/*
 * SH4Recomp — Direct execution on flycast's Sh4cntx global.
 * No bridge, no copy. Blocks use #define ctx (&Sh4cntx).
 */

#ifdef SH4RECOMP_BLOCKS

#include "types.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/mem/addrspace.h"

// Block lookup (defined in blocks_dispatch.cpp, extern "C" linkage)
typedef u32 (*RecompBlockFunc)(u8* mem, Sh4Context* ctx);
extern "C" RecompBlockFunc recomp_find_block(u32 pc);

static u64 _static_hits = 0;
static u64 _fallback_hits = 0;

bool sh4recomp_try_exec(u32 pc) {
    u32 lookup_pc = pc;
    if ((lookup_pc >> 24) == 0x8C || (lookup_pc >> 24) == 0xAC)
        lookup_pc = (lookup_pc & 0x00FFFFFF) | 0x0C000000;

    // Only execute game code blocks past init (>= 0x0C021000)
    // 0x0C020000-0x0C020FFF is init code (memset etc) that conflicts
    // with savestate loading — let interpreter handle it
    if (lookup_pc < 0x0C021000) {
        _fallback_hits++;
        return false;
    }

    RecompBlockFunc block = recomp_find_block(lookup_pc);
    if (!block) {
        _fallback_hits++;
        return false;
    }

    _static_hits++;

    // Execute directly on Sh4cntx — the block uses #define ctx (&Sh4cntx)
    // The mem parameter is unused in flycast mode (blocks use ReadMem32_nommu)
    u32 next_pc = block(nullptr, &Sh4cntx);
    Sh4cntx.pc = next_pc;

    if (_static_hits <= 10 || (_static_hits % 10000) == 0) {
        printf("[sh4recomp] Block #%lu: 0x%08X → 0x%08X (fallbacks=%lu)\n",
            _static_hits, pc, next_pc, _fallback_hits);
        fflush(stdout);
    }

    return true;
}

u64 sh4recomp_static_hits() { return _static_hits; }
u64 sh4recomp_fallback_hits() { return _fallback_hits; }

#endif
