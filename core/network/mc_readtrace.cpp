// mc_readtrace.cpp — see mc_readtrace.h. STEP 2 read-set delta measurement +
// STEP 3 seed dump (entry RAM image + CPU context for the isolated real-opcode run).
#include "mc_readtrace.h"
#include "hw/sh4/sh4_if.h"     // Sh4cntx (p_sh4rcb->cntx.r[16], .pr)
#include "hw/sh4/sh4_mem.h"    // addrspace::read32, mem_b (raw 16MB RAM), RAM_SIZE
#include "hw/sh4/modules/ccn.h" // CCN[18] (QACR0/1 — the one shippable non-RAM dep)
#include "cfg/option.h"        // config::DynarecEnabled
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <set>
#include <cstdint>

namespace mc_readtrace {

bool g_enabled = false;
bool g_armed   = false;

static const u32 AREA_MASK    = 0x1FFFFFFFu;
static const u32 DRIVER_PC    = 0x8C030858u & AREA_MASK;   // loc_8c030858 render driver
static bool g_done   = false;      // one-shot: capture the FIRST driver call only
static u32  g_spEntry = 0;
static u32  g_retPc   = 0;

// dynarec->interpreter flip state (boot dynarec-fast, trace one interpreted frame)
static u32  g_triggerFrame  = 60;      // env MAPLECAST_READTRACE_FRAME
static u32  g_frameCount    = 0;
static bool g_flipRequested = false;   // trigger reached; ask vblank to Stop
static bool g_flipApplied   = false;   // DynarecEnabled flipped (once)

// 16 MB system RAM (0x0C000000..0x0CFFFFFF area). Bitmap = 1 bit / byte = 2 MB.
static const u32 RT_RAM_LO = 0x0C000000u;
static const u32 RT_RAM_SZ = 16u * 1024u * 1024u;
static uint8_t*  g_ramBits = nullptr;           // READS within the closure
static uint8_t*  g_ramWr   = nullptr;           // WRITES within the closure
static uint8_t*  g_ramRbw  = nullptr;           // READ-BEFORE-WRITE (genuine external read)
// Distinct non-RAM reads (PVR regs, VRAM, store-queue, etc.).
static std::set<u32> g_other;

// STEP 3 byte-gate: the ENGINE's native store-queue TA emission during the driver
// window (same 32-byte parcels the standalone runner captures via doSqWrite).
static std::vector<uint8_t> g_engineTa;   // appended 32B per SQ flush while armed

static inline void setRamBit(u32 off){ g_ramBits[off >> 3] |= (uint8_t)(1u << (off & 7)); }
static inline bool getRamBit(u32 off){ return (g_ramBits[off >> 3] >> (off & 7)) & 1u; }
static inline void setWrBit (u32 off){ g_ramWr  [off >> 3] |= (uint8_t)(1u << (off & 7)); }
static inline bool getWrBit (u32 off){ return (g_ramWr  [off >> 3] >> (off & 7)) & 1u; }
static inline void setRbwBit(u32 off){ g_ramRbw [off >> 3] |= (uint8_t)(1u << (off & 7)); }
static inline bool getRbwBit(u32 off){ return (g_ramRbw [off >> 3] >> (off & 7)) & 1u; }

// --- region classifier for the DUMP (not the hot path) -----------------------
struct Region { u32 lo, hi; const char* name; int shippable; };
// shippable=1: the wire already ships it, OR it is trivially resident (game code /
// match- or stage-loaded 3D art / render globals rebuilt or seeded once). Reclass
// EMPIRICALLY confirmed against the frame-90 capture + re_kb/64 render read-set:
// EVERY delta address fell into resident 3D art, re_kb/64 render globals, or the
// objpool render nodes — ZERO game-state-page spread / input pad / physics/AI.
// shippable=0: game-loop state that would DISPROVE the bounded-read claim.
static const Region REGIONS[] = {
    {0x0C000000u, 0x0C010000u, "BIOS/low-RAM (below EntryPoint)",        1},
    {0x0C010000u, 0x0C1F0000u, "GAME CODE + literal pools (resident)",   1},
    {0x0C1F9000u, 0x0C1FC000u, "RENDER SCRATCH arena/desctable (rebuilt)",1},
    {0x0C200000u, 0x0C268000u, "GFX/heap (art blobs; node+0x15C/0x160)", 1},
    {0x0C268000u, 0x0C26A600u, "CHAR STRUCTS P1C1..P2C3 (shipped)",      1},
    {0x0C26A600u, 0x0C280000u, "objpool/object+satellite render nodes (built/shipped)", 1},
    // re_kb/64 render GLOBALS (resident, seeded once): list heads 0x8C287A5C +
    // slot/layer tables 0x8C288xxx, emit table 0x8C2AA508, param block 0x8C2DEE20,
    // proj/viewport matrices 0x8C2D6xxx, divisor 0x8C32B448 (+0x8C32Dxxx).
    {0x0C287000u, 0x0C289000u, "render list/slot/layer tables (re_kb/64 global)", 1},
    {0x0C289000u, 0x0C28A000u, "GAME-STATE page (shipped)",             1},
    {0x0C2AA000u, 0x0C2AB000u, "emit table 0x8C2AA508 (re_kb/64 global)",1},
    {0x0C2D6000u, 0x0C2D7000u, "CAMERA/proj/viewport matrices (shipped)",1},
    {0x0C2DA000u, 0x0C2DC000u, "rectab/idxtab (shipped)",               1},
    {0x0C2DE000u, 0x0C2DF000u, "param block 0x8C2DEE20 (re_kb/64 global)",1},
    {0x0C32B000u, 0x0C32E000u, "divisor 0x8C32B448 / render params (re_kb/64)", 1},
    {0x0C349000u, 0x0C34A000u, "frame_counter/global (shipped)",        1},
    // RESIDENT 3D model / POL / effect art (match/stage-loaded, like GFX1/2). The
    // dominant delta 0x8CE8-0x8CEA sits just below Effect Poly 0x8CED0000; the
    // 0x8C40-0x8C90 spans are POL/effect blobs the render walks to draw.
    {0x0C400000u, 0x0C900000u, "RESIDENT POL/effect 3D art (0x8C4x-8x)", 1},
    {0x0C900000u, 0x0CE00000u, "RESIDENT heap/art (mid)",               1},
    {0x0CE00000u, 0x0CF00000u, "RESIDENT 3D model/POL/Effect art (0x8CE8-ED)", 1},
    // Anything not matched above falls into UNCLASSIFIED and is scrutinized as
    // the potential DELTA (input pad / AI / physics / other prior-frame globals).
};

static const char* classify(u32 masked, int* shippable){
    for (const auto& r : REGIONS)
        if (masked >= r.lo && masked < r.hi) { *shippable = r.shippable; return r.name; }
    *shippable = 0;
    return "UNCLASSIFIED (candidate DELTA)";
}

static void dump(){
    const char* outPath = std::getenv("MAPLECAST_READTRACE_OUT");
    if (!outPath || !outPath[0]) outPath = "readtrace_delta.txt";
    FILE* f = std::fopen(outPath, "w");
    if (!f) { std::fprintf(stderr, "[readtrace] cannot open %s\n", outPath); return; }

    // Coalesce set RAM bits into runs; histogram by region; collect DELTA runs.
    struct Bucket { const char* name; int shippable; u64 bytes; u32 runs; };
    std::vector<Bucket> buckets;
    auto bump = [&](const char* name, int ship, u32 len){
        for (auto& b : buckets) if (b.name==name){ b.bytes+=len; b.runs++; return; }
        buckets.push_back({name, ship, len, 1});
    };

    std::fprintf(f, "=== STEP 2 READ-SET DELTA (refined): driver loc_8c030858 closure (one frame) ===\n");
    std::fprintf(f, "spEntry=0x%08X retPc=0x%08X\n", g_spEntry, g_retPc);
    std::fprintf(f, "A read of an address WRITTEN earlier this closure = build-then-read scratch\n");
    std::fprintf(f, "(NOT an external dependency). TRUE external delta = read && !written && !resident.\n\n");
    std::fprintf(f, "--- TRUE-EXTERNAL DELTA runs (read, never written this closure, unclassified) ---\n");

    u64 totalBytes = 0;      // all distinct bytes read in the closure
    u64 rawDelta   = 0;      // read && unclassified (pre write-refinement)
    u64 btr        = 0;      // read && unclassified && every read AFTER a write (build-then-read)
    u64 trueDelta  = 0;      // read && unclassified && read-before-write (genuine external)
    u32 run_start = 0; bool in_run = false;
    for (u32 off = 0; off <= RT_RAM_SZ; off++){
        bool bit = (off < RT_RAM_SZ) && getRamBit(off);
        int ship = 1;
        bool ext = false;
        if (bit){ classify(RT_RAM_LO + off, &ship);
                  totalBytes++;
                  if (!ship){ rawDelta++;
                      // genuine external iff a read preceded any in-closure write
                      if (getRbwBit(off)){ trueDelta++; ext = true; }
                      else                 btr++;        // build-then-read scratch
                  } }
        if (ext && !in_run){ in_run = true; run_start = off; }
        else if (!ext && in_run){
            in_run = false;
            u32 lo = RT_RAM_LO + run_start, hi = RT_RAM_LO + off, len = off - run_start;
            int s; const char* name = classify(lo, &s);
            std::fprintf(f, "  EXTERN 0x%08X..0x%08X  %6u B   %s\n", lo, hi, len, name);
        }
    }
    if (trueDelta == 0) std::fprintf(f, "  (none — zero genuine external reads)\n");

    // Region histogram over ALL read bytes (resident + scratch + any external).
    for (u32 off = 0; off < RT_RAM_SZ; off++){
        if (!getRamBit(off)) continue;
        int ship; const char* name = classify(RT_RAM_LO + off, &ship);
        // read-after-write of an unclassified byte = build-then-read scratch bucket
        if (!ship && !getRbwBit(off)) bump("BUILD-THEN-READ scratch (written before read)", 1, 1);
        else bump(name, ship, 1);
    }

    std::fprintf(f, "\n--- NON-RAM distinct reads (PVR regs / VRAM / SQ / P4) ---\n");
    for (u32 a : g_other) std::fprintf(f, "  0x%08X\n", a);

    std::fprintf(f, "\n--- REGION HISTOGRAM (all distinct read bytes) ---\n");
    for (auto& b : buckets)
        std::fprintf(f, "  %-52s %8llu B  %s\n", b.name,
                     (unsigned long long)b.bytes,
                     b.shippable ? "resident/shippable" : ">>> TRUE DELTA <<<");

    std::fprintf(f, "\n=== VERDICT INPUTS ===\n");
    std::fprintf(f, "total distinct RAM bytes read      : %llu\n", (unsigned long long)totalBytes);
    std::fprintf(f, "raw delta (read && unclassified)   : %llu\n", (unsigned long long)rawDelta);
    std::fprintf(f, "  of which BUILD-THEN-READ (written): %llu\n", (unsigned long long)btr);
    std::fprintf(f, "TRUE EXTERNAL delta (read,!written,!resident): %llu\n", (unsigned long long)trueDelta);
    std::fprintf(f, "non-RAM distinct addresses         : %zu\n", g_other.size());
    std::fclose(f);
    std::fprintf(stderr, "[readtrace] DONE. total=%lluB rawDelta=%lluB buildThenRead=%lluB TRUE-EXTERNAL=%lluB nonram=%zu -> %s\n",
                 (unsigned long long)totalBytes, (unsigned long long)rawDelta,
                 (unsigned long long)btr, (unsigned long long)trueDelta,
                 g_other.size(), outPath);
}

// --- public API --------------------------------------------------------------
void init(){
    g_enabled = (std::getenv("MAPLECAST_READTRACE") != nullptr);
    if (!g_enabled) return;
    g_ramBits = (uint8_t*)std::calloc(RT_RAM_SZ/8, 1);
    g_ramWr   = (uint8_t*)std::calloc(RT_RAM_SZ/8, 1);
    g_ramRbw  = (uint8_t*)std::calloc(RT_RAM_SZ/8, 1);
    if (const char* f = std::getenv("MAPLECAST_READTRACE_FRAME")) {
        u32 v = (u32)std::strtoul(f, nullptr, 10);
        if (v) g_triggerFrame = v;
    }
    // Boot stays DYNAREC (fast). We flip to interpreter at g_triggerFrame.
    std::fprintf(stderr, "[readtrace] ENABLED — dynarec boot; flip->interpreter+arm at frame %u (driver 0x8C030858)\n",
                 g_triggerFrame);
}

void onFrame(){
    if (!g_enabled || g_flipApplied) return;
    if (++g_frameCount >= g_triggerFrame) g_flipRequested = true;
}

bool flipStopPending(){
    return g_enabled && g_flipRequested && !g_flipApplied;
}

bool applyFlip(){
    if (!g_enabled || !g_flipRequested || g_flipApplied) return false;
    g_flipApplied = true;
    config::DynarecEnabled.override(false);   // getSh4Executor() now returns interpreter
    std::fprintf(stderr, "[readtrace] FLIP at frame %u — interpreter armed; tracing next driver call\n",
                 g_frameCount);
    return true;   // caller does ResetCache()+Start()
}

// STEP 3 SEED DUMP — at the driver-entry instant, snapshot the FULL entry state so
// the isolated real-opcode run can be reconstructed offline:
//   header: magic, entryPC, spEntry, retPc, sizeof(Sh4Context), RAM_SIZE
//   body:   raw Sh4Context (all CPU/FP regs at entry) + raw 16MB mem_b (entry RAM)
// The isolated runner (see HANDOFF) loads this, ZEROS every RAM byte outside the
// resident read-set regions, restores the CPU context, sets PC=0x8C030858, and runs
// the REAL Sh4 core to retPc. Control variant (no zeroing) validates the harness ==
// mirror; isolated variant is the make-or-break test. One-shot, gated on env.
static void step3_seed_dump(){
    const char* p = std::getenv("MAPLECAST_READTRACE_SEED");
    if (!p || !p[0]) p = "rt_seed.bin";
    FILE* f = std::fopen(p, "wb");
    if (!f){ std::fprintf(stderr, "[readtrace] STEP3 seed: cannot open %s\n", p); return; }
    // RTSEED02: header + CCN[18] (on-chip CPU regs incl QACR0/1 — the one non-RAM
    // dep the runner found) + raw Sh4Context + raw 16MB RAM. Seeding CCN lets the
    // standalone runner run with ZERO stubbed registers (byte-gate faithfulness).
    const char magic[8] = {'R','T','S','E','E','D','0','2'};
    u32 entryPC = 0x8C030858u;
    u32 ctxSz   = (u32)sizeof(Sh4cntx);
    u32 ramSz   = (u32)RAM_SIZE;
    u32 ccnSz   = (u32)sizeof(CCN);            // 18 * 4 = 72 bytes
    std::fwrite(magic, 1, 8, f);
    std::fwrite(&entryPC, 4, 1, f);
    std::fwrite(&g_spEntry, 4, 1, f);
    std::fwrite(&g_retPc, 4, 1, f);
    std::fwrite(&ctxSz, 4, 1, f);
    std::fwrite(&ramSz, 4, 1, f);
    std::fwrite(&ccnSz, 4, 1, f);
    std::fwrite(CCN, 1, ccnSz, f);             // on-chip CCN regs (QACR0/1 @ idx 14/15)
    std::fwrite(&Sh4cntx, 1, ctxSz, f);        // entry CPU/FP register context
    std::fwrite(&mem_b[0], 1, ramSz, f);       // entry 16MB main RAM
    std::fclose(f);
    std::fprintf(stderr, "[readtrace] STEP3 SEED(RTSEED02) written (%u B ccn + %u B ctx + %u B RAM) QACR0=0x%08X QACR1=0x%08X entryPC=0x%08X spEntry=0x%08X retPc=0x%08X -> %s\n",
                 ccnSz, ctxSz, ramSz, CCN[14], CCN[15], entryPC, g_spEntry, g_retPc, p);
}

// STEP 3 ISOLATE — zero every main-RAM byte OUTSIDE the resident read-set regions,
// in place, at the driver-entry instant. Keeps game code, the SH4 stack (low RAM),
// and the enumerated resident read-set (char/object structs, GFX1/2, resident POL/
// effect art, re_kb/64 render globals, camera, rectab/idxtab, render scratch). The
// read-set analysis found ZERO reads outside these regions, so if the real render
// then RUNS TO COMPLETION the read-set is proven complete; if it FAULTS, the fatal
// handler's epc/expEvn names exactly what a mid-frame entry still needs (a shippable
// one-time init, or — the only killer — a genuine game-loop dependency). Destructive
// one-shot: the emulator state is intentionally trashed after; capture + exit.
static bool g_isolated = false;
static void step3_zero_non_resident(){
    u8* ram = &mem_b[0];
    u32 n = (u32)RAM_SIZE;
    u64 zeroed = 0;
    for (u32 off = 0; off < n; off++){
        int ship; classify(RT_RAM_LO + off, &ship);   // masked-area classify
        if (!ship){ ram[off] = 0; zeroed++; }
    }
    g_isolated = true;
    std::fprintf(stderr, "[readtrace] STEP3 ISOLATE — zeroed %llu non-resident RAM bytes; running REAL render from 0x%08X on resident-only RAM\n",
                 (unsigned long long)zeroed, DRIVER_PC | 0x80000000u);
}

void onPc(u32 pc){
    if (!g_enabled || g_done) return;
    u32 m = pc & AREA_MASK;
    if (!g_armed){
        if (m == DRIVER_PC){
            g_armed   = true;
            g_spEntry = Sh4cntx.r[15];
            g_retPc   = Sh4cntx.pr & AREA_MASK;   // caller return addr (pre sts.l pr)
            // STEP 3: dump the entry seed BEFORE the driver runs (RAM/regs = entry state).
            if (std::getenv("MAPLECAST_READTRACE_STEP3")) step3_seed_dump();
            // STEP 3 ISOLATE: zero non-resident RAM so the render runs on resident-only.
            if (std::getenv("MAPLECAST_READTRACE_ISOLATE")) step3_zero_non_resident();
        }
        return;
    }
    // Armed: disarm the instant control returns to the caller's return address
    // at/above the entry frame (excludes the sibling HUD-pass driver call).
    if (m == g_retPc && Sh4cntx.r[15] >= g_spEntry){
        g_armed = false; g_done = true;
        if (g_isolated)
            std::fprintf(stderr, "[readtrace] STEP3 ISOLATE RESULT: RAN TO COMPLETION — real-opcode render reached retPc 0x%08X on RESIDENT-ONLY RAM (read-set PROVEN complete; no fault). Emitted-TA vs mirror = the byte/pixel gate.\n",
                         g_retPc | 0x80000000u);
        // STEP 3 byte-gate: write the ENGINE's native SQ-emitted TA for this exact
        // driver call (entry..retPc). Directly comparable to the runner's ta_out.bin.
        if (!g_engineTa.empty()){
            const char* tp = std::getenv("MAPLECAST_READTRACE_ENGINE_TA");
            if (!tp || !tp[0]) tp = "engine_ta.bin";
            if (FILE* tf = std::fopen(tp, "wb")){
                std::fwrite(g_engineTa.data(), 1, g_engineTa.size(), tf);
                std::fclose(tf);
                std::fprintf(stderr, "[readtrace] STEP3 ENGINE TA written: %zu parcels (%zu bytes) -> %s\n",
                             g_engineTa.size()/32, g_engineTa.size(), tp);
            }
        }
        dump();
    }
}

void onRead(u32 addr){
    // Scope to the driver's own call subtree (deeper stack than entry).
    if (Sh4cntx.r[15] >= g_spEntry) return;
    u32 m = addr & AREA_MASK;
    if (m >= RT_RAM_LO && m < RT_RAM_LO + RT_RAM_SZ){
        u32 off = m - RT_RAM_LO;
        setRamBit(off);
        // If this byte has NOT been written yet this closure, the value read was
        // NOT produced here -> a genuine external read (read-before-write). Sticky.
        if (!getWrBit(off)) setRbwBit(off);
    }
    else g_other.insert(m);
}

void onWrite(u32 addr){
    // Same subtree scope. A write marks the byte as build-then-read scratch: a
    // later read of it is not an external dependency (the pass produced it).
    if (Sh4cntx.r[15] >= g_spEntry) return;
    u32 m = addr & AREA_MASK;
    if (m >= RT_RAM_LO && m < RT_RAM_LO + RT_RAM_SZ) setWrBit(m - RT_RAM_LO);
}

// STEP 3 byte-gate: capture the 32-byte store-queue parcel the driver is about to
// flush (pref @Rn into the SQ area). Gated on g_armed by the caller so it only
// fires inside the driver window (entry..retPc) — the SAME window the standalone
// runner captures, so g_engineTa is byte-comparable to runner_ta.bin.
void onSqWrite(u32 dest, Sh4Context* ctx){
    const uint8_t* sq = ctx->sq_buffer[(dest >> 5) & 1].data;
    g_engineTa.insert(g_engineTa.end(), sq, sq + 32);
}

} // namespace mc_readtrace
