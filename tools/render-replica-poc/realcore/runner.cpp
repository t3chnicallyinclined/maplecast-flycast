// STEP 3 standalone real-opcode render runner.
//
// Loads scratchpad/rt_seed.bin (RTSEED01): {entryPC, spEntry, retPc, ctxSz, ramSz}
// + raw Sh4Context (512B) + raw 16MB main RAM, all captured at the driver-entry
// instant of a live MVC2 frame. ZEROS every main-RAM byte OUTSIDE the resident
// read-set regions (same classifier as mc_readtrace.cpp), restores the CPU
// context, sets PC=0x8C030858, and runs flycast's REAL Sh4 INTERPRETER core
// (sh4_interpreter.cpp + sh4_opcodes.cpp + sh4_fpu.cpp + sh4_opcode_list.cpp +
// sh4_core_regs.cpp + sh4_rom.cpp) to retPc under a watchdog.
//
// Memory + SQ handlers are OURS (flat 16MB + TA capture) so we do NOT link the
// PVR/holly/addrspace subsystem. TA parcels the driver emits (via the store queue
// pref path and any direct TA-FIFO writes) are captured to ta_out.bin.
//
// OUTCOMES:
//   (a) reaches retPc  -> real opcodes render from resident-only RAM. Dump TA.
//   (b) fault          -> Do_Exception captures epc/expEvn. Report exact PC.
//   (c) watchdog       -> instruction budget exceeded (loop/hang). Report last PC.
//   plus: read-before-write of a ZEROED (non-resident) byte = read-set
//         incompleteness, logged with the PC that read it.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>

#include "types.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_cycles.h"
#include "hw/sh4/modules/mmu.h"
#include "debug/gdb_server.h"
#include "network/mc_readtrace.h"

// ---------------------------------------------------------------------------
// Guest RAM (flat, 16 MB) + read-set bookkeeping
// ---------------------------------------------------------------------------
static const u32 RAM_LO   = 0x0C000000u;
static const u32 RAM_BYTES = 16u * 1024u * 1024u;
static const u32 RAM_MASK16 = RAM_BYTES - 1;      // 0x00FFFFFF
static u8*  g_ram    = nullptr;                    // 16 MB
static u8*  g_zeroed = nullptr;                    // 1 byte/byte: was zeroed (non-resident)
static u8*  g_wrote  = nullptr;                    // 1 byte/byte: written during the run
static u32  g_ccn[18] = { 0 };                     // seeded CCN regs (RTSEED02); QACR0/1 @ 14/15
static bool g_haveCcn = false;

// run state
static u32  g_entryPC = 0, g_spEntry = 0, g_retPc = 0;
static u32  g_curPc = 0;
static u64  g_icount = 0;
static const u64 WATCHDOG = 300ull * 1000 * 1000;  // 300M instructions
static bool g_reached = false, g_watchdog = false, g_faulted = false;
// --trace: log every executed PC (post-processed into the tick's function set + resolved indirects)
static bool g_dotrace = false;
static std::vector<u32> g_pctrace;
static const size_t TRACE_CAP = 20000000;
static u32  g_faultEpc = 0, g_faultEvn = 0;

// zeroed-resident-miss log (read-before-write of a zeroed byte)
struct Miss { u32 pc, addr; };
static std::vector<Miss> g_miss;
static const size_t MISS_CAP = 64;

// non-RAM read log (PVR regs / VRAM / other)
struct Other { u32 pc, addr, sz; };
static std::vector<Other> g_other;
static const size_t OTHER_CAP = 128;

// captured TA parcels (SQ + direct FIFO)
struct TaChunk { u32 dest; u8 data[32]; };
static std::vector<TaChunk> g_ta;

// --drop-scratch per-region miss accounting: the render-scratch regions the current
// wire ships but the driver is HYPOTHESIZED to rebuild within its own char-pass closure.
// A zeroed byte read-before-write here = genuine cross-frame input (FALSIFY); zero misses
// + byte-exact TA = driver-rebuilt (CONFIRM).
struct ScratchRange { u32 lo, hi; const char* name; u64 miss; u32 firstPc, firstAddr; };
static std::vector<ScratchRange> g_scratch;
static inline void logMiss(u32 addr) {
    if (g_miss.size() < MISS_CAP) g_miss.push_back({ g_curPc, addr });
    u32 phys = addr & 0x1FFFFFFFu;
    for (auto& s : g_scratch)
        if (phys >= s.lo && phys < s.hi) {
            if (s.miss == 0) { s.firstPc = g_curPc; s.firstAddr = addr; }
            s.miss++;
            break;
        }
}
static inline void logOther(u32 addr, u32 sz) {
    if (g_other.size() < OTHER_CAP) g_other.push_back({ g_curPc, addr, sz });
}

// ---------------------------------------------------------------------------
// Memory handlers (DYNACALL to match flycast fn-pointer typedefs)
// ---------------------------------------------------------------------------
template<typename T>
static inline T readT(u32 addr) {
    u32 phys = addr & 0x1FFFFFFFu;
    if (phys >= 0x0C000000u && phys < 0x10000000u) {
        u32 off = (phys - 0x0C000000u) & RAM_MASK16;
        // read-before-write of a zeroed (non-resident) byte = incompleteness
        if (g_zeroed[off] && !g_wrote[off]) logMiss(addr);
        T v; std::memcpy(&v, &g_ram[off], sizeof(T)); return v;
    }
    // On-chip CCN registers (P4/area7 0x1F000000..0x1F0000FF, incl QACR0/1 @ 0x38/0x3C).
    // Seeded from RTSEED02 so nothing is stubbed for the byte-gate. SH4IO_REGN index
    // = (addr & 0xFF)/4. Still logged as "other" so the audit stays honest.
    if (g_haveCcn && phys >= 0x1F000000u && phys < 0x1F000100u) {
        logOther(phys, (u32)sizeof(T));
        u32 idx = (phys & 0xFFu) >> 2;
        u32 reg = (idx < 18) ? g_ccn[idx] : 0;
        // sub-word reads: shift/mask by byte offset within the 32-bit reg
        u32 sh = (phys & 3u) * 8;
        return (T)(reg >> sh);
    }
    logOther(phys, (u32)sizeof(T));
    return (T)0;
}

static void captureTa(u32 dest, const u8* src) {
    TaChunk c; c.dest = dest; std::memcpy(c.data, src, 32); g_ta.push_back(c);
}

template<typename T>
static inline void writeT(u32 addr, T v) {
    // SH4 store queue: 0xE0000000..0xE3FFFFFF -> Sh4Context.sq_buffer (two 32B queues,
    // selected by bit 5). The interpreter's mov.l lands here; pref flushes via doSqWrite.
    if ((addr >> 26) == 0x38u) {
        u8* sq = (u8*)&Sh4cntx.sq_buffer[(addr >> 5) & 1];
        std::memcpy(sq + (addr & 0x1Fu), &v, sizeof(T));
        return;
    }
    u32 phys = addr & 0x1FFFFFFFu;
    if (phys >= 0x0C000000u && phys < 0x10000000u) {
        u32 off = (phys - 0x0C000000u) & RAM_MASK16;
        g_wrote[off] = 1;
        std::memcpy(&g_ram[off], &v, sizeof(T));
        return;
    }
    // TA polygon FIFO (area 4, 0x10000000..0x13FFFFFF) — direct-write path
    if (phys >= 0x10000000u && phys < 0x14000000u) {
        // accumulate 32-bit writes into 32-byte parcels keyed by dest & ~0x1f
        static u8  fifo[32]; static u32 fbase = ~0u; static u32 fmask = 0;
        u32 base = phys & ~0x1fu; u32 idx = phys & 0x1fu;
        if (base != fbase) { fbase = base; fmask = 0; std::memset(fifo, 0, 32); }
        std::memcpy(&fifo[idx], &v, sizeof(T));
        for (u32 i = 0; i < sizeof(T); i++) fmask |= (1u << ((idx + i) & 31));
        if (fmask == 0xFFFFFFFFu) { captureTa(base, fifo); fmask = 0; }
        return;
    }
    logOther(phys, (u32)sizeof(T));
}

static u8  DYNACALL rd8 (u32 a) { return readT<u8 >(a); }
static u16 DYNACALL rd16(u32 a) { return readT<u16>(a); }
static u32 DYNACALL rd32(u32 a) { return readT<u32>(a); }
static u64 DYNACALL rd64(u32 a) { return readT<u64>(a); }
static void DYNACALL wr8 (u32 a, u8  v) { writeT<u8 >(a, v); }
static void DYNACALL wr16(u32 a, u16 v) { writeT<u16>(a, v); }
static void DYNACALL wr32(u32 a, u32 v) { writeT<u32>(a, v); }
static void DYNACALL wr64(u32 a, u64 v) { writeT<u64>(a, v); }

// store-queue write hook (pref @Rn into 0xE0000000) — the TA emission path
static void DYNACALL sqCapture(u32 dest, Sh4Context* ctx) {
    int i = (dest >> 5) & 1;
    captureTa(dest, ctx->sq_buffer[i].data);
}

// ===========================================================================
// STUBS for symbols the compiled interpreter files reference but whose owning
// TUs we do NOT compile (mem/holly/pvr/sched/interrupts/mmu/reios/cycles).
// ===========================================================================
// -- logging / fatal stubs (oslib not compiled) --
#include <cstdarg>
void GenericLog(LogTypes::LOG_LEVELS, LogTypes::LOG_TYPE, const char*, int, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); std::vfprintf(stderr, fmt, ap); va_end(ap);
    std::fputc('\n', stderr);
}
void fatal_error(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); std::vfprintf(stderr, fmt, ap); va_end(ap);
    std::fputc('\n', stderr);
}
void os_DebugBreak() { std::fprintf(stderr, "[os_DebugBreak] aborting\n"); std::abort(); }

// -- flycast global memory fn pointers + context (declared extern in headers) --
// NOTE: p_sh4rcb is DEFINED by sh4_core_regs.cpp; we only assign it in main().
RamRegion mem_b;
ReadMem8Func  ReadMem8;
ReadMem16Func ReadMem16;
ReadMem16Func IReadMem16;
ReadMem32Func ReadMem32;
ReadMem64Func ReadMem64;
WriteMem8Func  WriteMem8;
WriteMem16Func WriteMem16;
WriteMem32Func WriteMem32;
WriteMem64Func WriteMem64;

// -- CCN register file + TLB (sh4_mmr.cpp / mmu.cpp not compiled) --
u32 CCN[18] = { 0 };
TLB_Entry UTLB[64];
TLB_Entry ITLB[4];
bool mmuOn = false;
bool UTLB_Sync(u32) { return false; }
void ITLB_Sync(u32) {}

// -- interrupts / scheduler (not compiled) --
int UpdateINTC() { return 0; }
bool SRdecode() { return false; }   // sh4_interrupts.cpp not compiled; UpdateSR() calls it

// -- gsta_charpass hook (sh4_interpreter.cpp references these; runner uses its own
//    mc_readtrace::onPc watchdog, so the charpass hook stays inert here) --
bool gsta_charpass_active = false;
void gsta_charpass_onpc(u32) {}
void Do_Exception(u32 epc, Sh4ExceptionCode expEvn) {
    // Capture the fault and break the Run loop via debugger::Stop (caught in Run()).
    g_faulted = true; g_faultEpc = epc; g_faultEvn = (u32)expEvn;
    throw debugger::Stop();
}
// sh4_sched.cpp not compiled
void sh4_sched_tick(int) {}
u64  sh4_sched_now64() { return 0; }

// -- reios trap (opcode table references it) --
struct Sh4Context;
void DYNACALL reios_trap(Sh4Context*, u32) {}

// -- Sh4Cycles out-of-line methods (sh4_cycles.cpp not compiled: needs `settings`) --
int Sh4Cycles::countCycles(u16) { return 1; }
int Sh4Cycles::readExternalAccessCycles(u32, u32) { return 1; }
int Sh4Cycles::writeExternalAccessCycles(u32, u32) { return 1; }

// -- mc_readtrace: our watchdog / retPc-stop version (real .cpp not compiled) --
namespace mc_readtrace {
    bool g_enabled = true;
    bool g_armed = false;
    void onPc(u32 pc) {
        g_curPc = pc;
        if (g_dotrace && g_pctrace.size() < TRACE_CAP) g_pctrace.push_back(pc);
        if (++g_icount > WATCHDOG) { g_watchdog = true; throw debugger::Stop(); }
        if ((pc & 0x1FFFFFFFu) == g_retPc && Sh4cntx.r[15] >= g_spEntry) {
            g_reached = true; throw debugger::Stop();
        }
    }
    // referenced by the pref opcode (sh4_opcodes.cpp); runner captures via doSqWrite
    // (sqCapture) instead, and keeps g_armed=false, so this is never called here.
    void onSqWrite(u32, ::Sh4Context*) {}
}

// ===========================================================================
// Resident read-set classifier — byte-identical to mc_readtrace.cpp REGIONS.
// shippable=1 stays; shippable=0 gets zeroed.
// ===========================================================================
struct Region { u32 lo, hi; int shippable; };
static const Region REGIONS[] = {
    {0x0C000000u, 0x0C010000u, 1}, {0x0C010000u, 0x0C1F0000u, 1},
    {0x0C1F9000u, 0x0C1FC000u, 1}, {0x0C200000u, 0x0C268000u, 1},
    {0x0C268000u, 0x0C26A600u, 1}, {0x0C26A600u, 0x0C280000u, 1},
    {0x0C287000u, 0x0C289000u, 1}, {0x0C289000u, 0x0C28A000u, 1},
    {0x0C2AA000u, 0x0C2AB000u, 1}, {0x0C2D6000u, 0x0C2D7000u, 1},
    {0x0C2DA000u, 0x0C2DC000u, 1}, {0x0C2DE000u, 0x0C2DF000u, 1},
    {0x0C32B000u, 0x0C32E000u, 1}, {0x0C349000u, 0x0C34A000u, 1},
    {0x0C400000u, 0x0C900000u, 1}, {0x0C900000u, 0x0CE00000u, 1},
    {0x0CE00000u, 0x0CF00000u, 1},
};
static bool residentByte(u32 phys) {
    for (auto& r : REGIONS) if (phys >= r.lo && phys < r.hi) return true;
    return false;
}

// ===========================================================================
// main
// ===========================================================================
int main(int argc, char** argv) {
    const char* seedPath = (argc > 1) ? argv[1] : "rt_seed.bin";
    bool doIsolate = true;
    bool dropChars = false;   // negative control: also zero char structs/objpool (a NEEDED region)
    bool dropScratch = false; // GATING TEST: also zero the render-scratch the driver rebuilds
    bool minCtx = false;      // zero r0..r14 (keep r15/pc/pr/sr/fpscr) to test reg-invariance
    bool leafMode = false;    // GAME-TICK LEAF ORACLE: force pr=retPc sentinel + r15=spEntry
                              // so a self-contained leaf's rts returns to the stop sentinel.
    const char* ctxOverride = nullptr; // load entry ctx from a DIFFERENT seed than the RAM
    // entry-register overrides for REG-ARG leaves (--setr=IDX:HEX / --setfr=IDX:HEX)
    int setrIdx[16], setfrIdx[16], nSetr = 0, nSetfr = 0; u32 setrVal[16], setfrVal[16];
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--no-isolate")) doIsolate = false;
        if (!std::strcmp(argv[i], "--drop-chars")) dropChars = true;
        if (!std::strcmp(argv[i], "--drop-scratch")) dropScratch = true;
        if (!std::strcmp(argv[i], "--min-ctx"))    minCtx = true;
        if (!std::strcmp(argv[i], "--leaf"))       leafMode = true;
        if (!std::strcmp(argv[i], "--trace"))      g_dotrace = true;
        if (!std::strncmp(argv[i], "--ctx-override=", 15)) ctxOverride = argv[i] + 15;
        { int ix; unsigned v;
          if (std::sscanf(argv[i], "--setr=%d:%x",  &ix, &v) == 2 && nSetr  < 16) { setrIdx[nSetr]=ix;   setrVal[nSetr++]=v; }
          if (std::sscanf(argv[i], "--setfr=%d:%x", &ix, &v) == 2 && nSetfr < 16) { setfrIdx[nSetfr]=ix; setfrVal[nSetfr++]=v; } }
    }

    FILE* f = std::fopen(seedPath, "rb");
    if (!f) { std::fprintf(stderr, "cannot open seed %s\n", seedPath); return 2; }

    char magic[8];
    u32 entryPC, spEntry, retPc, ctxSz, ramSz;
    if (std::fread(magic, 1, 8, f) != 8 ||
        (std::memcmp(magic, "RTSEED01", 8) != 0 && std::memcmp(magic, "RTSEED02", 8) != 0)) {
        std::fprintf(stderr, "bad magic\n"); return 2;
    }
    bool v2 = std::memcmp(magic, "RTSEED02", 8) == 0;
    std::fread(&entryPC, 4, 1, f); std::fread(&spEntry, 4, 1, f);
    std::fread(&retPc, 4, 1, f);   std::fread(&ctxSz, 4, 1, f);
    std::fread(&ramSz, 4, 1, f);
    if (v2) {   // RTSEED02: ccnSz + CCN[18] (seed QACR0/1 etc. — nothing stubbed)
        u32 ccnSz = 0; std::fread(&ccnSz, 4, 1, f);
        u32 want = (ccnSz <= sizeof(g_ccn)) ? ccnSz : (u32)sizeof(g_ccn);
        std::fread(g_ccn, 1, want, f);
        if (ccnSz > want) std::fseek(f, ccnSz - want, SEEK_CUR);
        g_haveCcn = true;
        std::printf("[seed] RTSEED02 CCN seeded (%u B): QACR0=0x%08X QACR1=0x%08X\n",
                    ccnSz, g_ccn[14], g_ccn[15]);
    }
    std::printf("[seed] entryPC=0x%08X spEntry=0x%08X retPc=0x%08X ctxSz=%u ramSz=%u\n",
                entryPC, spEntry, retPc, ctxSz, ramSz);
    if (ctxSz != sizeof(Sh4Context)) {
        std::fprintf(stderr, "ctxSz %u != sizeof(Sh4Context) %zu\n", ctxSz, sizeof(Sh4Context));
        return 2;
    }
    if (ramSz != RAM_BYTES) {
        std::fprintf(stderr, "ramSz %u != %u\n", ramSz, RAM_BYTES); return 2;
    }

    // allocate Sh4RCB (context lives at p_sh4rcb->cntx)
    p_sh4rcb = (Sh4RCB*)std::calloc(1, sizeof(Sh4RCB));
    if (!p_sh4rcb) { std::fprintf(stderr, "calloc Sh4RCB failed (%zu)\n", sizeof(Sh4RCB)); return 2; }

    // load entry Sh4Context into a temp buffer (Init() below memsets the live ctx,
    // so we restore it AFTER Init()).
    static Sh4Context seedCtx;
    if (std::fread(&seedCtx, 1, ctxSz, f) != ctxSz) { std::fprintf(stderr, "ctx read short\n"); return 2; }

    // allocate + load RAM
    g_ram    = (u8*)std::malloc(RAM_BYTES);
    g_zeroed = (u8*)std::calloc(RAM_BYTES, 1);
    g_wrote  = (u8*)std::calloc(RAM_BYTES, 1);
    if (!g_ram || !g_zeroed || !g_wrote) { std::fprintf(stderr, "ram alloc failed\n"); return 2; }
    if (std::fread(g_ram, 1, RAM_BYTES, f) != RAM_BYTES) { std::fprintf(stderr, "ram read short\n"); return 2; }
    std::fclose(f);
    mem_b.setRegion(g_ram, RAM_BYTES);

    // ISOLATE: zero every non-resident byte
    u64 zeroed = 0;
    if (doIsolate) {
        for (u32 off = 0; off < RAM_BYTES; off++) {
            u32 phys = RAM_LO + off;
            bool resident = residentByte(phys);
            // negative control: treat char structs + objpool as non-resident
            if (dropChars && phys >= 0x0C268000u && phys < 0x0C280000u) resident = false;
            if (!resident) { g_ram[off] = 0; g_zeroed[off] = 1; zeroed++; }
        }
    }
    std::printf("[isolate] %s — zeroed %llu / %u non-resident bytes\n",
                doIsolate ? "ON" : "OFF(control)", (unsigned long long)zeroed, RAM_BYTES);

    // ---- GATING TEST: --drop-scratch. ADDITIONALLY zero the render-scratch regions the
    // current wire ships but the driver is hypothesized to REBUILD within its own closure.
    // If any is read-before-write -> genuine cross-frame input (FALSIFY, reported per-region).
    if (dropScratch) {
        auto ramRd32 = [&](u32 guest) -> u32 {
            u32 off = (guest & 0x1FFFFFFFu) - RAM_LO;
            u32 v; std::memcpy(&v, &g_ram[off], 4); return v;
        };
        // Resolve the pointer-based idxtab/rectab arenas from the tab_ptr window (0x8C2DAD3C/4C),
        // exactly as maplecast_replica_live.cpp buildTables does.
        u32 idxtab = ramRd32(0x8C2DAD3Cu), rectab = ramRd32(0x8C2DAD4Cu);
        auto addScratch = [&](u32 guestLo, u32 len, const char* name) {
            u32 lo = guestLo & 0x1FFFFFFFu;
            g_scratch.push_back({ lo, lo + len, name, 0, 0, 0 });
        };
        addScratch(0x8C1F9000u, 0x3000u, "render-scratch(arena+tiledesc 0x1F9000-1FC000)");
        addScratch(idxtab,      0x2000u, "idxtab-arena");
        addScratch(rectab,      0x10000u, "rectab-arena");
        static const u32 EFX[7] = { 0x8C565000u, 0x8C955000u, 0x8C6B5000u,
                                    0x8CAA5000u, 0x8C805000u, 0x8CBF5000u, 0x8CD45000u };
        for (int i = 0; i < 7; i++) addScratch(EFX[i], 0x3000u, "efxtmpl");
        u64 zs = 0;
        for (auto& s : g_scratch)
            for (u32 phys = s.lo; phys < s.hi; phys++) {
                u32 off = phys - RAM_LO;
                if (off < RAM_BYTES && !g_zeroed[off]) { g_ram[off] = 0; g_zeroed[off] = 1; zs++; }
            }
        std::printf("[drop-scratch] ON — zeroed %llu scratch bytes; idxtab=0x%08X rectab=0x%08X; regions:\n",
                    (unsigned long long)zs, idxtab, rectab);
        for (auto& s : g_scratch)
            std::printf("  scratch 0x%08X..0x%08X  %s\n", s.lo | 0x80000000u, s.hi | 0x80000000u, s.name);
    }

    // wire memory handlers
    ReadMem8 = rd8; ReadMem16 = rd16; IReadMem16 = rd16; ReadMem32 = rd32; ReadMem64 = rd64;
    WriteMem8 = wr8; WriteMem16 = wr16; WriteMem32 = wr32; WriteMem64 = wr64;

    g_entryPC = entryPC; g_spEntry = spEntry; g_retPc = retPc & 0x1FFFFFFFu;

    // instantiate the REAL interpreter, THEN restore the seed context (Init memsets it)
    Sh4Executor* cpu = Get_Sh4Interpreter();
    cpu->Init();
    Sh4cntx = seedCtx;                 // restore entry CPU/FP register state
    if (ctxOverride) {                 // load the 512B entry ctx from a DIFFERENT seed
        FILE* cf = std::fopen(ctxOverride, "rb");
        if (cf) {
            char mg[8]; std::fread(mg,1,8,cf);
            bool cv2 = std::memcmp(mg,"RTSEED02",8)==0;
            std::fseek(cf, 8 + 20 + (cv2 ? (4 + 72) : 0), SEEK_SET);
            static Sh4Context ovCtx; std::fread(&ovCtx,1,sizeof(Sh4Context),cf);
            std::fclose(cf);
            Sh4cntx = ovCtx;
            std::printf("[run] --ctx-override: entry ctx loaded from %s\n", ctxOverride);
        }
    }
    if (minCtx) {                      // reg-invariance test: keep only r15/pc/pr/sr/fpscr
        for (int i = 0; i < 15; i++) Sh4cntx.r[i] = 0;
        for (int i = 0; i < 16; i++) { Sh4cntx.fr[i] = 0; Sh4cntx.xf[i] = 0; }
        Sh4cntx.mac.full = 0; Sh4cntx.fpul = 0;
        for (int i = 0; i < 8; i++) Sh4cntx.r_bank[i] = 0;
        std::printf("[run] --min-ctx: zeroed r0..r14, fr/xf, mac, fpul, r_bank (kept r15/pc/pr/sr/fpscr)\n");
    }
    // context fixups: force entry PC, override the (stale, cross-process) SQ pointer
    Sh4cntx.pc = entryPC;
    Sh4cntx.doSqWrite = sqCapture;
    Sh4cntx.CpuRunning = 0;
    if (leafMode) {
        // A self-contained game-tick leaf ends in `rts` (pc <- pr). Point pr at the
        // stop sentinel = retPc (header sets retPc, e.g. 0x8C000000 -> g_retPc=0), and
        // set r15 = spEntry so the onPc guard (r15 >= spEntry) matches on return but not
        // mid-function. entryPC must differ from retPc so entry doesn't stop early.
        Sh4cntx.pr = retPc;
        Sh4cntx.r[15] = spEntry;
        std::printf("[run] --leaf: pr=0x%08X r15=0x%08X (return sentinel retPc=0x%08X)\n",
                    Sh4cntx.pr, Sh4cntx.r[15], g_retPc);
    }
    // entry-register overrides (applied last so they win over min-ctx/leaf/seed)
    for (int k = 0; k < nSetr; k++)  { Sh4cntx.r[setrIdx[k] & 15] = setrVal[k];
        std::printf("[run] set r%d=0x%08X\n", setrIdx[k] & 15, setrVal[k]); }
    for (int k = 0; k < nSetfr; k++) { Sh4cntx.fr_hex(setfrIdx[k] & 15) = setfrVal[k];
        std::printf("[run] set fr%d=0x%08X\n", setfrIdx[k] & 15, setfrVal[k]); }

    std::printf("[run] entry pc=0x%08X sr=0x%08X fpscr=0x%08X r15=0x%08X pr=0x%08X\n",
                Sh4cntx.pc, Sh4cntx.sr.getFull(), Sh4cntx.fpscr.full, Sh4cntx.r[15], Sh4cntx.pr);
    std::printf("[run] starting real interpreter...\n");
    cpu->Start();          // CpuRunning = true
    cpu->Run();            // loops until debugger::Stop (retPc / watchdog / fault)

    // ---- verdict ----
    std::printf("\n===== STEP 3 RESULT =====\n");
    std::printf("instructions executed: %llu\n", (unsigned long long)g_icount);
    std::printf("last PC: 0x%08X\n", g_curPc);
    if (g_reached)
        std::printf("VERDICT: RAN TO COMPLETION — reached retPc 0x%08X on %s RAM\n",
                    g_retPc | 0x80000000u, doIsolate ? "RESIDENT-ONLY" : "FULL");
    else if (g_faulted)
        std::printf("VERDICT: FAULT — epc=0x%08X expEvn=0x%04X (see Sh4ExceptionCode)\n",
                    g_faultEpc, g_faultEvn);
    else if (g_watchdog)
        std::printf("VERDICT: WATCHDOG — %llu insns without reaching retPc (loop/hang)\n",
                    (unsigned long long)WATCHDOG);
    else
        std::printf("VERDICT: STOPPED (CpuRunning cleared unexpectedly)\n");

    std::printf("TA parcels captured: %zu (%zu bytes)\n", g_ta.size(), g_ta.size() * 32);
    std::printf("zeroed-resident-miss (read-before-write of zeroed byte): %zu (showing up to %zu)\n",
                g_miss.size(), MISS_CAP);
    for (auto& m : g_miss)
        std::printf("  MISS pc=0x%08X read addr=0x%08X\n", m.pc, m.addr);
    std::printf("non-RAM reads: %zu (showing up to %zu)\n", g_other.size(), OTHER_CAP);
    for (auto& o : g_other)
        std::printf("  OTHER pc=0x%08X addr=0x%08X sz=%u\n", o.pc, o.addr, o.sz);

    // ---- GATING TEST per-region verdict ----
    if (!g_scratch.empty()) {
        std::printf("\n===== DROP-SCRATCH PER-REGION VERDICT =====\n");
        u64 tot = 0;
        for (auto& s : g_scratch) {
            tot += s.miss;
            if (s.miss == 0)
                std::printf("  REBUILT   0x%08X..0x%08X  %-42s  0 miss\n",
                            s.lo | 0x80000000u, s.hi | 0x80000000u, s.name);
            else
                std::printf("  MUST-STAY 0x%08X..0x%08X  %-42s  %llu miss (first pc=0x%08X addr=0x%08X)\n",
                            s.lo | 0x80000000u, s.hi | 0x80000000u, s.name,
                            (unsigned long long)s.miss, s.firstPc, s.firstAddr);
        }
        std::printf("  TOTAL scratch read-before-write misses: %llu\n", (unsigned long long)tot);
    }

    // dump TA
    if (!g_ta.empty()) {
        FILE* tf = std::fopen("ta_out.bin", "wb");
        if (tf) {
            for (auto& c : g_ta) std::fwrite(c.data, 1, 32, tf);
            std::fclose(tf);
            std::printf("wrote ta_out.bin (%zu bytes)\n", g_ta.size() * 32);
        }
    }

    // ---- TICKTRACE: dump the executed-PC trace (function set + resolved indirects) ----
    if (g_dotrace && !g_pctrace.empty()) {
        FILE* tf = std::fopen("trace_out.bin", "wb");
        if (tf) { std::fwrite(g_pctrace.data(), 4, g_pctrace.size(), tf); std::fclose(tf); }
        std::printf("wrote trace_out.bin (%zu PCs)\n", g_pctrace.size());
    }

    // ---- GAME-TICK LEAF ORACLE: dump final ctx + full RAM = flycast ground truth ----
    // The transpiled executor's write-set / returned r0 is diffed against these.
    if (g_reached) {
        FILE* cf2 = std::fopen("oracle_ctx.txt", "w");
        if (cf2) {
            for (int i = 0; i < 16; i++) std::fprintf(cf2, "r%d=0x%08X\n", i, Sh4cntx.r[i]);
            for (int i = 0; i < 16; i++) std::fprintf(cf2, "fr%d=0x%08X\n", i, Sh4cntx.fr_hex(i));
            std::fprintf(cf2, "macl=0x%08X mach=0x%08X pr=0x%08X fpul=0x%08X\n",
                         Sh4cntx.mac.l, Sh4cntx.mac.h, Sh4cntx.pr, Sh4cntx.fpul);
            std::fclose(cf2);
            std::printf("wrote oracle_ctx.txt (final r0=0x%08X)\n", Sh4cntx.r[0]);
        }
        FILE* mf2 = std::fopen("oracle_ram_out.bin", "wb");
        if (mf2) {
            std::fwrite(g_ram, 1, RAM_BYTES, mf2);
            std::fclose(mf2);
            std::printf("wrote oracle_ram_out.bin (%u bytes)\n", RAM_BYTES);
        }
    }
    return g_reached ? 0 : (g_faulted ? 3 : (g_watchdog ? 4 : 5));
}
