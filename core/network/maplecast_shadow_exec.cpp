// SHADOW EXECUTOR — see maplecast_shadow_exec.h.
//
// This C++ TU uses flycast's guest-RAM (mem_b) and address space; it does NOT include the
// executor's sh4ctx.h (which redefines RAM_SIZE). It talks to the transpiled executor only
// through the C entry `mc_shadow_run_tick(u8*)` in shadow_exec_runner.c, which owns sh4ctx.h.
#include "maplecast_shadow_exec.h"

// The executor-dependent path is compiled ONLY when the MAPLECAST_SHADOW_EXEC CMake option
// links in the transpiled game-tick (gen_tick_all.c) + shadow_exec_runner.c. Without it, this
// TU still compiles and onFrame() is a no-op, so maplecast_mirror.cpp's unconditional call
// never becomes an unresolved symbol.
#ifndef MAPLECAST_SHADOW_EXEC_BUILD

namespace maplecast_shadow_exec { void onFrame() {} }

#else

#include "hw/sh4/sh4_mem.h"      // mem_b — flat 16MB area-3 image, offset = guestAddr & 0xFFFFFF
#include "hw/mem/addrspace.h"    // addrspace::read8 (in-match gate)

#include <cstdio>
#include <cstdlib>
#include <cstring>

// implemented in tools/render-replica-poc/shadow_exec_runner.c (C linkage)
extern "C" long mc_shadow_run_tick(unsigned char *ram);

namespace maplecast_shadow_exec {

static const unsigned RAMSZ = 16u * 1024u * 1024u;   // Dreamcast main RAM (matches mem_b)
static unsigned char *g_prev = nullptr;              // previous frame's snapshot (executor input)
static unsigned char *g_work = nullptr;              // scratch the executor mutates in place
static bool  g_havePrev = false;
static int   g_enabled  = -1;                        // -1 unresolved; 0/1 from env
static long  g_frame = 0, g_okFrames = 0, g_divFrames = 0;

static bool enabled() {
    if (g_enabled < 0) {
        const char *e = getenv("MAPLECAST_SHADOW_EXEC");
        g_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
        if (g_enabled) {
            g_prev = (unsigned char *)malloc(RAMSZ);
            g_work = (unsigned char *)malloc(RAMSZ);
            if (!g_prev || !g_work) { g_enabled = 0; printf("[SHADOW] malloc failed; disabled\n"); }
            else printf("[SHADOW] ENABLED — read-only per-frame executor validation vs mem_b\n");
        }
    }
    return g_enabled == 1;
}

// Render-SUBTREE intermediate state the game-logic tick legitimately does not own — these
// diverge because the executor excludes the render walk, NOT because the game logic is wrong.
// (Every one was classified from the disasm/docs offline; see validate_multiframe.c.)
static inline bool masked(unsigned off) {
    if (off >= 0x2D5748 && off <= 0x2D574B) return true;     // TCNT0 stopwatch mirror
    if (off >= 0x32DBAC && off <= 0x32DBAF) return true;     // TCNT0 stopwatch mirror
    if (off == 0x268250) return true;                        // fight-tick (timing-derived)
    if (off >= 0x3496B0 && off <= 0x3496B3) return true;     // frame_counter (vsync)
    if (off >= 0x2895E0 && off <= 0x2895EF) return true;     // slot-table counts (render draw list)
    for (int k = 0; k < 6; k++) { unsigned cb = 0x268340u + (unsigned)k * 0x5A4u;
        if (off == cb + 0x502u || off == cb + 0x503u) return true;   // render anim counter
        if (off >= cb + 0x0E0u && off <= cb + 0x0EBu) return true;   // screen x/y/z (render deposit)
    }
    return false;
}

static long diffRegion(const unsigned char *exec, const unsigned char *live,
                       const char *name, unsigned base, unsigned len) {
    long d = 0; unsigned first = 0;
    for (unsigned o = base; o < base + len; o++) { if (masked(o)) continue;
        if (exec[o] != live[o]) { if (!d) first = o; d++; } }
    if (d) printf("[SHADOW] f=%ld DIVERGE %-12s %ld bytes  first@0x8C%06X (exec=%02X live=%02X)\n",
                  g_frame, name, d, first, exec[first], live[first]);
    return d;
}

void onFrame() {
    if (!enabled()) return;
    // in-match gate — same 0x8C289624 flag the .mctele tap uses; reset continuity between matches
    if (addrspace::read8(0x8C289624) == 0) { g_havePrev = false; return; }

    const unsigned char *live = (const unsigned char *)&mem_b[0];
    g_frame++;

    if (g_havePrev) {
        memcpy(g_work, g_prev, RAMSZ);          // executor input = previous frame's snapshot
        long disp = mc_shadow_run_tick(g_work); // one game-logic tick, in place
        long d = 0;
        d += diffRegion(g_work, live, "char-structs", 0x268340u, 6u * 0x5A4u);
        d += diffRegion(g_work, live, "globals",      0x289000u, 0x1000u);
        d += diffRegion(g_work, live, "camera",       0x26A520u, 0x60u);
        if (d == 0) {
            g_okFrames++;
            if ((g_okFrames % 300) == 0)   // heartbeat every ~5s of match at 60fps
                printf("[SHADOW] f=%ld OK — %ld byte-exact frames, %ld diverged (disp=%ld)\n",
                       g_frame, g_okFrames, g_divFrames, disp);
        } else {
            g_divFrames++;
        }
    }
    memcpy(g_prev, live, RAMSZ);                 // this frame becomes next frame's input
    g_havePrev = true;
}

} // namespace maplecast_shadow_exec

#endif // MAPLECAST_SHADOW_EXEC_BUILD
