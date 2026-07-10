/* texel_gate.cpp — OFFLINE BYTE-GATE for the native GSTA body-texture staging.
 *
 * Reproduces gstaDecodeBodies (core/network/maplecast_mirror.cpp) OUTSIDE the client:
 *   reconstructed 16MB RAM + BTCW words -> render_frame (the SAME PoC sources the native
 *   client amalgamates via gsta_render_frame.c) -> parity-pin tcw mutation -> the decode
 *   chain (LZSS/detwiddle/carve/retwiddle, copied VERBATIM from maplecast_mirror.cpp,
 *   marked [COPY]) -> a staged 512KB VRAM band [0x400000,0x480000).
 *
 * Then:
 *   (1) CERTIFY: byte-compare the staged band vs the LIVE client dump (gsta_vram_<vf>.bin)
 *       at every staged tile — proves this replica == the shipping native code.
 *   (2) GATE: per scene quad, detwiddle the staged tile + the engine mirror tile
 *       (_live3_eng_vram_<mi>.bin, same addr AND parity twin +/-0x30000) and compare ONLY
 *       the sampled mW x mH window (engine bytes outside the UV window are stale garbage
 *       by design). Classes: EXACT / WRONG / ZERO(client zero, engine nonzero) / BOTHZERO.
 *   (3) DIAG: per (gfx1,sel): W,H (span), cols/rows/m grid, LZSS destLen vs PRODUCED bytes
 *       vs src length — pinpoints undershoot vs carve error.
 *
 * Build (before):  cl /O2 ... texel_gate.cpp render_frame.c gen_*.c
 * Build (after):   add /DTEXFIX  (the candidate fix, mirrored into maplecast_mirror.cpp)
 * Usage: texel_gate.exe <ram16.bin> <btcw.bin> <eng_band.bin> [cli_band.bin]
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <algorithm>
#include <string>
extern "C" {
#include "sh4ctx.h"
typedef struct {
    u32 pcw, isp, tsp, tcw, recidx;
    float Ax,Ay,Bx,By,Cx,Cy,Dx,Dy, u1, v1;
    u32 sel; u32 gfx1; u32 mirror; u32 facing; float z;
} SceneQuad;
void render_frame(Sh4Ctx *c);
int  render_frame_nscene(void);
const SceneQuad* render_frame_scene(void);
u32  render_frame_quad_colrow_impl(int* out_cr, u32 cap);
u32  render_frame_quad_is_effect_impl(unsigned char* out_e, u32 cap);
u32  render_frame_quad_srcdesc_impl(unsigned char* out, u32 cap);
void render_frame_set_body_tcws(const u32* buf, int nWords);
}

/* ======================= [COPY] decode chain from maplecast_mirror.cpp ======================= */
static int gsta_twiddleSlow(int x, int y, int xs, int ys) {
    int rv = 0, sh = 0; xs >>= 1; ys >>= 1;
    while (xs || ys) {
        if (ys) { rv |= (y & 1) << sh; ys >>= 1; y >>= 1; sh++; }
        if (xs) { rv |= (x & 1) << sh; xs >>= 1; x >>= 1; sh++; }
    }
    return rv;
}
static int gsta_DETW[2][11][1024];
static const int gsta_PAL4_ORDER[16][2] = {
    {0,0},{0,1},{1,0},{1,1},{0,2},{0,3},{1,2},{1,3},{2,0},{2,1},{3,0},{3,1},{2,2},{2,3},{3,2},{3,3}
};
static bool gsta_twInit = false;
static void gstaTwInit() {
    if (gsta_twInit) return;
    for (int s = 0; s < 11; s++) {
        int ys = 1 << s;
        for (int i = 0; i < 1024; i++) {
            gsta_DETW[0][s][i] = gsta_twiddleSlow(i, 0, 1024, ys);
            gsta_DETW[1][s][i] = gsta_twiddleSlow(0, i, ys, 1024);
        }
    }
    gsta_twInit = true;
}
static int gsta_log2i(int v) { int n = -1; while (v) { v >>= 1; n++; } return n; }
static void gstaDetwiddlePal4(const uint8_t* data, size_t dataLen, int w, int h, std::vector<uint8_t>& idx) {
    int bcx = gsta_log2i(w), bcy = gsta_log2i(h);
    idx.assign((size_t)w * h, 0);
    for (int y = 0; y < h; y += 4) for (int x = 0; x < w; x += 4) {
        int blk = (gsta_DETW[0][bcy][x] + gsta_DETW[1][bcx][y]) / 16;
        int base = blk * 8;
        for (int i = 0; i < 16; i++) {
            int cx = gsta_PAL4_ORDER[i][0], cy = gsta_PAL4_ORDER[i][1];
            uint8_t b = ((size_t)(base + (i >> 1)) < dataLen) ? data[base + (i >> 1)] : 0;
            idx[(size_t)(y + cy) * w + (x + cx)] = (i & 1) ? ((b >> 4) & 0xF) : (b & 0xF);
        }
    }
}
static void gstaRetwiddle32(const uint8_t* lin, uint8_t* out512) {
    memset(out512, 0, 512);
    for (int y = 0; y < 32; y += 4) for (int x = 0; x < 32; x += 4) {
        int blk = (gsta_DETW[0][5][x] + gsta_DETW[1][5][y]) / 16;
        int base = blk * 8;
        for (int i = 0; i < 16; i++) {
            int cx = gsta_PAL4_ORDER[i][0], cy = gsta_PAL4_ORDER[i][1];
            uint8_t nib = lin[(size_t)(y + cy) * 32 + (x + cx)] & 0xF;
            if (i & 1) out512[base + (i >> 1)] |= nib << 4; else out512[base + (i >> 1)] |= nib;
        }
    }
}
/* gstaDecodeA + PRODUCED-bytes diagnostic out (harness-only addition; algorithm verbatim). */
static void gstaDecodeA(const uint8_t* src, size_t sp, size_t srcEnd, size_t destLen,
                        std::vector<uint8_t>& out, size_t* producedOut) {
    out.assign(destLen, 0);
    size_t o = 0; uint32_t bc = 0; uint32_t flags = 0;
    while (o < destLen && sp < srcEnd) {
        if (bc == 0) { flags = src[sp++]; bc = 0x80; if (sp >= srcEnd) break; }
        if ((flags & bc) == 0) { out[o++] = src[sp++]; }
        else {
            uint8_t b = src[sp++];
            long s = (long)o - (b >> 4) - 1;
            int cnt = (b & 0x0F) + 2;
            for (int k = 0; k < cnt && o < destLen; k++, s++)
                out[o++] = (s >= 0 && (size_t)s < o) ? out[s] : 0;
        }
        bc >>= 1;
    }
    if (producedOut) *producedOut = o;
}
static inline uint32_t gramU32(const uint8_t* r, uint32_t a){ a &= 0x00FFFFFF; return (uint32_t)r[a]|((uint32_t)r[a+1]<<8)|((uint32_t)r[a+2]<<16)|((uint32_t)r[a+3]<<24); }
static inline uint8_t  gramU8 (const uint8_t* r, uint32_t a){ return r[a & 0x00FFFFFF]; }
struct GstaGfx { uint32_t n; std::vector<uint32_t> offs, srt; };
static std::unordered_map<uint32_t, GstaGfx> _gstaGfxCache;
struct GstaDecodedPart { std::vector<uint8_t> lin, raw; int W=0, H=0; size_t destLen=0, produced=0; uint32_t srcStart=0, srcEnd=0; bool ok=false; };
static int gstaTwTileYFirst(int col, int row, int Tw, int Th) {
    int rv = 0, sh = 0, xs = Tw >> 1, ys = Th >> 1, x = col, y = row;
    while (xs || ys) {
        if (ys) { rv |= (y & 1) << sh; ys >>= 1; y >>= 1; sh++; }
        if (xs) { rv |= (x & 1) << sh; xs >>= 1; x >>= 1; sh++; }
    }
    return rv;
}
static std::map<uint64_t, GstaDecodedPart> _gstaPartCache;
static GstaGfx& gstaGfx1Offsets(const uint8_t* ram, uint32_t gfx1) {
    auto it = _gstaGfxCache.find(gfx1);
    if (it != _gstaGfxCache.end()) return it->second;
    GstaGfx g; g.n = gramU32(ram, gfx1) >> 2;
    if (g.n > 0x40000) g.n = 0;
    g.offs.resize(g.n);
    for (uint32_t i = 0; i < g.n; i++) g.offs[i] = gramU32(ram, gfx1 + i * 4);
    std::set<uint32_t> uniq(g.offs.begin(), g.offs.end());
    g.srt.assign(uniq.begin(), uniq.end());
    return _gstaGfxCache.emplace(gfx1, std::move(g)).first->second;
}
static uint32_t gstaEndOf(const std::vector<uint32_t>& srt, uint32_t off) {
    auto it = std::upper_bound(srt.begin(), srt.end(), off);
    return it != srt.end() ? *it : off + 0x4000;
}
/* ============================= end [COPY] ============================= */

static const size_t BAND = 0x400000, BANDEND = 0x480000;

struct TileWrite { uint32_t vaddr; uint8_t bytes[512]; };

/* the gstaDecodeBodies loop, verbatim semantics (minus effect-poly guard I/O + dump hooks).
 * isEff: per-quad bit15 flag — the TEXFIX grid applies to NON-effect quads only (bit15
 * scale-walker staging keeps the legacy path bit-for-bit). */
static int decodeBodies(const uint8_t* ram, const SceneQuad* S, int nQuad,
                        const std::vector<int>& colrow, const std::vector<uint8_t>& isEff,
                        const std::vector<uint8_t>& srcDesc,
                        std::vector<TileWrite>& outTiles)
{
    gstaTwInit();
    struct RunExt { int mc, mr; };
    std::unordered_map<uint64_t, RunExt> runExt;
    for (int q = 0; q < nQuad; q++) {
        uint32_t g = S[q].gfx1;
        if (!(g & 0x0C000000u) && !(g & 0x8C000000u)) continue;
        uint64_t k = ((uint64_t)g << 32) | S[q].sel;
        int c = colrow[2 * q], r = colrow[2 * q + 1];
        auto it = runExt.find(k);
        if (it == runExt.end()) runExt[k] = { c, r };
        else { if (c > it->second.mc) it->second.mc = c; if (r > it->second.mr) it->second.mr = r; }
    }
    int written = 0;
    std::vector<uint8_t> raw, tileLin(1024), tile512(512);
    for (int q = 0; q < nQuad; q++) {
        uint32_t gfx1 = S[q].gfx1;
        if (!(gfx1 & 0x0C000000u) && !(gfx1 & 0x8C000000u)) continue;
        if (gfx1 >= 0x0CED0000u && gfx1 < 0x0CEE0000u) continue;
        /* BIT15 EFFECT QUADS ARE RESIDENT-BACKED — NEVER STAGE (2026-07-05 _live4 byte-gate).
         * Their textures are ENGINE-UPLOADED (rotating effect slots 0x475xxx/0x60xxxx/0x400xxx,
         * shipped in the GSTA prefix VRAM byte-exact — MEASURED 512/512 vs engine at every
         * sampled slot). Their sels are NOT GFX1 indices (0xC000-class sentinels) — decoding
         * them produced garbage tiles that OVERWROTE the good resident texels (the Z48 at
         * m1345 once the cull revision let them reach the decode). Pre-revision they never
         * got here (culled), so this skip restores the validated staging set exactly. */
        if (isEff[q]) continue;
        uint32_t sel = S[q].sel;
        uint32_t vaddr = (S[q].tcw & 0x1FFFFF) << 3;
        if ((size_t)vaddr + 512 > 8u*1024*1024) continue;

        uint64_t key = ((uint64_t)gfx1 << 32) | sel;
        auto pit = _gstaPartCache.find(key);
        if (pit == _gstaPartCache.end()) {
            GstaDecodedPart pd;
            GstaGfx& G = gstaGfx1Offsets(ram, gfx1);
            if (sel < G.n) {
                uint32_t pbase = gfx1 + G.offs[sel];
                int sw = gramU8(ram, pbase + 2), sh = gramU8(ram, pbase + 3);
                int W = sw * 8, H = sh * 8;
                if (W > 0 && H > 0 && W <= 1024 && H <= 1024) {
                    size_t destLen = (size_t)(W * H) >> 1;
                    uint32_t srcStart = (pbase + 4) & 0x00FFFFFF;
                    uint32_t srcEnd   = (gfx1 + gstaEndOf(G.srt, G.offs[sel])) & 0x00FFFFFF;
                    gstaDecodeA(ram, srcStart, srcEnd, destLen, raw, &pd.produced);
                    gstaDetwiddlePal4(raw.data(), raw.size(), W, H, pd.lin);
                    pd.raw = raw;
                    pd.W = W; pd.H = H; pd.destLen = destLen; pd.ok = true;
                    pd.srcStart = srcStart; pd.srcEnd = srcEnd;
                }
            }
            pit = _gstaPartCache.emplace(key, std::move(pd)).first;
        }
        const GstaDecodedPart& pd = pit->second;
        if (!pd.ok) continue;

        int colRaw = colrow[2 * q], row = colrow[2 * q + 1];
        int W = pd.W, H = pd.H;
        auto re = runExt.find(key);
        int cols = (re != runExt.end()) ? (re->second.mc + 1) : 1;
        int rows = (re != runExt.end()) ? (re->second.mr + 1) : 1;
        int col = (S[q].mirror & 1u) ? (cols - 1 - colRaw) : colRaw;
        int m = (cols > 0) ? (W / cols) : W;
        int mR = (rows > 0) ? (H / rows) : H;
        if (mR < m) m = mR;
        if (m <= 0) m = 32; if (m > 32) m = 32;
#ifdef TEXFIX2
        /* [TEXFIX2 2026-07-05 — DESC-KEYED CARVE, supersedes TEXFIX's rank+mod]
         * The walker's OWN per-tile descriptor (DESC_TABLE 0x8C1F9F9C, entry = recidx -
         * arena_base, rebuilt pose-correct by rebuild_tile_grid before every walk) carries
         * the ENGINE-AUTHORITATIVE carve key: [0]=m (tile px), [2]=cx (STORAGE column,
         * facing-independent), [3]=rows-row. Rank-based col/row is only an approximation
         * that BREAKS when two satellite instances of the same (gfx1,sel) interleave on
         * screen X (global ranks alternate between nodes -> garbage cols). MEASURED
         * vf1795922 sel 0xDEC: two instances, ranks 0..15 interleaved, desc cx 0..7 per
         * node. No mirror reversal here: cx IS storage; the visual flip stays the texU
         * mirror alone (the single-source-of-flip invariant). Bit15 quads keep the legacy
         * path (their recidx is a scale-walker alloc, not a DESC index). */
        if (!isEff[q]) {
            /* EMIT-TIME SOURCE DESC (render_frame_quad_srcdesc_impl): [m,cx,ry,flags] the
             * walker itself consumed — clobber-proof vs the shared-scratch desc rebuild
             * overwrite (measured overlap on torn +0xDC: recidx 464 claimed by Cable
             * 0xD4C m32 AND satellite 0xDE6 m16 at DECODE time; emit-time is unambiguous). */
            int dm  = srcDesc[4*q+0];
            int dcx = srcDesc[4*q+1];
            int dry = srcDesc[4*q+2];
            int dfl = srcDesc[4*q+3];
            int usz = 8 << ((S[q].tsp >> 3) & 7);
            int mq = (int)(S[q].u1 * (float)usz + 0.5f);
            if (mq < 1) mq = 1; if (mq > 32) mq = 32;
            int pCols = W / mq; if (pCols < 1) pCols = 1;
            int pRows = H / mq; if (pRows < 1) pRows = 1;
            if ((dfl & 1) && dm == mq) {
                /* DESC-KEYED: cx = STORAGE column (facing-INDEPENDENT). flags bit1 =
                 * per-record flip4000 = DRAW-TIME texU mirror ONLY (loc_8c0346c4,
                 * re_kb/24), NEVER a storage re-store. The old
                 * `col = (dfl&2) ? (pCols-1-cc) : cc` DOUBLE-APPLIED 0x4000 on the LINEAR
                 * path (twin of the re_kb/71 native double-apply); native overrides col
                 * with ncol so it was immune, linear used col directly -> reversed every
                 * flip4000 horizontal multi-col part. BYTE-GATED spurious over the whole
                 * 59-char GFX2 catalog (_zz_catalog_carve_gate.mjs): reversal ON = 2618
                 * BAD parts / 7664 BAD tiles; OFF (this) = 0 bad, 0 regression. */
                int cc = dcx % pCols;
                col = cc;
                int rr = pRows - dry;             /* desc[3] = rows - row */
                if (rr < 0) rr = 0; if (rr >= pRows) rr = pRows - 1;
                row = rr;
            } else {
                /* FALLBACK: part-grid + screen-rank wrap (texU-mirror-keyed reversal). */
                int cc = (S[q].mirror & 1u) ? (cols - 1 - colRaw) : colRaw;
                col = ((cc % pCols) + pCols) % pCols;
                row = ((colrow[2*q+1] % pRows) + pRows) % pRows;
            }
            m = mq;
            cols = pCols; rows = pRows;           /* native-chunk gate keys on the part grid */
        }
#elif defined(TEXFIX)
        /* [TEXFIX 2026-07-05, finding: satellite carve-grid collapse] The rank-extent grid
         * (cols/rows = max distinct-Ax/Ay rank + 1, ranked PER (gfx1,sel) ACROSS THE WHOLE
         * SCENE) breaks whenever the same part is drawn more than once per frame — multiple
         * satellite instances sharing (gfx1,sel), or a screen-tiled repeat — because the
         * merged ranks overcount the part's real columns/rows: m = W/cols collapses (16->8,
         * 8->5) and high ranks carve out of range (the ZERO tail). MEASURED texel_gate
         * vf1795922: sel 0xDEC W=128 (8 cols x 16px) ranked cols 0..15 across two typhoon
         * instances -> m=8, cols 12+ zero; bodies immune (single instance, grid==part).
         * FIX: the ENGINE tile size comes from the quad's OWN UV span (render_frame.c body
         * path: u1 = m/tile, tile = usize = 8<<TSP.texU  =>  mq = u1*usize = descriptor m),
         * and the part grid from the part's OWN FULL-SPAN dims (pCols=W/mq, pRows=H/mq);
         * the screen rank maps into storage by WRAP (col%pCols, row%pRows). The rank
         * derivation (Ax-ASC storage col, Ay-DESC row) and the facing-mirror reversal over
         * the SCREEN extent are UNCHANGED (reversal first, then wrap). Single-instance
         * parts are bit-identical (sCols==pCols, W/cols==mq). Bit15 quads keep the legacy
         * path (staging behavior unchanged this pass). */
        if (!isEff[q]) {
            int usz = 8 << ((S[q].tsp >> 3) & 7);
            int mq = (int)(S[q].u1 * (float)usz + 0.5f);
            if (mq < 1) mq = 1; if (mq > 32) mq = 32;
            int pCols = W / mq; if (pCols < 1) pCols = 1;
            int pRows = H / mq; if (pRows < 1) pRows = 1;
            m = mq;
            col = ((col % pCols) + pCols) % pCols;
            row = ((row % pRows) + pRows) % pRows;
            /* native-chunk gate below keys on the PART grid, not the rank extent */
            cols = pCols; rows = pRows;
        }
#endif
        int Tw = W / 32, Th = H / 32;
        if (m == 32 && cols > 1 && rows > 1 && !pd.raw.empty()) {
            /* NATIVE-CHUNK ORDER = WHOLE-PART Y-FIRST TWIDDLE (gstaTwTileYFirst). CORRECTED
             * 2026-07-10 (re_kb/70): the roster byte-gate (_zz_roster_carve_gate.mjs) MEASURED
             * the colpair/descriptor order WRONG on every 4x8 (128x256) + the 8x8 (256x256) —
             * 608 bad tiles / 37 parts / 13 chars (incl Sentinel rocket-punch 4x8 sel570..1167);
             * gstaTwTileYFirst = 0 bad, all shapes. == colpair for Tw<=4&&Th<=4 (zero-regression).
             * The engine stores each part as ONE verbatim WxH twiddle blob (DMA loc_8c033d78). */
            // FLIP4000 fix (re_kb/24, 2026-07-10): the per-record 0x4000 is a DRAW-TIME texU mirror
            // ONLY, never a storage re-store. The col reversal above (dfl&2) double-applied it on
            // the native path -> Storm Lightning-Strike flying-pose (sel0x35d flip4000 64x64) tile
            // column-SWAP. Use the RAW storage column for the native twiddle-chunk; the reversed
            // col stays for the LINEAR path. Byte-gated 52/52+48/48 EXACT (_storm_native_gate.mjs).
            int ncol = col;
            if ((dfl & 1) && dm == (int)m) ncol = ((dcx % pCols) + pCols) % pCols;
            int k = gstaTwTileYFirst(ncol, row, Tw, Th);
            size_t o = (size_t)k * 512;
            if (o + 512 <= pd.raw.size()) {
                outTiles.emplace_back();
                TileWrite& tw = outTiles.back();
                tw.vaddr = vaddr;
                memcpy(tw.bytes, &pd.raw[o], 512);
                written++;
                continue;
            }
        }
        int ox = col * m, oy = row * m;
        std::fill(tileLin.begin(), tileLin.end(), 0);
        for (int yy = 0; yy < m; yy++) {
            int py = oy + yy; if (py >= H) break;
            const uint8_t* rowBase = &pd.lin[(size_t)py * W];
            uint8_t* dst = &tileLin[(size_t)yy * 32];
            for (int xx = 0; xx < m; xx++) {
                int px = ox + xx; if (px >= W) break;
                dst[xx] = rowBase[px];
            }
        }
        gstaRetwiddle32(tileLin.data(), tile512.data());
        outTiles.emplace_back();
        TileWrite& tw = outTiles.back();
        tw.vaddr = vaddr;
        memcpy(tw.bytes, tile512.data(), 512);
        written++;
    }
    return written;
}

/* detwiddle ONE 32x32 PAL4 512B tile -> 1024B linear indices (for window compare). */
static void detw32(const uint8_t* t512, uint8_t* lin1024){
    for (int y = 0; y < 32; y += 4) for (int x = 0; x < 32; x += 4) {
        int blk = (gsta_DETW[0][5][x] + gsta_DETW[1][5][y]) / 16;
        int base = blk * 8;
        for (int i = 0; i < 16; i++) {
            int cx = gsta_PAL4_ORDER[i][0], cy = gsta_PAL4_ORDER[i][1];
            uint8_t b = t512[base + (i >> 1)];
            lin1024[(size_t)(y + cy) * 32 + (x + cx)] = (i & 1) ? ((b >> 4) & 0xF) : (b & 0xF);
        }
    }
}

int main(int argc, char** argv){
    if (argc < 4) { fprintf(stderr, "usage: %s <ram16> <btcw> <eng_band> [cli_band]\n", argv[0]); return 2; }
    static uint8_t RAM[RAM_SIZE];
    { FILE* f=fopen(argv[1],"rb"); if(!f){perror("ram");return 2;} size_t g=fread(RAM,1,RAM_SIZE,f); fclose(f); if(g!=RAM_SIZE){fprintf(stderr,"short ram\n");return 2;} }
    static u32 BT[65536]; int btWords=0;
    { FILE* f=fopen(argv[2],"rb"); if(!f){perror("btcw");return 2;} btWords=(int)fread(BT,4,65536,f); fclose(f); }
    std::vector<uint8_t> engBand(0x80000), cliBand;
    { FILE* f=fopen(argv[3],"rb"); if(!f){perror("eng");return 2;} fread(engBand.data(),1,0x80000,f); fclose(f); }
    if (argc >= 5) { cliBand.resize(0x80000); FILE* f=fopen(argv[4],"rb"); if(!f){perror("cli");return 2;} fread(cliBand.data(),1,0x80000,f); fclose(f); }

    static Sh4Ctx c; memset(&c,0,sizeof c); c.ram=RAM;
    render_frame_set_body_tcws(BT, btWords);
    render_frame(&c);
    int nQuad = render_frame_nscene();
    std::vector<SceneQuad> S(render_frame_scene(), render_frame_scene()+nQuad);
    std::vector<int> colrow((size_t)nQuad*2, 0);
    render_frame_quad_colrow_impl(colrow.data(), (u32)nQuad);
    std::vector<uint8_t> isEff((size_t)nQuad, 0);
    render_frame_quad_is_effect_impl(isEff.data(), (u32)nQuad);
    std::vector<uint8_t> srcDesc((size_t)nQuad*4, 0);
    render_frame_quad_srcdesc_impl(srcDesc.data(), (u32)nQuad);

    /* PARITY PIN (verbatim from maplecast_mirror.cpp, arena==400 -> LOW half).
     * PIN SCOPE FIX (2026-07-05): only the double-buffered body arena (byteaddr
     * [0x440000,0x460000) -> -0x30000). Bit15 effect tcws (0x475xxx/0x60xxxx/0x400xxx,
     * MEASURED not double-buffered: same addrs at both arena parities m1330/m1345) keep
     * their shipped addr. Build with -DOLDPIN for the pre-fix blanket shift (A/B). */
    uint32_t arena = gramU32(RAM, 0x1F9D94);
    if (arena == 400) {
        for (int q = 0; q < nQuad; q++) {
            uint32_t g = S[q].gfx1;
            bool isBody = ((g & 0x0C000000u) || (g & 0x8C000000u)) && !(g >= 0x0CED0000u && g < 0x0CEE0000u);
            if (!isBody) continue;
            uint32_t ta = S[q].tcw & 0x1FFFFFu;
#ifdef OLDPIN
            if (ta >= 0x6000u) S[q].tcw = (S[q].tcw & ~0x1FFFFFu) | (ta - 0x6000u);
#else
            if (ta >= 0x88000u && ta < 0x8C000u) S[q].tcw = (S[q].tcw & ~0x1FFFFFu) | (ta - 0x6000u);
#endif
        }
    }

    std::vector<TileWrite> tiles;
    int written = decodeBodies(RAM, S.data(), nQuad, colrow, isEff, srcDesc, tiles);
    printf("=== %s: nQuad=%d staged=%d arena=%u ===\n", argv[1], nQuad, written, arena);

    /* staged band — seeded from the PREFIX VRAM band (argv[5], models the client's resident
     * seed: prefix -> then fr.tiles overwrite; unstaged resident-backed quads now gate against
     * their true resident texels instead of a false ZERO) */
    std::vector<uint8_t> myBand(0x80000, 0);
    if (argc >= 6) { FILE* f=fopen(argv[5],"rb"); if(f){ fread(myBand.data(),1,0x80000,f); fclose(f);} }
    for (auto& t : tiles) if (t.vaddr >= BAND && t.vaddr+512 <= BANDEND) memcpy(&myBand[t.vaddr-BAND], t.bytes, 512);

    /* (1) CERTIFY vs live client band */
    if (!cliBand.empty()) {
        int ex=0, df=0;
        for (auto& t : tiles) {
            if (t.vaddr < BAND || t.vaddr+512 > BANDEND) continue;
            if (memcmp(t.bytes, &cliBand[t.vaddr-BAND], 512)==0) ex++; else df++;
        }
        printf("CERTIFY vs live client band: tiles exact=%d diff=%d %s\n", ex, df, df==0?"[REPLICA==NATIVE]":"[MISMATCH]");
    }

    /* (2) GATE vs engine band — window-level */
    struct Agg { int E=0, W=0, Z=0, BZ=0; };
    std::map<uint32_t, Agg> byPal;
    struct Bad { uint32_t sel, addr, pal; int col,row,mW,mH; const char* cls; };
    std::vector<Bad> bads;
    gstaTwInit();
    uint8_t linC[1024], linE[1024], linE2[1024];
    for (int q = 0; q < nQuad; q++) {
        uint32_t g = S[q].gfx1;
        if (!(g & 0x0C000000u) && !(g & 0x8C000000u)) continue;
        if (g >= 0x0CED0000u && g < 0x0CEE0000u) continue;
        uint32_t vaddr = (S[q].tcw & 0x1FFFFF) << 3;
        if (vaddr < BAND || vaddr+512 > BANDEND) continue;
        float w = fabsf(S[q].Bx - S[q].Ax), h = fabsf(S[q].Dy - S[q].Ay);
        int mW = (int)(w*3.0f/5.0f + 0.5f), mH = (int)(h*7.0f/15.0f + 0.5f);
        if (mW < 1) mW = 1; if (mW > 32) mW = 32; if (mH < 1) mH = 1; if (mH > 32) mH = 32;
        uint32_t pal = (S[q].tcw >> 21) & 0x3F;
        detw32(&myBand[vaddr-BAND], linC);
        detw32(&engBand[vaddr-BAND], linE);
        uint32_t tw = (vaddr >= 0x440000) ? vaddr-0x30000 : vaddr+0x30000;
        bool haveTwin = (tw >= BAND && tw+512 <= BANDEND);
        if (haveTwin) detw32(&engBand[tw-BAND], linE2);
        int neq=0, neq2=0, cz=0, ez=0, ez2=0, n=0;
        for (int y=0;y<mH;y++) for (int x=0;x<mW;x++){
            uint8_t cv=linC[y*32+x], e1=linE[y*32+x], e2=haveTwin?linE2[y*32+x]:0;
            n++;
            if (cv==e1) neq++;
            if (haveTwin && cv==e2) neq2++;
            if (cv==0) cz++;
            if (e1) ez++;
            if (e2) ez2++;
        }
        const char* cls;
        if (neq==n || (haveTwin && neq2==n)) cls = (cz==n) ? "BOTHZERO" : "EXACT";
        else if (cz==n && (ez>0 || ez2>0)) cls = "ZERO";
        else cls = "WRONG";
        Agg& a = byPal[pal];
        if (!strcmp(cls,"EXACT")) a.E++;
        else if (!strcmp(cls,"WRONG")) { a.W++; bads.push_back({S[q].sel, vaddr, pal, colrow[2*q], colrow[2*q+1], mW, mH, "WRONG"}); }
        else if (!strcmp(cls,"ZERO")) { a.Z++; bads.push_back({S[q].sel, vaddr, pal, colrow[2*q], colrow[2*q+1], mW, mH, "ZERO"}); }
        else a.BZ++;
    }
    for (auto& kv : byPal)
        printf("GATE pal%u: EXACT=%d WRONG=%d ZERO=%d BOTHZERO=%d\n", kv.first, kv.second.E, kv.second.W, kv.second.Z, kv.second.BZ);
    for (auto& b : bads)
        printf("  %s sel=0x%X addr=0x%X pal=%u col=%d row=%d win=%dx%d\n", b.cls, b.sel, b.addr, b.pal, b.col, b.row, b.mW, b.mH);

    /* (2b) MAP DIAGNOSTIC (TEXMAP env): for every pal17 quad, search the DECODED part for the
     * storage window that byte-matches the ENGINE tile at the quad's addr -> reveals the true
     * rank->storage mapping rule. Prints: sel rank(col,row) emissionIdx -> engine-matching
     * storage (sx,sy) in mq units (or NONE / AMBIG count). */
    if (getenv("TEXMAP")) {
        std::map<uint64_t,int> emis;   /* per (gfx1,sel) emission counter, scene order */
        for (int q = 0; q < nQuad; q++) {
            uint32_t g = S[q].gfx1;
            if (!(g & 0x0C000000u) && !(g & 0x8C000000u)) continue;
            if (g >= 0x0CED0000u && g < 0x0CEE0000u) continue;
            uint32_t pal = (S[q].tcw >> 21) & 0x3F;
            uint64_t key = ((uint64_t)g << 32) | S[q].sel;
            int em = emis[key]++;
            if (pal != 17) continue;
            uint32_t vaddr = (S[q].tcw & 0x1FFFFF) << 3;
            if (vaddr < BAND || vaddr+512 > BANDEND) continue;
            auto pit = _gstaPartCache.find(key);
            if (pit == _gstaPartCache.end() || !pit->second.ok) continue;
            const GstaDecodedPart& pd = pit->second;
            int usz = 8 << ((S[q].tsp >> 3) & 7);
            int mq = (int)(S[q].u1 * (float)usz + 0.5f);
            if (mq < 1) mq = 1; if (mq > 32) mq = 32;
            float w = fabsf(S[q].Bx - S[q].Ax);
            int mW = (int)(w*3.0f/5.0f + 0.5f); if (mW<1) mW=1; if (mW>32) mW=32;
            detw32(&engBand[vaddr-BAND], linE);
            /* engine window all-zero? skip (unmeasurable) */
            int ez=0; for (int y=0;y<mW;y++) for (int x=0;x<mW;x++) ez += (linE[y*32+x]!=0);
            if (!ez) { printf("MAP sel=0x%X rank(%d,%d) em=%d engine-window-zero\n", S[q].sel, colrow[2*q], colrow[2*q+1], em); continue; }
            /* search every mq-aligned AND half-step storage window */
            int hits=0, hx=-1, hy=-1;
            for (int sy = 0; sy + mW <= pd.H; sy += mq/2 ? mq/2 : 1)
              for (int sx = 0; sx + mW <= pd.W; sx += mq/2 ? mq/2 : 1) {
                bool okm=true;
                for (int y=0;y<mW && okm;y++) for (int x=0;x<mW;x++)
                    if (pd.lin[(size_t)(sy+y)*pd.W + sx+x] != linE[y*32+x]) { okm=false; break; }
                if (okm){ hits++; if(hits==1){hx=sx;hy=sy;} }
              }
            printf("MAP sel=0x%X W=%d H=%d mq=%d win=%d rank(%d,%d) mir=%u em=%d -> %s (%d,%d) hits=%d\n",
                S[q].sel, pd.W, pd.H, mq, mW, colrow[2*q], colrow[2*q+1], S[q].mirror, em,
                hits? "storage" : "NONE", hx, hy, hits);
        }
    }

    /* (2c) DESC DIAGNOSTIC (TEXDESC env): per pal17 quad print the DESC_TABLE entry at
     * idx = recidx - arena_base (the walker's own alloc index) -> (m, cnt, cx, ry) and the
     * implied storage (col,row). Cross-check against MAP's content-derived storage. */
    if (getenv("TEXDESC")) {
        uint32_t arenaBase = gramU32(RAM, 0x1F9D94);
        for (int q = 0; q < nQuad; q++) {
            uint32_t pal = (S[q].tcw >> 21) & 0x3F;
            if (getenv("TEXDESC")[0]=='A' ? 0 : (pal != 17 && pal != 16)) continue;
            uint32_t idx = S[q].recidx - arenaBase;
            uint32_t a = (0x1F9F9Cu + idx*4u);
            uint8_t dm = RAM[a], dcnt = RAM[a+1], dcx = RAM[a+2], dry = RAM[a+3];
            printf("DESC pal%u sel=0x%X recidx=%u idx=%u m=%u cnt+1=%u cx=%u ry=%u addr=0x%X rank(%d,%d)\n",
                pal, S[q].sel, S[q].recidx, idx, dm, dcnt+1, dcx, dry,
                (S[q].tcw & 0x1FFFFF) << 3, colrow[2*q], colrow[2*q+1]);
        }
    }

    /* (2d) PIXEL DUMP (TEXDUMP=<selhex>): print MY decoded part rows (nibbles) + each ENGINE
     * tile window for that sel, so the shift/structure is visible. */
    if (const char* ds = getenv("TEXDUMP")) {
        uint32_t dsel = (uint32_t)strtoul(ds, nullptr, 16);
        for (auto& kv : _gstaPartCache) {
            if ((uint32_t)(kv.first & 0xFFFFFFFF) != dsel || !kv.second.ok) continue;
            const GstaDecodedPart& pd = kv.second;
            printf("MYLIN sel=0x%X W=%d H=%d (rows of hex nibbles):\n", dsel, pd.W, pd.H);
            for (int y = 0; y < pd.H && y < 16; y++) {
                for (int x = 0; x < pd.W && x < 128; x++) putchar("0123456789ABCDEF"[pd.lin[(size_t)y*pd.W+x] & 0xF]);
                putchar('\n');
            }
        }
        for (int q = 0; q < nQuad; q++) {
            if (S[q].sel != dsel) continue;
            uint32_t vaddr = (S[q].tcw & 0x1FFFFF) << 3;
            if (vaddr < BAND || vaddr+512 > BANDEND) continue;
            detw32(&engBand[vaddr-BAND], linE);
            float w = fabsf(S[q].Bx - S[q].Ax);
            int mW = (int)(w*3.0f/5.0f + 0.5f); if (mW<1) mW=1; if (mW>32) mW=32;
            printf("ENG tile addr=0x%X rank(%d,%d) win=%d:\n", vaddr, colrow[2*q], colrow[2*q+1], mW);
            for (int y = 0; y < mW && y < 16; y++) {
                for (int x = 0; x < mW; x++) putchar("0123456789ABCDEF"[linE[y*32+x] & 0xF]);
                putchar('\n');
            }
        }
    }

    /* (2e) CONTENT-LEVEL GATE (TEXCONTENT=<band2>[,<band3>...]): addr-free check that every
     * staged NONZERO tile's 512B content exists somewhere (0x200-aligned) in the engine bands
     * (this frame + neighbors) — separates "carve produces real engine tiles" from the
     * addr-pairing skew caused by the +/-1-frame satellite-pool tearing. */
    if (const char* cb = getenv("TEXCONTENT")) {
        std::vector<std::vector<uint8_t>> bands;
        bands.push_back(engBand);
        { std::string s(cb); size_t p0=0;
          while (p0 < s.size()) { size_t p1=s.find(',',p0); if(p1==std::string::npos)p1=s.size();
            std::string fn=s.substr(p0,p1-p0);
            std::vector<uint8_t> b(0x80000);
            FILE* f=fopen(fn.c_str(),"rb"); if(f){ fread(b.data(),1,0x80000,f); fclose(f); bands.push_back(std::move(b)); }
            p0=p1+1; } }
        std::set<std::vector<uint8_t>> engTiles;
        for (auto& b : bands)
            for (size_t o = 0; o + 512 <= b.size(); o += 0x200) {
                bool nz=false; for (int i=0;i<512;i++) if(b[o+i]){nz=true;break;}
                if (nz) engTiles.insert(std::vector<uint8_t>(b.begin()+o, b.begin()+o+512));
            }
        std::map<uint32_t,std::pair<int,int>> res; /* pal -> (found, notfound) */
        for (int q = 0; q < nQuad; q++) {
            uint32_t g = S[q].gfx1;
            if (!(g & 0x0C000000u) && !(g & 0x8C000000u)) continue;
            if (g >= 0x0CED0000u && g < 0x0CEE0000u) continue;
            uint32_t vaddr = (S[q].tcw & 0x1FFFFF) << 3;
            if (vaddr < BAND || vaddr+512 > BANDEND) continue;
            const uint8_t* t = &myBand[vaddr-BAND];
            bool nz=false; for (int i=0;i<512;i++) if(t[i]){nz=true;break;}
            if (!nz) continue;   /* zero tiles judged by the window gate, not here */
            uint32_t pal = (S[q].tcw >> 21) & 0x3F;
            bool found = engTiles.count(std::vector<uint8_t>(t, t+512)) != 0;
            if (found) res[pal].first++; else res[pal].second++;
        }
        for (auto& kv : res)
            printf("CONTENT pal%u: staged-nonzero tiles found-in-engine=%d NOT-found=%d\n",
                kv.first, kv.second.first, kv.second.second);
    }

    /* (3) per-part LZSS diagnostics */
    printf("--- parts (destLen vs produced; undershoot -> zero tail) ---\n");
    for (auto& kv : _gstaPartCache) {
        const GstaDecodedPart& p = kv.second;
        if (!p.ok) { printf("part gfx1=0x%08X sel=0x%X NOT-OK\n", (uint32_t)(kv.first>>32), (uint32_t)(kv.first&0xFFFFFFFF)); continue; }
        printf("part gfx1=0x%08X sel=0x%04X W=%d H=%d destLen=%zu produced=%zu srcLen=%u %s\n",
            (uint32_t)(kv.first>>32), (uint32_t)(kv.first&0xFFFFFFFF), p.W, p.H, p.destLen, p.produced,
            p.srcEnd - p.srcStart, p.produced < p.destLen ? "<-- UNDERSHOOT" : "");
    }
    return 0;
}
