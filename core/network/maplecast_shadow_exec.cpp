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

namespace maplecast_shadow_exec { void onFrame() {} bool driveFrame() { return false; } }

#else

#include "hw/sh4/sh4_mem.h"      // mem_b — flat 16MB area-3 image, offset = guestAddr & 0xFFFFFF
#include "hw/mem/addrspace.h"    // addrspace::read8 (in-match gate)
#include "input/gamepad_device.h"        // kcode[4], lt[4], rt[4] — the live input latch (drive path)
#include "maplecast_replica_live.h"      // onRenderFrame() broadcast + prefixReady() handover gate

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <thread>

// implemented in tools/render-replica-poc/shadow_exec_runner.c (C linkage)
extern "C" long mc_shadow_run_tick(unsigned char *ram);
extern "C" long mc_shadow_last_nonram_reads(void);   // reads outside area-3 RAM (ROM/hw) last tick
extern "C" unsigned mc_shadow_last_nonram_addr(void);
extern "C" unsigned mc_shadow_compose_proj(unsigned char *ram);   // rebuild render proj @0x2D6AD8 post-tick

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

// ============================================================================
// EXECUTOR DRIVE — the transpiled game-tick REPLACES the SH-4 (MAPLECAST_EXECUTOR).
// Unlike onFrame() (read-only validation on a COPY), this runs the tick IN PLACE on the
// AUTHORITATIVE mem_b: the executor IS the game. Same recipe proven byte-exact offline
// (engine_loop / oracle_diff): inject Input_DEC from the live pad latch, run one game-logic
// tick, reset the render-queue arena/counters, and recompose the render projection so the
// /replica-live wire ships render-ready state. The existing client renders it UNCHANGED.
// ============================================================================

static const unsigned IN_MATCH = 0x8C289624;   // in-match flag (same as onFrame / the .mctele tap)

// DC pad (active-low) -> CPS2 latched button bits (active-high). Byte-identical to the offline
// executor_server_net.c / native-client replica.rs dc_to_cps2 that drove the playable demo.
static inline uint16_t dc_to_cps2(uint16_t btn, uint8_t ltv, uint8_t rtv) {
    #define DN(b) ((btn & (b)) == 0)
    uint16_t m = 0;
    if (DN(0x0010)) m |= 0x2000; if (DN(0x0020)) m |= 0x1000; if (DN(0x0040)) m |= 0x0800; if (DN(0x0080)) m |= 0x0400;
    if (DN(0x0008)) m |= 0x8000; if (DN(0x0400)) m |= 0x0200; if (DN(0x0200)) m |= 0x0100;
    if (DN(0x0004)) m |= 0x0040; if (DN(0x0002)) m |= 0x0020;
    if (ltv >= 0x80) m |= 0x0080; if (rtv >= 0x80) m |= 0x0010;
    return m;
    #undef DN
}

static inline void wr16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

static int  g_drive = -1;                       // -1 unresolved; 0/1 from MAPLECAST_EXECUTOR
static uint16_t g_prevIn[2] = { 0, 0 };         // per-slot previous CPS2 latch (edge computation)
static long g_driveFrames = 0;
static std::chrono::steady_clock::time_point g_pace; static bool g_paceInit = false;

static bool driveOn() {
    if (g_drive < 0) {
        const char *e = getenv("MAPLECAST_EXECUTOR");
        g_drive = (e && e[0] && e[0] != '0') ? 1 : 0;
        if (g_drive) printf("[EXECUTOR] ENABLED — transpiled game-tick DRIVES the authoritative mem_b "
                            "(SH-4 game-tick replaced; /replica-live broadcasts it)\n");
    }
    return g_drive == 1;
}

bool driveFrame() {
    if (!driveOn()) return false;
    // In-match gate — the executor only reproduces the in-match tick (loc_8c0358be). Outside a
    // match, hand back to the SH-4 (menus / char-select / transitions are not transpiled).
    if (addrspace::read8(IN_MATCH) == 0) { g_paceInit = false; return false; }
    // HANDOVER: let the SH-4 render the first in-match frames so the /replica-live static prefix
    // (VRAM 8MB + PVR palette + GFX tables) is captured from a real render pass before we take
    // over. Bodies decode from RAM thereafter (VRAM static), so one valid capture suffices.
    if (!maplecast_replica_live::prefixReady()) return false;

    unsigned char *ram = (unsigned char *)&mem_b[0];

    // 1. Inject Input_DEC (P1 @0x2681DC, P2 @0x2681F0; stride 0x14) from the live pad latch.
    //    kcode is active-low DC buttons | 0xFFFF0000; lt/rt hold the 0-255 trigger in the high byte.
    for (int p = 0; p < 2; p++) {
        uint16_t btn = (uint16_t)(kcode[p] & 0xFFFFu);
        uint16_t cur = dc_to_cps2(btn, (uint8_t)(lt[p] >> 8), (uint8_t)(rt[p] >> 8));
        uint16_t prev = g_prevIn[p];
        unsigned char *id = ram + 0x2681DC + p * 0x14;
        wr16(id + 0, cur); wr16(id + 2, prev);
        wr16(id + 4, (uint16_t)(cur & ~prev)); wr16(id + 6, (uint16_t)(prev & ~cur));
        g_prevIn[p] = cur;
    }
    // 2. Arena frame-setup (render-queue tile arena; engine_loop recipe).
    *(uint32_t *)(ram + 0x1F9D98) = 0; *(uint32_t *)(ram + 0x1F9D94) = 16;
    // 3. Authoritative game-logic tick, byte-exact vs flycast, IN PLACE on mem_b.
    mc_shadow_run_tick(ram);
    // 4. Render-queue counter reset.
    ram[0x289F80] = ram[0x289F81] = ram[0x289F82] = ram[0x289F83] = 0;
    // 5. Recompose the render projection @0x2D6AD8 from the driven matrix stack (dynamic camera).
    mc_shadow_compose_proj(ram);
    // 6. Slot-count clamp (guard the render draw-list counts).
    for (int L = 0; L < 16; L++) if (ram[0x2895E0 + L] > 0x60) ram[0x2895E0 + L] = 0x60;
    // 7. Advance the guest vframe (0x3496B0) — the executor masks it (vsync-owned), but
    //    onRenderFrame dedups on it, so bump it or every capture past the first is dropped.
    { uint32_t v = *(uint32_t *)(ram + 0x3496B0) + 1u; *(uint32_t *)(ram + 0x3496B0) = v; }
    // 8. Broadcast this frame via /replica-live (STARTRENDER is gone with the SH-4 render pass).
    maplecast_replica_live::onRenderFrame(nullptr);
    g_driveFrames++;

    // 9. Pace to ~60 fps real-time — the SH-4's frame throttle went away with ->Run().
    auto now = std::chrono::steady_clock::now();
    if (g_paceInit) {
        auto target = g_pace + std::chrono::microseconds(16667);
        if (now < target) { std::this_thread::sleep_until(target); g_pace = target; }
        else g_pace = now;   // fell behind — don't accumulate debt
    } else { g_pace = now; g_paceInit = true; }

    if ((g_driveFrames % 300) == 0)
        printf("[EXECUTOR] drove %ld authoritative frames (SH-4 game-tick bypassed)\n", g_driveFrames);
    return true;
}

} // namespace maplecast_shadow_exec

#endif // MAPLECAST_SHADOW_EXEC_BUILD
