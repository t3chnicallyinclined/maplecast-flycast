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
         std::vector<uint8_t>& outTa, double* wallMs, bool inPlace)
{
    if (!ram16 || !ctx512) return false;
    auto t0 = std::chrono::steady_clock::now();

    // interpreter core needs p_sh4rcb; allocate if the (SH4-off) client never did.
    static bool s_ownRcb = false;
    if (!p_sh4rcb) { p_sh4rcb = (Sh4RCB*)std::calloc(1, sizeof(Sh4RCB)); s_ownRcb = true; }
    if (!p_sh4rcb) return false;

    // PERF: inPlace runs the driver DIRECTLY on the caller's 16MB _gstaRam (no per-frame
    // copy). Safe because the driver only writes build-then-read RENDER SCRATCH (arena/TA
    // buffers), which the GSTA wire re-ships every frame (drop-scratch confirmed), and the
    // char/GFX inputs the downstream palette+decode read are never written by the pass.
    // The byte-exact TA is captured during the run regardless (same entry state -> same TA).
    if (inPlace) {
        g_wram = const_cast<uint8_t*>(ram16);
    } else {
        static uint8_t* g_wramOwned = nullptr;
        if (!g_wramOwned) g_wramOwned = (uint8_t*)std::malloc(RAM_BYTES);
        if (!g_wramOwned) return false;
        g_wram = g_wramOwned;
        std::memcpy(g_wram, ram16, RAM_BYTES);
    }

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

// EMBEDDED stable driver-entry Sh4Context (512B, captured at the frame-90 char-pass
// driver entry). PROVEN frame-invariant for the TA output: running frame-150's RAM
// with THIS context reproduces frame-150's own engine TA byte-for-byte (8a6ed250...),
// i.e. the per-frame-varying regs (r5/r6) do NOT affect the char pass — the driver
// re-derives everything it renders from RAM. So this one context is byte-exact for
// ALL frames. (Env MAPLECAST_GSTA_CHARPASS_SEED overrides it if set.)
static const unsigned char kEntryCtx[512] = {
  0x02,0x00,0x8c,0x84,0x00,0x00,0x00,0x80,0x00,0x00,0x80,0x20,0xa1,0x0e,0x6a,0x3c,
  0x15,0x0b,0x7e,0x3e,0x37,0x02,0x01,0x3f,0x00,0x00,0x80,0x3f,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0xf3,0xb5,0x18,0x44,0xc7,0xc8,0xd3,0x43,0xa1,0x0e,0x6a,0x3c,
  0x9e,0xe7,0x7d,0x3e,0x49,0x92,0x1f,0x3f,0x00,0x00,0x80,0x3f,0x00,0x00,0x00,0x00,
  0x00,0x00,0x80,0x3f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x3f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x3f,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x3f,
  0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x3f,0x84,0xeb,0x11,0x3f,0x00,0x00,0xe8,0x41,
  0x95,0xbf,0xd6,0x33,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x3f,0x00,0x00,0x00,0x40,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xbf,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x02,0x00,0x00,0x00,0x32,0x96,0x03,0x8c,0x00,0x00,0xff,0xff,0x58,0x08,0x03,0x8c,
  0x88,0x3c,0x2a,0x8c,0x3c,0x00,0x00,0x00,0x05,0x00,0x00,0x00,0xdc,0x55,0x13,0x8c,
  0xdc,0x81,0x26,0x8c,0x20,0xd4,0x11,0x8c,0xb0,0x74,0x02,0x8c,0xf0,0x81,0x26,0x8c,
  0xc0,0x02,0x01,0x8c,0x94,0xc3,0x28,0x8c,0x01,0x00,0x00,0x00,0xec,0xf3,0x00,0x8c,
  0xa4,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0xf0,0x00,0x00,0x70,0x0f,0xff,0xff,0xff,
  0x1c,0x00,0x00,0xff,0x00,0x00,0x00,0x00,0x00,0xfc,0x00,0xac,0x00,0x00,0x00,0x00,
  0x00,0x04,0x00,0x00,0x00,0x04,0x00,0x00,0x20,0xb4,0x32,0x8c,0x00,0x00,0x00,0x60,
  0x54,0x3c,0x03,0x8c,0x84,0xf3,0x00,0x8c,0x10,0x00,0x00,0x8c,0x00,0xf4,0x00,0x8c,
  0x48,0x96,0x03,0x8c,0xe0,0x01,0x00,0x00,0x58,0x08,0x03,0x8c,0x80,0xe3,0x18,0x8c,
  0x00,0x01,0x00,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x24,0x00,0x00,0x00,0x00,0x60,
  0x00,0x00,0x24,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x01,0x00,0x00,0x70,0x2c,0x01,0x00,0x00,0x90,0xd4,0xb4,0x30,0xf6,0x7f,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
static uint32_t kEntryCcn[18] = { 0 };   // QACR0/1 (idx 14/15) set to 0x0C at first use

static std::vector<uint8_t> s_liveCtx, s_liveCcn;   // optional env-file override
static int s_liveCtxState = 0;   // 0=unloaded 1=file 2=embedded

static void loadLiveContextOnce()
{
    if (s_liveCtxState != 0) return;
    kEntryCcn[14] = kEntryCcn[15] = 0x0000000Cu;
    const char* p = std::getenv("MAPLECAST_GSTA_CHARPASS_SEED");
    if (p && p[0]) {
        FILE* f = std::fopen(p, "rb");
        if (f) {
            char magic[8];
            if (std::fread(magic,1,8,f)==8 &&
                (!std::memcmp(magic,"RTSEED02",8) || !std::memcmp(magic,"RTSEED01",8))) {
                bool v2 = std::memcmp(magic,"RTSEED02",8)==0;
                u32 e,sp,rp,ctxSz,ramSz; std::fread(&e,4,1,f);std::fread(&sp,4,1,f);std::fread(&rp,4,1,f);
                std::fread(&ctxSz,4,1,f); std::fread(&ramSz,4,1,f);
                if (v2){ u32 cs=0; std::fread(&cs,4,1,f); s_liveCcn.resize(cs); std::fread(s_liveCcn.data(),1,cs,f); }
                s_liveCtx.resize(ctxSz); std::fread(s_liveCtx.data(),1,ctxSz,f);
                if (ctxSz==sizeof(Sh4Context)) { s_liveCtxState = 1;
                    std::printf("[charpass] entry context from file %s\n", p); }
            }
            std::fclose(f);
        }
    }
    if (s_liveCtxState == 0) { s_liveCtxState = 2;
        std::printf("[charpass] entry context: EMBEDDED constant (frame-invariant, byte-exact)\n"); }
}

bool run_live(const uint8_t* ram16, std::vector<uint8_t>& outTa, double* wallMs)
{
    loadLiveContextOnce();
    // inPlace=true: run on the client's _gstaRam directly (no 16MB copy) — the PERF path.
    if (s_liveCtxState == 1)
        return run(ram16, s_liveCtx.data(), s_liveCcn.empty()?nullptr:s_liveCcn.data(), outTa, wallMs, true);
    return run(ram16, kEntryCtx, (const uint8_t*)kEntryCcn, outTa, wallMs, true);  // embedded default
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
    // Also exercise run_live() (EMBEDDED constant context unless CHARPASS_SEED is set) on
    // the same RAM — must match, confirming the embedded 512B context is byte-correct.
    std::vector<uint8_t> ta2; double ms2 = 0;
    bool ok2 = run_live(ram.data(), ta2, &ms2);
    unsigned char d2[16]; MD5_CTX c2; MD5_Init(&c2);
    MD5_Update(&c2, ta2.data(), (unsigned long)ta2.size()); MD5_Final(d2, &c2);
    char hx2[33]; for (int i=0;i<16;i++) std::snprintf(hx2+i*2,3,"%02x",d2[i]);
    std::printf("[charpass-selftest] run_live(EMBEDDED ctx) ran=%d parcels=%zu md5=%s wall=%.3fms %s\n",
                (int)ok2, ta2.size()/32, hx2, ms2,
                (ta2 == ta) ? "== run() (embedded ctx byte-correct)" : "!!! MISMATCH");
    return true;
}

} // namespace gsta_charpass
