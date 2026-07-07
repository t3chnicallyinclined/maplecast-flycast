// gsta_charpass.cpp — see gsta_charpass.h. Runs the REAL MVC2 char-pass render
// driver (loc_8c030858 -> retPc 0x8C039648) in-process via flycast's SH4 interpreter
// core, on a working copy of a 16MB RAM image, capturing the store-queue-emitted TA.
//
// Isolation: the interpreter reads/writes go through the global ReadMem*/WriteMem*/
// IReadMem16 function pointers and Sh4cntx.doSqWrite; we SWAP those to our working-
// buffer + SQ-capture handlers for the duration of the run, then RESTORE. SH4 is OFF
// in the GSTA client, so the global Sh4cntx / handlers are otherwise idle (no
// contention). The Run loop stops at RET_PC via the gated gsta_charpass_onpc hook.
#include "gsta_charpass.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mem.h"
#include "debug/gdb_server.h"
#include "md5/md5.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

// ---------------------------------------------------------------------------
// Run-loop hook state (file-scope; single-threaded use within run()).
// ---------------------------------------------------------------------------
bool gsta_charpass_active = false;

namespace {
static uint8_t*  g_wram   = nullptr;                 // 16MB working buffer
static const uint32_t RAM_BYTES = 16u * 1024 * 1024;
static const uint32_t RAM_MASK16 = RAM_BYTES - 1;
static uint32_t  g_ccn[18] = { 0 };
static bool      g_haveCcn = false;
static std::vector<uint8_t>* g_out = nullptr;

static uint32_t  g_spEntry = gsta_charpass::SP_ENTRY;
static uint32_t  g_retPc   = gsta_charpass::RET_PC & 0x1FFFFFFFu;
static uint64_t  g_icount  = 0;
static const uint64_t WATCHDOG = 400ull * 1000 * 1000;
static bool      g_reached = false, g_watchdog = false;

// saved globals (restored after the run)
static ReadMem8Func  sv_R8;  static ReadMem16Func sv_R16, sv_IR16;
static ReadMem32Func sv_R32; static ReadMem64Func sv_R64;
static WriteMem8Func sv_W8;  static WriteMem16Func sv_W16;
static WriteMem32Func sv_W32; static WriteMem64Func sv_W64;

template<typename T> static inline T rdT(u32 addr) {
    u32 phys = addr & 0x1FFFFFFFu;
    if (phys >= 0x0C000000u && phys < 0x10000000u) {
        T v; std::memcpy(&v, &g_wram[(phys - 0x0C000000u) & RAM_MASK16], sizeof(T)); return v;
    }
    // on-chip CCN regs (P4 0x1F0000xx incl QACR0/1 @ 0x38/0x3C) — seeded, else 0
    if (g_haveCcn && phys >= 0x1F000000u && phys < 0x1F000100u) {
        u32 idx = (phys & 0xFFu) >> 2; u32 reg = (idx < 18) ? g_ccn[idx] : 0;
        return (T)(reg >> ((phys & 3u) * 8));
    }
    return (T)0;
}
template<typename T> static inline void wrT(u32 addr, T v) {
    if ((addr >> 26) == 0x38u) {   // store queue -> Sh4Context.sq_buffer
        u8* sq = (u8*)&Sh4cntx.sq_buffer[(addr >> 5) & 1];
        std::memcpy(sq + (addr & 0x1Fu), &v, sizeof(T));
        return;
    }
    u32 phys = addr & 0x1FFFFFFFu;
    if (phys >= 0x0C000000u && phys < 0x10000000u)
        std::memcpy(&g_wram[(phys - 0x0C000000u) & RAM_MASK16], &v, sizeof(T));
    // else: PVR regs / VRAM / other — driver's char pass never needs these written.
}

static u8  DYNACALL h_r8 (u32 a){ return rdT<u8 >(a); }
static u16 DYNACALL h_r16(u32 a){ return rdT<u16>(a); }
static u32 DYNACALL h_r32(u32 a){ return rdT<u32>(a); }
static u64 DYNACALL h_r64(u32 a){ return rdT<u64>(a); }
static void DYNACALL h_w8 (u32 a,u8  v){ wrT<u8 >(a,v); }
static void DYNACALL h_w16(u32 a,u16 v){ wrT<u16>(a,v); }
static void DYNACALL h_w32(u32 a,u32 v){ wrT<u32>(a,v); }
static void DYNACALL h_w64(u32 a,u64 v){ wrT<u64>(a,v); }

// store-queue flush (pref @Rn into SQ area) — the TA emission path
static void DYNACALL h_sqCapture(u32 dest, Sh4Context* ctx) {
    const u8* sq = ctx->sq_buffer[(dest >> 5) & 1].data;
    if (g_out) g_out->insert(g_out->end(), sq, sq + 32);
}
} // anon namespace

// gated per-instruction hook (sh4_interpreter.cpp Run loop). Stops at RET_PC.
void gsta_charpass_onpc(u32 pc) {
    if (++g_icount > WATCHDOG) { g_watchdog = true; throw debugger::Stop(); }
    if ((pc & 0x1FFFFFFFu) == g_retPc && Sh4cntx.r[15] >= g_spEntry) {
        g_reached = true; throw debugger::Stop();
    }
}

namespace gsta_charpass {

bool run(const uint8_t* ram16, const uint8_t* ctx512, const uint8_t* ccn72,
         std::vector<uint8_t>& outTa, double* wallMs)
{
    if (!ram16 || !ctx512) return false;
    auto t0 = std::chrono::steady_clock::now();

    // interpreter core needs p_sh4rcb; allocate if the (SH4-off) client never did.
    static bool s_ownRcb = false;
    if (!p_sh4rcb) { p_sh4rcb = (Sh4RCB*)std::calloc(1, sizeof(Sh4RCB)); s_ownRcb = true; }
    if (!p_sh4rcb) return false;

    // working RAM copy (driver writes build-then-read scratch; keep caller's image clean)
    if (!g_wram) g_wram = (uint8_t*)std::malloc(RAM_BYTES);
    if (!g_wram) return false;
    std::memcpy(g_wram, ram16, RAM_BYTES);

    g_haveCcn = false;
    if (ccn72) { std::memcpy(g_ccn, ccn72, sizeof(g_ccn)); g_haveCcn = true; }

    outTa.clear();
    g_out = &outTa;
    g_icount = 0; g_reached = false; g_watchdog = false;
    g_spEntry = SP_ENTRY; g_retPc = RET_PC & 0x1FFFFFFFu;

    // save + swap the global memory handlers
    sv_R8=ReadMem8; sv_R16=ReadMem16; sv_IR16=IReadMem16; sv_R32=ReadMem32; sv_R64=ReadMem64;
    sv_W8=WriteMem8; sv_W16=WriteMem16; sv_W32=WriteMem32; sv_W64=WriteMem64;
    ReadMem8=h_r8; ReadMem16=h_r16; IReadMem16=h_r16; ReadMem32=h_r32; ReadMem64=h_r64;
    WriteMem8=h_w8; WriteMem16=h_w16; WriteMem32=h_w32; WriteMem64=h_w64;

    // save the live Sh4 context, then seed the driver-entry context
    static Sh4Context savedCtx;
    savedCtx = Sh4cntx;

    static Sh4Executor* s_cpu = nullptr;
    if (!s_cpu) { s_cpu = Get_Sh4Interpreter(); s_cpu->Init(); }

    std::memcpy(&Sh4cntx, ctx512, sizeof(Sh4Context));
    Sh4cntx.pc = ENTRY_PC;
    Sh4cntx.doSqWrite = h_sqCapture;
    Sh4cntx.CpuRunning = 0;

    gsta_charpass_active = true;
    s_cpu->Start();
    s_cpu->Run();               // stops at RET_PC / watchdog via gsta_charpass_onpc
    gsta_charpass_active = false;

    // restore globals
    Sh4cntx = savedCtx;
    ReadMem8=sv_R8; ReadMem16=sv_R16; IReadMem16=sv_IR16; ReadMem32=sv_R32; ReadMem64=sv_R64;
    WriteMem8=sv_W8; WriteMem16=sv_W16; WriteMem32=sv_W32; WriteMem64=sv_W64;
    g_out = nullptr;

    if (wallMs) *wallMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    return g_reached;
}

// Lazy-loaded stable driver-entry context (from an RTSEED02 seed). The entry register
// file is stable across frames (the driver reads its inputs from RAM); shipping it in
// the GSTA prefix is the productionization step — for now it comes from a seed file.
static std::vector<uint8_t> s_liveCtx, s_liveCcn;
static int s_liveCtxState = 0;   // 0=unloaded 1=ready -1=unavailable

static void loadLiveContextOnce()
{
    if (s_liveCtxState != 0) return;
    const char* p = std::getenv("MAPLECAST_GSTA_CHARPASS_SEED");
    if (!p || !p[0]) { s_liveCtxState = -1; return; }
    FILE* f = std::fopen(p, "rb");
    if (!f) { s_liveCtxState = -1; return; }
    char magic[8];
    if (std::fread(magic,1,8,f) != 8 ||
        (std::memcmp(magic,"RTSEED02",8) && std::memcmp(magic,"RTSEED01",8))) {
        std::fclose(f); s_liveCtxState = -1; return;
    }
    bool v2 = std::memcmp(magic,"RTSEED02",8) == 0;
    u32 e,sp,rp,ctxSz,ramSz; std::fread(&e,4,1,f);std::fread(&sp,4,1,f);std::fread(&rp,4,1,f);
    std::fread(&ctxSz,4,1,f); std::fread(&ramSz,4,1,f);
    if (v2) { u32 cs=0; std::fread(&cs,4,1,f); s_liveCcn.resize(cs); std::fread(s_liveCcn.data(),1,cs,f); }
    s_liveCtx.resize(ctxSz); std::fread(s_liveCtx.data(),1,ctxSz,f);
    std::fclose(f);
    s_liveCtxState = (ctxSz == sizeof(Sh4Context)) ? 1 : -1;
    std::printf("[charpass] live entry context %s (ctx=%u ccn=%zu) from %s\n",
                s_liveCtxState==1?"loaded":"INVALID", ctxSz, s_liveCcn.size(), p);
}

bool run_live(const uint8_t* ram16, std::vector<uint8_t>& outTa, double* wallMs)
{
    loadLiveContextOnce();
    if (s_liveCtxState != 1) return false;
    return run(ram16, s_liveCtx.data(),
               s_liveCcn.empty() ? nullptr : s_liveCcn.data(), outTa, wallMs);
}

bool selftest_from_env()
{
    const char* p = std::getenv("MAPLECAST_CHARPASS_SELFTEST");
    if (!p || !p[0]) return false;
    FILE* f = std::fopen(p, "rb");
    if (!f) { std::printf("[charpass-selftest] cannot open %s\n", p); return true; }
    char magic[8];
    if (std::fread(magic, 1, 8, f) != 8 ||
        (std::memcmp(magic, "RTSEED02", 8) != 0 && std::memcmp(magic, "RTSEED01", 8) != 0)) {
        std::printf("[charpass-selftest] bad magic\n"); std::fclose(f); return true;
    }
    bool v2 = std::memcmp(magic, "RTSEED02", 8) == 0;
    u32 entryPC, spEntry, retPc, ctxSz, ramSz;
    std::fread(&entryPC,4,1,f); std::fread(&spEntry,4,1,f); std::fread(&retPc,4,1,f);
    std::fread(&ctxSz,4,1,f);   std::fread(&ramSz,4,1,f);
    std::vector<uint8_t> ccn;
    if (v2) { u32 ccnSz=0; std::fread(&ccnSz,4,1,f); ccn.resize(ccnSz); std::fread(ccn.data(),1,ccnSz,f); }
    std::vector<uint8_t> ctx(ctxSz); std::fread(ctx.data(),1,ctxSz,f);
    std::vector<uint8_t> ram(ramSz); std::fread(ram.data(),1,ramSz,f);
    std::fclose(f);

    std::vector<uint8_t> ta; double ms = 0;
    bool ok = run(ram.data(), ctx.data(), v2 && !ccn.empty() ? ccn.data() : nullptr, ta, &ms);

    unsigned char dig[16]; MD5_CTX c; MD5_Init(&c);
    MD5_Update(&c, ta.data(), (unsigned long)ta.size()); MD5_Final(dig, &c);
    char hex[33]; for (int i=0;i<16;i++) std::snprintf(hex+i*2,3,"%02x",dig[i]);
    std::printf("[charpass-selftest] ran=%d reached=%d parcels=%zu bytes=%zu md5=%s wall=%.3fms\n",
                (int)ok, (int)ok, ta.size()/32, ta.size(), hex, ms);
    std::printf("[charpass-selftest] EXPECT md5=be1377d28b3d4bf624c18590dae21ce5 (byte gate)\n");
    if (FILE* tf = std::fopen("charpass_ta.bin","wb")) {
        std::fwrite(ta.data(),1,ta.size(),tf); std::fclose(tf);
    }
    return true;
}

} // namespace gsta_charpass
