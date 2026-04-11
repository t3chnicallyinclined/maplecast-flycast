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

    // Only execute game code blocks (>= 0x0C020000)
    // BIOS code runs on interpreter — it uses privileged instructions
    if (lookup_pc < 0x0C020000) {
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

    if (_static_hits <= 5 || (_static_hits % 100000) == 0) {
        printf("[sh4recomp] Block #%lu: 0x%08X → 0x%08X\n",
            _static_hits, pc, next_pc);
        fflush(stdout);
    }

    return true;
}

u64 sh4recomp_static_hits() { return _static_hits; }
u64 sh4recomp_fallback_hits() { return _fallback_hits; }

#endif
