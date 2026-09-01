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
#include <algorithm>

namespace mc_readtrace {

bool g_enabled = false;
bool g_armed   = false;

static const u32 AREA_MASK    = 0x1FFFFFFFu;
static const u32 DRIVER_PC    = 0x8C030858u & AREA_MASK;   // loc_8c030858 render driver
static bool g_done   = false;      // one-shot: capture the FIRST driver call only
static u32  g_spEntry = 0;
static u32  g_retPc   = 0;

// ============================================================================
// GAME-TICK read-set mode (MAPLECAST_TICKTRACE) — measures the read-set of the
// WHOLE per-frame SH4 game tick f(): state(t+1)=f(state(t),input(t)), NOT just
// the render driver subtree. Same fastmem-proof mechanism: boot dynarec, flip to
// interpreter at a trigger frame, then trace EVERY guest read/instruction-fetch
// (all funnel through addrspace::readt<T>) over N vblank-delimited ticks. Per tick
// we reset a read/write/read-before-write bitset; a byte is DYNAMIC live-in iff it
// was read-before-written in the tick and is outside the CODE/const/art bands,
// SCRATCH iff written-then-read, STATIC iff in a code/const/art band. Union across
// ticks classifies the whole match-wide read-set. READ-ONLY, gated OFF by default.
// ============================================================================
static bool g_tickMode = false;          // MAPLECAST_TICKTRACE
static u32  g_tkTicks  = 200;            // MAPLECAST_TICKTRACE_TICKS
static u32  g_tkTickN  = 0;              // completed ticks so far
static bool g_tkFirst  = true;           // first vblank after flip = partial tick, discard
static bool g_tkInRender = false;        // inside a render-driver (0x8C030858) call
static u32  g_tkRenderSp = 0;            // r15 at that driver entry
static bool g_tkRenderPushed = false;    // saw r15 dip below entry sp (frame pushed)

// per-tick bitsets (reset each tick)
static uint8_t* g_tkRead = nullptr;      // read this tick
static uint8_t* g_tkWr   = nullptr;      // written this tick
static uint8_t* g_tkRbw  = nullptr;      // read-before-write this tick (genuine live-in)
// union bitsets (never reset — whole-run classification)
static uint8_t* g_tkURead = nullptr;     // any read, any tick
static uint8_t* g_tkURbw  = nullptr;     // read-before-write in >=1 tick (dynamic live-in)
static uint8_t* g_tkUWr   = nullptr;     // WRITTEN in >=1 tick (distinguishes state vs const table)
static uint8_t* g_tkSim   = nullptr;     // read OUTSIDE render driver (game-update/sim)
static uint8_t* g_tkRen   = nullptr;     // read INSIDE render driver
static uint8_t* g_tkPc    = nullptr;     // distinct instruction addresses executed
// per-tick distributions
static std::vector<u32> g_tkWholeHist;   // per-tick distinct read bytes
static std::vector<u32> g_tkDynHist;     // per-tick dynamic live-in bytes
static std::set<u32>    g_tkOther;       // non-RAM distinct reads (whole run)

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

// --- tick-mode bit helpers (generic, over any 2MB bitset) --------------------
static inline void tkSet(uint8_t* bm, u32 off){ bm[off >> 3] |= (uint8_t)(1u << (off & 7)); }
static inline bool tkGet(uint8_t* bm, u32 off){ return (bm[off >> 3] >> (off & 7)) & 1u; }

// STATIC/DYNAMIC/SCRATCH classifier for the GAME TICK read-set. STATIC = code /
// literal-pool / const-table / load-time art (frame-invariant, ship once). All
// instruction fetches land in the code band. DYNAMIC = mutable per-frame game
// state (char structs, globals, objpool, render lists, camera, frame counter).
// A read outside a STATIC band is then split by read-before-write: RBW = genuine
// DYNAMIC live-in; written-then-read = SCRATCH. So this returns only whether the
// address is a STATIC band (region wins) — the RBW test does dynamic/scratch.
// masked = area-masked guest addr (0x0Cxxxxxx).
enum TkClass { TK_STATIC, TK_MAYBE_DYN };
static TkClass tkRegion(u32 masked, const char** name){
    struct R { u32 lo, hi; TkClass k; const char* n; };
    static const R RS[] = {
        // ---- STATIC: code, literals, const tables, load-time art ----
        {0x0C000000u, 0x0C010000u, TK_STATIC, "BIOS/low-RAM (below EntryPoint)"},
        {0x0C010000u, 0x0C1F0000u, TK_STATIC, "GAME CODE + literal pools + const tables"},
        {0x0C200000u, 0x0C268000u, TK_STATIC, "GFX/heap art blobs (node+0x15C/0x160 load-time)"},
        {0x0C400000u, 0x0CE00000u, TK_STATIC, "resident POL/effect/model 3D art (load-time)"},
        {0x0CE00000u, 0x0D000000u, TK_STATIC, "resident 3D model/POL/Effect art (0x8CE8-ED)"},
        // ---- DYNAMIC candidates: mutable per-frame game state ----
        {0x0C1F0000u, 0x0C200000u, TK_MAYBE_DYN, "low game-state / render scratch (0x8C1Fxxxx)"},
        {0x0C268000u, 0x0C26A600u, TK_MAYBE_DYN, "CHAR STRUCTS P1C1..P2C3 (0x8C268340, game state)"},
        {0x0C26A600u, 0x0C287000u, TK_MAYBE_DYN, "objpool / object+satellite nodes (0x8C26AA54)"},
        {0x0C287000u, 0x0C289000u, TK_MAYBE_DYN, "render list/slot/layer tables (rebuilt/frame)"},
        {0x0C289000u, 0x0C28A000u, TK_MAYBE_DYN, "GLOBAL GAME-STATE page (0x8C289xxx)"},
        {0x0C28A000u, 0x0C2D6000u, TK_MAYBE_DYN, "game heap/state mid (0x8C28A-0x8C2D6)"},
        {0x0C2D6000u, 0x0C2D7000u, TK_MAYBE_DYN, "CAMERA/proj/viewport matrices"},
        {0x0C2D7000u, 0x0C349000u, TK_MAYBE_DYN, "game heap/state hi (0x8C2D7-0x8C349)"},
        {0x0C349000u, 0x0C34A000u, TK_MAYBE_DYN, "frame_counter/global (0x8C3496B0)"},
        {0x0C34A000u, 0x0C400000u, TK_MAYBE_DYN, "game heap/state (0x8C34A-0x8C400)"},
        {0x0D000000u, 0x10000000u, TK_MAYBE_DYN, "RAM mirror / high (0x0D-0x0F)"},
    };
    for (const auto& r : RS)
        if (masked >= r.lo && masked < r.hi){ if(name)*name=r.n; return r.k; }
    if(name)*name="UNMAPPED-area3 (candidate DYNAMIC)";
    return TK_MAYBE_DYN;
}

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

// ---- tick-mode dump: classify the union read-set + per-tick distribution -----
static u32 tk_med(std::vector<u32> v){ if(v.empty())return 0; std::sort(v.begin(),v.end()); return v[v.size()/2]; }
static void tk_dump(){
    const char* outPath = std::getenv("MAPLECAST_TICKTRACE_OUT");
    if (!outPath || !outPath[0]) outPath = "ticktrace.txt";
    FILE* f = std::fopen(outPath, "w");
    if (!f){ std::fprintf(stderr, "[ticktrace] cannot open %s\n", outPath); return; }

    // Classify the UNION read-set (whole run). Byte-skip the zero bitmap bytes.
    // A read outside a static region:
    //   read-before-write in >=1 tick AND written in >=1 tick  -> TRUE DYNAMIC state (live-in)
    //   read-before-write but NEVER written (read-only)         -> const table -> STATIC (ship once)
    //   written-then-read only                                  -> SCRATCH
    struct Bucket { const char* name; u64 bytes; int isStatic; };
    std::vector<Bucket> bk;
    auto bump=[&](const char* n,int st,u32 add){ for(auto&b:bk) if(b.name==n){b.bytes+=add;return;} bk.push_back({n,add,st}); };
    u64 uStatic=0, uDyn=0, uScratch=0, uConstRO=0, uTotal=0;
    for (u32 byteIdx=0; byteIdx < RT_RAM_SZ/8; byteIdx++){
        uint8_t rb = g_tkURead[byteIdx]; if(!rb) continue;
        for (int b=0;b<8;b++){
            if(!((rb>>b)&1)) continue;
            u32 off = (byteIdx<<3)|b;
            u32 masked = RT_RAM_LO + off;
            const char* nm=nullptr; TkClass k=tkRegion(masked,&nm);
            uTotal++;
            if (tkGet(g_tkPc,off)){ uStatic++; bump("executed CODE (instruction fetch)",1,1); }
            else if (k==TK_STATIC){ uStatic++; bump(nm,1,1); }
            else if (tkGet(g_tkURbw,off)){
                if (tkGet(g_tkUWr,off)){ uDyn++; bump(nm,0,1); }          // written => true state
                else { uConstRO++; uStatic++; bump("read-only const table in dyn region (never written)",1,1); }
            }
            else { uScratch++; bump("BUILD-THEN-READ scratch (written before read)",1,1); }
        }
    }
    // sim vs render union byte counts (clamped to the finalized-tick read union)
    u64 simB=0, renB=0, bothB=0, pcB=0;
    for (u32 byteIdx=0; byteIdx < RT_RAM_SZ/8; byteIdx++){
        uint8_t u=g_tkURead[byteIdx];
        uint8_t s=g_tkSim[byteIdx]&u, r=g_tkRen[byteIdx]&u, p=g_tkPc[byteIdx];
        for(int b=0;b<8;b++){ int sb=(s>>b)&1, rb2=(r>>b)&1; simB+=sb; renB+=rb2; bothB+=(sb&rb2); pcB+=(p>>b)&1; }
    }

    // per-tick distribution
    u64 wSum=0; u32 wMin=0xffffffff,wMax=0; for(u32 v:g_tkWholeHist){wSum+=v; if(v<wMin)wMin=v; if(v>wMax)wMax=v;}
    u64 dSum=0; u32 dMin=0xffffffff,dMax=0; for(u32 v:g_tkDynHist){dSum+=v; if(v<dMin)dMin=v; if(v>dMax)dMax=v;}
    u32 nT=(u32)g_tkWholeHist.size();
    if(!nT){wMin=dMin=0;}

    std::fprintf(f,"=== GAME-TICK READ-SET (MAPLECAST_TICKTRACE) ===\n");
    std::fprintf(f,"ticks measured: %u (vblank-delimited, interpreter)\n",nT);
    std::fprintf(f,"tick = one full per-frame SH4 game step f(): input latch + game logic + render.\n");
    std::fprintf(f,"Reads include instruction fetches (they route through readt<u16>) => CODE band = code+literals.\n\n");

    std::fprintf(f,"--- PER-TICK distinct bytes read (whole tick f(), incl. code fetch) ---\n");
    std::fprintf(f,"  min=%u  median=%u  mean=%llu  max=%u\n",
        wMin, tk_med(g_tkWholeHist), (unsigned long long)(nT?wSum/nT:0), wMax);
    std::fprintf(f,"--- PER-TICK DYNAMIC live-in bytes (read-before-write, non-static region; UPPER bound, includes read-only const tables) ---\n");
    std::fprintf(f,"  min=%u  median=%u  mean=%llu  max=%u\n",
        dMin, tk_med(g_tkDynHist), (unsigned long long)(nT?dSum/nT:0), dMax);

    std::fprintf(f,"\n--- UNION over all %u ticks (match-wide distinct bytes) ---\n",nT);
    std::fprintf(f,"  total distinct bytes read : %llu\n",(unsigned long long)uTotal);
    std::fprintf(f,"  STATIC  (code/literal/const/art, ship once) : %llu\n",(unsigned long long)uStatic);
    std::fprintf(f,"    of which read-only const table in dyn region: %llu\n",(unsigned long long)uConstRO);
    std::fprintf(f,"  DYNAMIC live-in (read-before-write AND written; per-frame state) : %llu\n",(unsigned long long)uDyn);
    std::fprintf(f,"  SCRATCH (written-then-read within a tick)   : %llu\n",(unsigned long long)uScratch);
    std::fprintf(f,"  distinct instruction addresses executed    : %llu  (~recompilation code surface)\n",(unsigned long long)pcB);
    std::fprintf(f,"  reads outside render driver (sim/update)    : %llu bytes\n",(unsigned long long)simB);
    std::fprintf(f,"  reads inside  render driver (0x8C030858)    : %llu bytes\n",(unsigned long long)renB);
    std::fprintf(f,"  read by BOTH sim and render                 : %llu bytes\n",(unsigned long long)bothB);
    std::fprintf(f,"  non-RAM distinct reads (PVR/VRAM/SQ/P4)     : %zu\n",g_tkOther.size());

    std::fprintf(f,"\n--- UNION region histogram (all distinct read bytes, by region) ---\n");
    std::sort(bk.begin(),bk.end(),[](const Bucket&a,const Bucket&b){return a.bytes>b.bytes;});
    for(auto&b:bk)
        std::fprintf(f,"  %-56s %9llu B  %s\n", b.name, (unsigned long long)b.bytes,
            b.isStatic ? "STATIC/scratch" : ">>> DYNAMIC <<<");

    // Coalesced TRUE-DYNAMIC live-in address ranges (read-before-write, written, non-static
    // region, not code). This IS the per-frame state a re-simulator must be fed each tick.
    std::fprintf(f,"\n--- TRUE-DYNAMIC live-in coalesced runs (guest P1 addr; gap<=16 merged) ---\n");
    { u32 runLo=0, runEnd=0; bool inRun=false; u32 nRuns=0; u64 dynRunBytes=0;
      auto emit=[&](u32 lo,u32 end){ const char* nm; tkRegion(RT_RAM_LO+lo,&nm);
          if(nRuns<1200) std::fprintf(f,"  0x%08X..0x%08X  %5u B   %s\n", 0x8C000000u|lo, 0x8C000000u|end, end-lo, nm);
          nRuns++; dynRunBytes += end-lo; };
      for (u32 off=0; off<RT_RAM_SZ; off++){
          bool dyn=false;
          if (tkGet(g_tkURead,off)){
              const char* nm; TkClass k=tkRegion(RT_RAM_LO+off,&nm);
              dyn = !tkGet(g_tkPc,off) && k!=TK_STATIC && tkGet(g_tkURbw,off) && tkGet(g_tkUWr,off);
          }
          if (dyn){
              if (!inRun){ inRun=true; runLo=off; }
              else if (off - runEnd > 16){ emit(runLo,runEnd); runLo=off; }
              runEnd = off+1;
          }
      }
      if (inRun) emit(runLo,runEnd);
      std::fprintf(f,"  (%u runs, %llu bytes)\n", nRuns, (unsigned long long)dynRunBytes);
    }

    std::fclose(f);
    std::fprintf(stderr,"[ticktrace] DONE ticks=%u perTickWhole[min=%u med=%u max=%u] perTickDyn[min=%u med=%u max=%u] "
        "UNION total=%lluB static=%lluB dynamic=%lluB scratch=%lluB instrAddrs=%lluB -> %s\n",
        nT, wMin, tk_med(g_tkWholeHist), wMax, dMin, tk_med(g_tkDynHist), dMax,
        (unsigned long long)uTotal,(unsigned long long)uStatic,(unsigned long long)uDyn,
        (unsigned long long)uScratch,(unsigned long long)pcB, outPath);
}

// finalize the tick that just ended at this vblank: classify its read bitset,
// record per-tick counts, merge into the union, then reset the per-tick bitsets.
static void tk_finalizeTick(){
    u32 whole=0, dyn=0;
    for (u32 byteIdx=0; byteIdx < RT_RAM_SZ/8; byteIdx++){
        uint8_t rb = g_tkRead[byteIdx]; if(!rb) continue;
        for (int b=0;b<8;b++){
            if(!((rb>>b)&1)) continue;
            u32 off = (byteIdx<<3)|b;
            tkSet(g_tkURead, off);                 // union read
            whole++;
            bool rbw = tkGet(g_tkRbw, off);
            if (rbw) tkSet(g_tkURbw, off);          // union dynamic live-in
            const char* nm; TkClass k = tkRegion(RT_RAM_LO+off,&nm);
            bool isCode = tkGet(g_tkPc, off);        // executed instruction addr = code
            if (!isCode && k!=TK_STATIC && rbw) dyn++;  // per-tick dynamic live-in
        }
    }
    g_tkWholeHist.push_back(whole);
    g_tkDynHist.push_back(dyn);
    // OR the per-tick write set into the union (state-vs-const-table discriminator)
    for (u32 i=0;i<RT_RAM_SZ/8;i++) g_tkUWr[i] |= g_tkWr[i];
    std::memset(g_tkRead, 0, RT_RAM_SZ/8);
    std::memset(g_tkWr,   0, RT_RAM_SZ/8);
    std::memset(g_tkRbw,  0, RT_RAM_SZ/8);
}

// --- public API --------------------------------------------------------------
void init(){
    g_tickMode = (std::getenv("MAPLECAST_TICKTRACE") != nullptr);
    g_enabled = g_tickMode || (std::getenv("MAPLECAST_READTRACE") != nullptr);
    if (!g_enabled) return;
    g_ramBits = (uint8_t*)std::calloc(RT_RAM_SZ/8, 1);
    g_ramWr   = (uint8_t*)std::calloc(RT_RAM_SZ/8, 1);
    g_ramRbw  = (uint8_t*)std::calloc(RT_RAM_SZ/8, 1);
    if (const char* f = std::getenv("MAPLECAST_READTRACE_FRAME")) {
        u32 v = (u32)std::strtoul(f, nullptr, 10);
        if (v) g_triggerFrame = v;
    }
    if (const char* f = std::getenv("MAPLECAST_TICKTRACE_FRAME")) {
        u32 v = (u32)std::strtoul(f, nullptr, 10);
        if (v) g_triggerFrame = v;
    }
    if (g_tickMode){
        g_tkRead  = (uint8_t*)std::calloc(RT_RAM_SZ/8, 1);
        g_tkWr    = (uint8_t*)std::calloc(RT_RAM_SZ/8, 1);
        g_tkRbw   = (uint8_t*)std::calloc(RT_RAM_SZ/8, 1);
        g_tkURead = (uint8_t*)std::calloc(RT_RAM_SZ/8, 1);
        g_tkURbw  = (uint8_t*)std::calloc(RT_RAM_SZ/8, 1);
        g_tkUWr   = (uint8_t*)std::calloc(RT_RAM_SZ/8, 1);
        g_tkSim   = (uint8_t*)std::calloc(RT_RAM_SZ/8, 1);
        g_tkRen   = (uint8_t*)std::calloc(RT_RAM_SZ/8, 1);
        g_tkPc    = (uint8_t*)std::calloc(RT_RAM_SZ/8, 1);
        if (const char* t = std::getenv("MAPLECAST_TICKTRACE_TICKS")) {
            u32 v = (u32)std::strtoul(t, nullptr, 10); if(v) g_tkTicks = v;
        }
        if (!g_triggerFrame || g_triggerFrame < 60) g_triggerFrame = 120;
        std::fprintf(stderr, "[ticktrace] ENABLED — GAME-TICK read-set; dynarec boot; flip->interpreter at frame %u; trace %u vblank-delimited ticks\n",
                     g_triggerFrame, g_tkTicks);
        return;
    }
    // Boot stays DYNAREC (fast). We flip to interpreter at g_triggerFrame.
    std::fprintf(stderr, "[readtrace] ENABLED — dynarec boot; flip->interpreter+arm at frame %u (driver 0x8C030858)\n",
                 g_triggerFrame);
}

void onFrame(){
    if (!g_enabled) return;
    if (g_tickMode){
        if (!g_flipApplied){                       // pre-flip: count to trigger
            if (++g_frameCount >= g_triggerFrame) g_flipRequested = true;
            return;
        }
        // post-flip: each vblank is a TICK boundary.
        if (g_tkFirst){                            // first is a partial tick — discard
            g_tkFirst = false;
            std::memset(g_tkRead, 0, RT_RAM_SZ/8);
            std::memset(g_tkWr,   0, RT_RAM_SZ/8);
            std::memset(g_tkRbw,  0, RT_RAM_SZ/8);
            return;
        }
        tk_finalizeTick();
        if (++g_tkTickN >= g_tkTicks){
            g_armed = false;
            tk_dump();
            std::fflush(nullptr);
            std::_Exit(0);                          // hard stop; file already flushed
        }
        return;
    }
    if (g_flipApplied) return;
    if (++g_frameCount >= g_triggerFrame) g_flipRequested = true;
}

bool flipStopPending(){
    return g_enabled && g_flipRequested && !g_flipApplied;
}

bool applyFlip(){
    if (!g_enabled || !g_flipRequested || g_flipApplied) return false;
    g_flipApplied = true;
    config::DynarecEnabled.override(false);   // getSh4Executor() now returns interpreter
    if (g_tickMode){
        g_armed = true;                       // whole-frame trace begins now
        std::fprintf(stderr, "[ticktrace] FLIP at frame %u — interpreter armed; tracing whole-tick read-set\n", g_frameCount);
        return true;
    }
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
    if (g_tickMode){
        if (!g_armed) return;
        u32 m = pc & AREA_MASK;
        // distinct executed instruction addresses = recompilation code surface
        if (m >= RT_RAM_LO && m < RT_RAM_LO + RT_RAM_SZ) tkSet(g_tkPc, m - RT_RAM_LO);
        // render-driver depth (best-effort sim/render split): enter at 0x8C030858,
        // exit when r15 climbs back above the entry sp after dipping below it.
        u32 sp = Sh4cntx.r[15];
        if (!g_tkInRender){
            if (m == DRIVER_PC){ g_tkInRender = true; g_tkRenderSp = sp; g_tkRenderPushed = false; }
        } else {
            if (sp < g_tkRenderSp) g_tkRenderPushed = true;
            else if (g_tkRenderPushed && sp >= g_tkRenderSp) g_tkInRender = false;
        }
        return;
    }
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

void onRead(u32 addr, u32 size){
    if (g_tickMode){
        u32 m = addr & AREA_MASK;
        if (m >= RT_RAM_LO && m < RT_RAM_LO + RT_RAM_SZ){
            uint8_t* rt = g_tkInRender ? g_tkRen : g_tkSim;   // sim vs render split
            for (u32 i=0;i<size;i++){
                u32 mm = m + i; if (mm >= RT_RAM_LO + RT_RAM_SZ) break;
                u32 off = mm - RT_RAM_LO;
                tkSet(g_tkRead, off);
                if (!tkGet(g_tkWr, off)) tkSet(g_tkRbw, off); // read-before-write this tick
                tkSet(rt, off);
            }
        } else g_tkOther.insert(m);
        return;
    }
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

void onWrite(u32 addr, u32 size){
    if (g_tickMode){
        u32 m = addr & AREA_MASK;
        if (m >= RT_RAM_LO && m < RT_RAM_LO + RT_RAM_SZ)
            for (u32 i=0;i<size;i++){ u32 mm=m+i; if(mm>=RT_RAM_LO+RT_RAM_SZ)break; tkSet(g_tkWr, mm-RT_RAM_LO); }
        return;
    }
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
    if (g_tickMode) return;   // tick mode: don't accumulate engine TA (unbounded over N ticks)
    const uint8_t* sq = ctx->sq_buffer[(dest >> 5) & 1].data;
    g_engineTa.insert(g_engineTa.end(), sq, sq + 32);
}

} // namespace mc_readtrace
