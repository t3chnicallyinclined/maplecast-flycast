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
#include <chrono>

// implemented in tools/render-replica-poc/shadow_exec_runner.c (C linkage)
extern "C" long mc_shadow_run_tick(unsigned char *ram);
extern "C" long mc_shadow_last_nonram_reads(void);   // reads outside area-3 RAM (ROM/hw) last tick
extern "C" unsigned mc_shadow_last_nonram_addr(void);

namespace maplecast_shadow_exec {

static const unsigned RAMSZ = 16u * 1024u * 1024u;   // Dreamcast main RAM (matches mem_b)

// Two 16MB buffers, pointer-swapped so a frame costs ONE snapshot copy (not two): g_prev holds
// the previous frame's snapshot; each frame we snapshot mem_b into the recycled buffer, run the
// executor IN PLACE on g_prev (we don't need it afterward), diff vs the fresh snapshot, then the
// fresh snapshot becomes the next g_prev and the mutated buffer is recycled.
static unsigned char *g_prev = nullptr, *g_free = nullptr;
static bool  g_havePrev = false;
static int   g_enabled  = -1;                        // -1 unresolved; 0/1 from env
static long  g_stride   = 1;                         // validate every Nth frame (safety valve)
static long  g_frame = 0, g_valFrames = 0, g_okFrames = 0, g_divFrames = 0;
static long  g_nonRamFrames = 0, g_nonRamMax = 0; static unsigned g_nonRamAddr = 0;   // ROM/hw read census

// per-frame timing (ms) — running mean + max + last
static double g_execAvg=0, g_execMax=0, g_execLast=0;
static double g_copyAvg=0, g_copyMax=0, g_copyLast=0;
static double g_totAvg=0,  g_totMax=0,  g_totLast=0;
// last divergence (for the log + dashboard)
static long g_divFrame=0, g_divCount=0; static char g_divRegion[16]={0};
static unsigned g_divAddr=0; static unsigned char g_divExec=0, g_divLive=0;

static const char *statsPath() {
    const char *p = getenv("MAPLECAST_SHADOW_STATS_PATH");
    return (p && p[0]) ? p : "/dev/shm/mc_shadow_stats.json";
}

static bool enabled() {
    if (g_enabled < 0) {
        const char *e = getenv("MAPLECAST_SHADOW_EXEC");
        g_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
        const char *s = getenv("MAPLECAST_SHADOW_STRIDE");
        if (s && s[0]) { long v = atol(s); if (v >= 1) g_stride = v; }
        if (g_enabled) {
            g_prev = (unsigned char *)malloc(RAMSZ);
            g_free = (unsigned char *)malloc(RAMSZ);
            if (!g_prev || !g_free) { g_enabled = 0; printf("[SHADOW] malloc failed; disabled\n"); }
            else printf("[SHADOW] ENABLED — read-only per-frame executor validation vs mem_b "
                        "(stride=%ld, stats=%s)\n", g_stride, statsPath());
        }
    }
    return g_enabled == 1;
}

static inline double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
static inline void acc(double &avg, double &mx, double v, long n) {
    avg += (v - avg) / (double)(n > 0 ? n : 1);   // running mean
    if (v > mx) mx = v;
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
    if (d) {   // remember the FIRST divergent region this frame for the log/dashboard
        if (g_divRegion[0] == 0 || g_divFrame != g_frame) {
            snprintf(g_divRegion, sizeof g_divRegion, "%s", name);
            g_divFrame = g_frame; g_divAddr = first; g_divExec = exec[first]; g_divLive = live[first];
        }
        g_divCount += d;
    }
    return d;
}

static void writeStatsJson() {
    FILE *f = fopen(statsPath(), "w");
    if (!f) return;
    fprintf(f,
        "{\"frame\":%ld,\"validated\":%ld,\"ok\":%ld,\"diverged\":%ld,\"stride\":%ld,"
        "\"exec_ms\":{\"avg\":%.3f,\"max\":%.3f,\"last\":%.3f},"
        "\"copy_ms\":{\"avg\":%.3f,\"max\":%.3f,\"last\":%.3f},"
        "\"total_ms\":{\"avg\":%.3f,\"max\":%.3f,\"last\":%.3f},"
        "\"pct_budget_avg\":%.2f,"
        "\"nonram\":{\"frames\":%ld,\"max\":%ld,\"addr\":\"0x%08X\"},"
        "\"last_div\":{\"frame\":%ld,\"region\":\"%s\",\"addr\":\"0x8C%06X\","
        "\"exec\":%u,\"live\":%u,\"bytes\":%ld}}",
        g_frame, g_valFrames, g_okFrames, g_divFrames, g_stride,
        g_execAvg, g_execMax, g_execLast, g_copyAvg, g_copyMax, g_copyLast,
        g_totAvg, g_totMax, g_totLast, g_totAvg / 16.67 * 100.0,
        g_nonRamFrames, g_nonRamMax, g_nonRamAddr,
        g_divFrame, g_divRegion[0] ? g_divRegion : "-", g_divAddr, g_divExec, g_divLive, g_divCount);
    fclose(f);
}

void onFrame() {
    using clock = std::chrono::steady_clock;
    if (!enabled()) return;
    // in-match gate — same 0x8C289624 flag the .mctele tap uses; reset continuity between matches
    if (addrspace::read8(0x8C289624) == 0) { g_havePrev = false; return; }
    g_frame++;

    // snapshot the authoritative frame into the recycled buffer (this frame's "truth")
    auto tCopy = clock::now();
    memcpy(g_free, (const unsigned char *)&mem_b[0], RAMSZ);
    g_copyLast = msSince(tCopy); acc(g_copyAvg, g_copyMax, g_copyLast, g_frame);

    if (g_havePrev && (g_frame % g_stride) == 0) {
        auto tTot = clock::now();
        long disp = mc_shadow_run_tick(g_prev);          // one tick, IN PLACE on the prev snapshot
        g_valFrames++;
        g_execLast = msSince(tTot); acc(g_execAvg, g_execMax, g_execLast, g_valFrames);
        long nonram = mc_shadow_last_nonram_reads();     // did the tick reach outside main RAM?
        if (nonram > 0) { g_nonRamFrames++; g_nonRamAddr = mc_shadow_last_nonram_addr();
            if (nonram > g_nonRamMax) g_nonRamMax = nonram; }
        long d = 0;
        d += diffRegion(g_prev, g_free, "char-structs", 0x268340u, 6u * 0x5A4u);
        d += diffRegion(g_prev, g_free, "globals",      0x289000u, 0x1000u);
        d += diffRegion(g_prev, g_free, "camera",       0x26A520u, 0x60u);
        g_totLast = msSince(tTot); acc(g_totAvg, g_totMax, g_totLast, g_valFrames);
        if (d == 0) g_okFrames++;
        else {
            g_divFrames++;
            printf("[SHADOW] f=%ld DIVERGE %s %ld B first@0x8C%06X (exec=%02X live=%02X) disp=%ld\n",
                   g_frame, g_divRegion, g_divCount, g_divAddr, g_divExec, g_divLive, disp);
        }
        if ((g_valFrames % 60) == 0) {   // heartbeat + stats flush every ~1s of match
            printf("[SHADOW] f=%ld OK — %ld validated (%ld ok / %ld diverged) | exec %.2f/%.2f avg/max ms, "
                   "total %.2f ms = %.1f%% of 16.7ms budget (stride %ld) | ROM/hw reads: %ld frames%s\n",
                   g_frame, g_valFrames, g_okFrames, g_divFrames,
                   g_execAvg, g_execMax, g_totAvg, g_totAvg / 16.67 * 100.0, g_stride,
                   g_nonRamFrames, g_nonRamFrames ? " (tick reached outside RAM — see stats)" : " (RAM-only)");
            writeStatsJson();
        }
    }

    // the fresh snapshot becomes next frame's prev; recycle the (now-stale) prev buffer
    unsigned char *tmp = g_prev; g_prev = g_free; g_free = tmp;
    g_havePrev = true;
}

} // namespace maplecast_shadow_exec

#endif // MAPLECAST_SHADOW_EXEC_BUILD
