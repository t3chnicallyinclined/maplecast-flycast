// gsta_stage.cpp — native MVC2 STAGE renderer for the GSTA client. See gsta_stage.h.
//
// GROUND TRUTH (cited):
//  - Stage list = OPAQUE (ListType 0), ParaType 4 (Polygon). CONFIRMED-BY-MEASUREMENT:
//    atlas/stages/STG0B_ta.json (engine-TA bake) = all 72 meshes ListType=0 ParaType=4
//    (PCW high byte 0x80). Bodies stay TR (ListType 2, PCW|0x02000000 per
//    gen_submit_params.c loc_8c1246b0). flycast draws OP->PT->TR so stage is BEHIND bodies.
//  - Camera XMTRX = M1(0x8C2D6B18) . M2(0x8C2D6AD8), col-major matmul (loc_8c120540);
//    fv = XMTRX.(x,y,z,1); screen = fv[0..1]/fv[3]; depth = 1/fv[3]
//    (transform_object_122560 / re_kb finding:stage_transform_is_m1m2_CONFIRMED).
//  - Per-mesh world-authored gate + per-vertex RGB modulate + intensity floor:
//    faithful port of stage-client.mjs _buildFromTA + _meshIsWorldAuthored
//    (re_kb finding:replica_live_stage_black_fix).

#include "gsta_stage.h"

#ifdef MAPLECAST_GSTA_CLIENT_BUILD

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include "../deps/json/json.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

// Directory containing the running executable (so STGxx_ta.json resolves regardless of
// the launch cwd — the user runs flycast from $HOME and the cwd-relative bases miss).
// Returns "" if it can't be determined.
static std::string gstaExeDir()
{
    static std::string cached;
    static bool done = false;
    if (done) return cached;
    done = true;
#if defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::string p(buf, n);
        size_t slash = p.find_last_of("\\/");
        if (slash != std::string::npos) cached = p.substr(0, slash);
    }
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        std::string p(buf);
        size_t slash = p.find_last_of('/');
        if (slash != std::string::npos) cached = p.substr(0, slash);
    }
#endif
    return cached;
}

using json = nlohmann::json;

// ---- TA emit layout constants (match core/hw/pvr/ta.cpp ta_handle_cmd FSM) ----
// Polygon global param (ParaType 4) is 32B: PCW, ISP, TSP, TCW, then 4 reserved/face words.
// Vertex (ParaType 7) for Polygon Type 3 (Col_Type=0, Textured, UV32) is 32B:
//   PCW(0xE0..), xyz[3] f32, u f32, v f32, BaseCol u32, OffsCol u32 (=TA_Vertex3).
// We force obj_ctrl = Texture only (0x08) so the FSM selects vertex type 3 regardless of
// the engine mesh's original Col_Type — we always carry a packed ARGB8888 base colour.
static constexpr float SCREEN_W = 640.f, SCREEN_H = 480.f;
// (the old whole-tri MARGIN reject was replaced by a per-tri screen-bbox-overlap test in
//  gstaStageEmitTA — see re_kb finding:gsta_stage_floor_cull_fix.)

struct StageVtx { float pos[3]; float world[3]; float uv[2]; uint8_t rgb[3]; bool hasWorld; };
struct StageMesh {
    uint32_t pcw, isp, tsp, tcw;
    int      textured;
    std::vector<StageVtx> verts;   // 3*nTris, in tri order
    // cached world-authored classification (-1 unknown, 0 no, 1 yes)
    int      wa = -1;
};

struct StageData {
    bool                   ready = false;
    bool                   hasWorld = false;
    uint32_t               stageId = 0xFFFFFFFFu;
    int                    fileIdx = -1;
    std::vector<StageMesh> meshes;
};

static StageData g_stage;

// ---- stage_id (wire @0x8C289638) -> STGxx file index. Mirror of stage-client.mjs
// STAGE_ID_MAP (re_kb 26: only 0x11->STG0B confirmed; else id&0xFF fallback). ----
static int gstaResolveStageFile(uint32_t stageId) {
    if (stageId == 0x11) return 0x0B;     // CONFIRMED live (re_kb 26)
    return (int)(stageId & 0xFF);
}

// ---- LE byte writers into a growing vector ----
static inline void wU32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back((uint8_t)v); o.push_back((uint8_t)(v>>8));
    o.push_back((uint8_t)(v>>16)); o.push_back((uint8_t)(v>>24));
}
static inline void wF32(std::vector<uint8_t>& o, float f) {
    uint32_t u; memcpy(&u,&f,4); wU32(o,u);
}

// ---- column-major matmul: X = A . B (loc_8c120540 / _matmulColMaj) ----
static void matmulColMaj(const float* A, const float* B, float* X) {
    for (int col=0; col<4; col++)
        for (int i=0; i<4; i++)
            X[col*4+i] = A[i]*B[col*4+0] + A[i+4]*B[col*4+1]
                       + A[i+8]*B[col*4+2] + A[i+12]*B[col*4+3];
}

// ---- engine projection: fv = X.(x,y,z,1); persp divide (transform_object_122560) ----
static inline void projectEngine(const float* X, float x, float y, float z,
                                 float& sx, float& sy, float& depth) {
    float fx = X[0]*x + X[4]*y + X[8]*z  + X[12];
    float fy = X[1]*x + X[5]*y + X[9]*z  + X[13];
    float fw = X[3]*x + X[7]*y + X[11]*z + X[15];
    float inv = 1.0f / (fw != 0.f ? fw : 1e-6f);
    sx = fx * inv; sy = fy * inv;
    depth = inv > 1e-9f ? inv : 1e-9f;
}

// ---- world-authored vs local-prop classifier (_meshIsWorldAuthored, re_kb 26) ----
static bool meshIsWorldAuthored(StageMesh& m) {
    if (m.wa >= 0) return m.wa != 0;
    float minZ = 1e30f, xext = 0.f;
    for (auto& v : m.verts) {
        if (!v.hasWorld) { m.wa = 0; return false; }
        if (v.world[2] < minZ) minZ = v.world[2];
        float ax = std::fabs(v.world[0]); if (ax > xext) xext = ax;
    }
    m.wa = (minZ < -500.f || xext > 1000.f) ? 1 : 0;
    return m.wa != 0;
}

// =============================================================================
// LOAD: parse atlas/stages/STGxx_ta.json into g_stage (engine-TA grounded bake).
// =============================================================================
bool gstaStageEnsureLoaded(uint32_t stageId)
{
    if (g_stage.ready && g_stage.stageId == stageId) return true;
    int fileIdx = gstaResolveStageFile(stageId);

    // base dir resolution order (CONFIRMED-BY-MEASUREMENT: the user launches the GSTA
    // client from $HOME, so the cwd-relative bases below MISS and the stage silently
    // fails to load — re_kb finding:gsta_stage_path_from_binary). Resolve, in order:
    //   1. MAPLECAST_GSTA_STAGE_DIR (explicit override),
    //   2. the EXECUTABLE's own dir + ./atlas/stages + ../atlas/stages + ../../atlas/stages
    //      (binary lives in build/, repo root is build/.. so ../atlas/stages hits),
    //   3. cwd-relative fallbacks (legacy; work when launched from the repo root/build/).
    std::string base, ed = gstaExeDir();
    if (const char* e = std::getenv("MAPLECAST_GSTA_STAGE_DIR")) base = e;
    std::string edStages   = ed.empty() ? "" : ed + "/atlas/stages";
    std::string edUp1      = ed.empty() ? "" : ed + "/../atlas/stages";
    std::string edUp2      = ed.empty() ? "" : ed + "/../../atlas/stages";
    const char* candBases[] = { base.c_str(),
                                ed.c_str(), edStages.c_str(), edUp1.c_str(), edUp2.c_str(),
                                "atlas/stages", "../atlas/stages", "../../atlas/stages" };
    char fname[64];
    snprintf(fname, sizeof(fname), "STG%02X_ta.json", fileIdx & 0xFF);

    FILE* f = nullptr; std::string path;
    for (const char* b : candBases) {
        if (!b || !*b) continue;
        path = std::string(b) + "/" + fname;
        f = fopen(path.c_str(), "rb");
        if (f) break;
    }
    if (!f) {
        printf("[GSTA-STAGE] no bake for stage_id=0x%X (file STG%02X_ta.json) in any base; "
               "stage stays unrendered (set MAPLECAST_GSTA_STAGE_DIR)\n", stageId, fileIdx & 0xFF);
        g_stage.ready = false; g_stage.stageId = stageId;   // remember so we don't retry every frame
        return false;
    }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::string buf; buf.resize((size_t)sz);
    size_t rd = fread(&buf[0], 1, (size_t)sz, f); fclose(f);
    buf.resize(rd);

    json d;
    try { d = json::parse(buf); }
    catch (const std::exception& ex) {
        printf("[GSTA-STAGE] STG%02X_ta.json parse failed: %s\n", fileIdx & 0xFF, ex.what());
        g_stage.ready = false; g_stage.stageId = stageId; return false;
    }

    StageData nd;
    nd.stageId = stageId; nd.fileIdx = fileIdx;
    nd.hasWorld = d.value("hasWorld", false);
    if (!d.contains("meshes")) { printf("[GSTA-STAGE] no meshes in bake\n"); return false; }

    for (auto& mj : d["meshes"]) {
        StageMesh m;
        m.pcw = mj.value("pcw", 0u);
        m.isp = mj.value("isp", 0u);
        m.tsp = mj.value("tsp", 0u);
        m.tcw = mj.value("tcw", 0u);
        m.textured = mj.value("textured", 0);
        if (!mj.contains("tris")) continue;
        for (auto& tri : mj["tris"]) {
            if (!tri.is_array()) continue;
            for (auto& vj : tri) {
                StageVtx v; memset(&v, 0, sizeof(v));
                if (vj.contains("pos") && vj["pos"].size() >= 3) {
                    v.pos[0]=vj["pos"][0]; v.pos[1]=vj["pos"][1]; v.pos[2]=vj["pos"][2];
                }
                if (vj.contains("world") && vj["world"].size() >= 3) {
                    v.world[0]=vj["world"][0]; v.world[1]=vj["world"][1]; v.world[2]=vj["world"][2];
                    v.hasWorld = true;
                }
                if (vj.contains("uv") && vj["uv"].size() >= 2) {
                    v.uv[0]=vj["uv"][0]; v.uv[1]=vj["uv"][1];
                }
                if (vj.contains("rgb") && vj["rgb"].size() >= 3) {
                    v.rgb[0]=(uint8_t)(int)vj["rgb"][0];
                    v.rgb[1]=(uint8_t)(int)vj["rgb"][1];
                    v.rgb[2]=(uint8_t)(int)vj["rgb"][2];
                } else {
                    // fall back to the mono intensity (pre-rgb bakes)
                    float i = vj.value("i", 1.0f);
                    int c = (int)std::lround(std::fmax(0.f, std::fmin(1.f, i)) * 255.f);
                    v.rgb[0]=v.rgb[1]=v.rgb[2]=(uint8_t)c;
                }
                m.verts.push_back(v);
            }
        }
        if (!m.verts.empty()) nd.meshes.push_back(std::move(m));
    }
    nd.ready = !nd.meshes.empty();
    g_stage = std::move(nd);
    size_t triTotal = 0; for (auto& m : g_stage.meshes) triTotal += m.verts.size()/3;
    printf("[GSTA-STAGE] loaded STG%02X (engine-TA grounded) from %s: %zu meshes, %zu tris, hasWorld=%d\n",
           fileIdx & 0xFF, path.c_str(), g_stage.meshes.size(), triTotal, (int)g_stage.hasWorld);
    return g_stage.ready;
}

bool gstaStageReady() { return g_stage.ready; }

// =============================================================================
// EMIT: append the stage OPAQUE polygon-TA, re-projected through the live camera.
// =============================================================================
size_t gstaStageEmitTA(std::vector<uint8_t>& out, const float* M1, const float* M2,
                       const uint8_t* ram)
{
    if (!g_stage.ready) return 0;

    // XMTRX = M1 . M2 (col-major). Re-project world-authored meshes through it.
    float X[16]; bool haveCam = false;
    if (M1 && M2) {
        // sanity: a degenerate camera (all-zero) -> skip reprojection (use baked screen pos)
        bool m1z = true, m2z = true;
        for (int i=0;i<16;i++){ if (M1[i]!=0.f) m1z=false; if (M2[i]!=0.f) m2z=false; }
        if (!m1z && !m2z) { matmulColMaj(M1, M2, X); haveCam = true; }
    }

    // ====================================================================================
    // LIVE HUD RESHAPE (bars-deplete fix, reshape-on-the-rendering-path).
    // The engine-TA stage bake (bake_stage_from_ta.py) baked the ENTIRE MVC2 HUD overlay into
    // STG0B_ta.json as static screen-space meshes (life bars / super-meter / frames / name
    // plates / tag bars) frozen at the capture frame's HP/meter. PROVEN (live HUD_DIAG verify):
    // this baked stage HUD is the ONLY HUD that renders on the native GSTA client — the HUDQ
    // path (gstaBuildHudTA) does NOT reach the screen (no HUDQ tail / not presented). So we
    // RESHAPE the dynamic-fill meshes RIGHT HERE on the path that provably renders: keep the
    // engine's pixel-exact baked geometry/colors/position, and drive ONLY the fill width from
    // the LIVE per-frame state in `ram` (_gstaRam). Zero bandwidth, no HUDQ dependency.
    //
    // BANDS (MEASURED on STG0B_ta.json, tcw & 0x1FFFFF):
    //   RESHAPED: 0x80000          — life-bar FILL quads (w~152 h~8, the team-color body)
    //             0x9de00..0x9e900 — super-meter + per-slot fill bars (one band per element)
    //   LEFT BAKED AS-IS: 0x9be00  — bar frames/backing + bottom tag/round bars (correct for
    //             this matchup), name plates, and the real stage 0x9fc00/0xa0000 floor+skybox.
    // (Permanent fix tracked separately: stage/matchup-general HUD STATE reconstruction +
    //  bake_stage_from_ta.py HUD exclusion. This reshape unblocks depleting bars now.)
    //
    // SLOT MAP (ported verbatim from maplecast_mirror.cpp gstaBuildHudTA): map each fill mesh
    // to a fighter slot by SIDE (mesh center cx<320 => P1) + VISUAL ROW (cluster the side's
    // fill-mesh center-Ys into rows within 6px; active-point-char on the TOP bar). Per-char
    // bases page-616, HP @ +0x420 (max 144). Meter @ globals page-649.
    static const uint32_t CHAR_BASE[6] = { 0x268340,0x2688E4,0x268E88,0x26942C,0x2699D0,0x269F74 };
    static const int P1_SLOTS[3] = {0,2,4}, P2_SLOTS[3] = {1,3,5};
    const uint8_t HP_MAX = 144;
    const bool   haveRam = (ram != nullptr);
    const bool   hudDiag = (std::getenv("MAPLECAST_HUD_DIAG") != nullptr);   // =1 force 25% (reach test)
    auto rd8  = [&](uint32_t a)->uint8_t  { return ram[a & 0x00FFFFFFu]; };
    auto rd16 = [&](uint32_t a)->uint16_t { uint32_t b=a&0x00FFFFFFu; return (uint16_t)(ram[b]|(ram[b+1]<<8)); };

    // Per-mesh: is it a life-bar FILL (0x80000) or a meter FILL (0x9de00..0x9e900)?
    auto bandOf = [](const StageMesh& m){ return m.tcw & 0x1FFFFFu; };
    auto isLifeFill  = [&](const StageMesh& m){ return bandOf(m) == 0x80000u; };
    auto isMeterFill = [&](const StageMesh& m){ uint32_t b=bandOf(m); return b>=0x9de00u && b<=0x9e900u; };

    // Baked X/Y extent of a mesh (screen-space; HUD fill meshes are NOT world-authored so their
    // baked pos == screen pos). Returns false on empty.
    auto meshExtent = [](const StageMesh& m, float& minx, float& maxx, float& cy)->bool{
        if (m.verts.empty()) return false;
        minx=1e30f; maxx=-1e30f; float mny=1e30f, mxy=-1e30f;
        for (auto& v : m.verts){ float x=v.pos[0], y=v.pos[1];
            if (x<minx)minx=x; if (x>maxx)maxx=x; if (y<mny)mny=y; if (y>mxy)mxy=y; }
        cy=(mny+mxy)*0.5f; return true;
    };

    // Active-first slot order per side (active point char on the TOP bar) — same as gstaBuildHudTA.
    auto orderSide = [&](const int* slots, int* outOrder){
        int n=0, rest[3], nr=0;
        for (int i=0;i<3;i++){ uint32_t b=CHAR_BASE[slots[i]];
            if (haveRam && rd8(b+0x000)) outOrder[n++]=slots[i]; else rest[nr++]=slots[i]; }
        for (int i=0;i<nr;i++) outOrder[n++]=rest[i];
    };
    int p1ord[3], p2ord[3]; orderSide(P1_SLOTS,p1ord); orderSide(P2_SLOTS,p2ord);

    // PRE-PASS: per-side sorted ROW list (distinct cy clusters of LIFE-FILL meshes), so the
    // multiple same-cy fill meshes per visible bar (body + highlight pass) all map to one row
    // -> one slot -> shrink together (the gstaBuildHudTA twin-overdraw fix, re_kb/56).
    auto rowsForSide = [&](bool p1)->std::vector<float>{
        std::vector<float> cys;
        for (auto& m : g_stage.meshes){ if (!isLifeFill(m)) continue;
            float mnx,mxx,cy; if(!meshExtent(m,mnx,mxx,cy)) continue;
            bool meshP1 = (((mnx+mxx)*0.5f) < 320.f);
            if (meshP1==p1) cys.push_back(cy); }
        std::sort(cys.begin(), cys.end());
        std::vector<float> rows;
        for (float cy:cys){ if (rows.empty() || cy-rows.back()>6.f) rows.push_back(cy); }
        return rows;
    };
    std::vector<float> p1rows = haveRam ? rowsForSide(true)  : std::vector<float>();
    std::vector<float> p2rows = haveRam ? rowsForSide(false) : std::vector<float>();
    auto lifeSlotForMesh = [&](bool p1, float cy)->int{
        const std::vector<float>& rows = p1 ? p1rows : p2rows;
        int row=0; for (size_t r=0;r<rows.size();r++){ if (std::fabs(cy-rows[r])<=6.f){ row=(int)r; break; } }
        if (row>2) row=2;
        return p1 ? p1ord[row] : p2ord[row];
    };

    size_t startBytes = out.size();
    size_t emittedTris = 0;

    for (auto& m : g_stage.meshes) {
        if (m.verts.empty()) continue;

        // ---- LIVE HUD-FILL RESHAPE (per mesh; see header block above) ----------------
        // Compute, for a dynamic-fill mesh, the INNER x clamp so its width = fullWidth*frac.
        // The fill is anchored to its OUTER (portrait-side) edge; the INNER edge moves toward
        // it as state drops. We clamp fx[k] (the emitted x, below) — NOT g_stage (frame-safe).
        bool   reshapeFill = false;     // this mesh is a state-driven fill
        bool   fillP1      = false;     // anchored to its left edge (P1) vs right edge (P2)
        float  fillOuter   = 0.f;       // the anchored outer edge x (baked)
        float  fillInner   = 0.f;       // the clamped inner edge x (= outer +/- frac*fullWidth)
        if (haveRam && (isLifeFill(m) || isMeterFill(m))) {
            float minx, maxx, cy;
            if (meshExtent(m, minx, maxx, cy)) {
                fillP1 = (((minx+maxx)*0.5f) < 320.f);
                float fullW = maxx - minx;
                float frac  = 1.f;
                if (isLifeFill(m)) {
                    int slot = lifeSlotForMesh(fillP1, cy);
                    frac = (float)rd8(CHAR_BASE[slot] + 0x420) / (float)HP_MAX;
                } else {
                    // SUPER-METER fill. meter_fill @0x8C289646 (P1) — the HUD shows the ACTIVE
                    // side's meter on its bar; map by side. lvl @0x8C28964A gates max but the
                    // fill value already encodes current charge; normalize by the per-level max
                    // (one bar segment = 0..~144 like the life bar; the engine bar texture is the
                    // same swatch). Use the side's meter_fill u16, clamped to [0,1] over a full
                    // segment. (Tracked: exact meter normalization + multi-level rendering.)
                    uint32_t meterAddr = fillP1 ? 0x289646u : 0x289648u;  // P1/P2 meter_fill u16
                    float    raw = (float)rd16(meterAddr);
                    // engine meter segment is 0..~0x90 (144) per the life-bar swatch scale; clamp.
                    frac = raw / 144.f;
                }
                if (hudDiag) frac = 0.25f;                 // HUD_DIAG=1: reach test (snap to 25%)
                if (frac < 0.f) frac = 0.f; if (frac > 1.f) frac = 1.f;
                reshapeFill = true;
                if (fillP1) { fillOuter = minx; fillInner = minx + fullW*frac; }   // P1 anchored LEFT
                else        { fillOuter = maxx; fillInner = maxx - fullW*frac; }   // P2 anchored RIGHT
            }
        }
        // -----------------------------------------------------------------------------

        const size_t nTris = m.verts.size() / 3;

        // PCW: keep the engine ParaType(4)/ListType(0)/depth bits, but FORCE obj_ctrl to
        // Texture-only (0x08) -> the FSM selects vertex type 3 (packed colour, UV32),
        // matching the TA_Vertex3 layout we write below. Clear the low obj_ctrl byte first.
        uint32_t pcw = (m.pcw & 0xFFFFFF00u) | 0x08u;     // Texture=1, Col_Type=0, UV16=0, Gouraud=0
        // guarantee ParaType=4 (Polygon) + ListType=0 (Opaque) on the high bits:
        pcw = (pcw & 0x00FFFFFFu) | (4u << 29) | (0u << 24);

        const bool reproject = haveCam && meshIsWorldAuthored(m);

        for (size_t t = 0; t < nTris; t++) {
            const StageVtx* tv[3] = { &m.verts[t*3+0], &m.verts[t*3+1], &m.verts[t*3+2] };
            // resolve final screen verts first so we can reject the whole tri atomically.
            // CULL MODEL (CONFIRMED-BY-MEASUREMENT, re_kb finding:gsta_stage_floor_cull_fix):
            //  - REJECT a vertex only if it is GARBAGE: non-finite, or a 1/w-clamp sentinel
            //    (depth==SENTINEL_Z, |screen|>1e6 — the 8 grazing/behind-camera verts the
            //    engine TA carries with inv=10 in STG0B mesh0). The OLD `|coord|>MARGIN(800)`
            //    per-vertex reject WRONGLY culled mesh3 — the BLUE FLOOR deck quad — whose 2
            //    triangles legitimately span X +-6822 while crossing the visible bottom band
            //    (Y 362..585). The engine SUBMITS that huge quad and lets the PVR clip it; so
            //    must we. Floor was 0.03 coverage vs engine 0.41 BEFORE this fix.
            //  - then KEEP the triangle iff its SCREEN BBOX OVERLAPS the visible frame (+slack);
            //    flycast's rasterizer clips the off-screen remainder exactly like the engine.
            static constexpr float BBOX_SLACK = 64.f;     // px slop around the 640x480 frame
            static constexpr float GARBAGE    = 1.0e6f;   // |screen px| beyond this = sentinel
            float fx[3], fy[3], fz[3]; bool ok = true;
            float bxMin=1e30f, bxMax=-1e30f, byMin=1e30f, byMax=-1e30f;
            for (int k=0;k<3;k++) {
                float px = tv[k]->pos[0], py = tv[k]->pos[1], pz = tv[k]->pos[2];
                if (reproject && tv[k]->hasWorld) {
                    projectEngine(X, tv[k]->world[0], tv[k]->world[1], tv[k]->world[2], px, py, pz);
                }
                // LIVE HUD-FILL RESHAPE: clamp the INNER-edge x to fillInner so the bar width
                // tracks state. Outer (portrait-side) edge is preserved; only the verts on the
                // inner side move. Frame-safe (mutates the local px only, never g_stage).
                if (reshapeFill) {
                    if (fillP1) { if (px > fillOuter + 0.5f) px = fillInner; }   // anchored LEFT
                    else        { if (px < fillOuter - 0.5f) px = fillInner; }   // anchored RIGHT
                }
                if (!std::isfinite(px) || !std::isfinite(py)
                    || std::fabs(px) > GARBAGE || std::fabs(py) > GARBAGE) { ok = false; break; }
                fx[k]=px; fy[k]=py; fz[k]=pz;
                if (px<bxMin) bxMin=px; if (px>bxMax) bxMax=px;
                if (py<byMin) byMin=py; if (py>byMax) byMax=py;
            }
            if (!ok) continue;
            // bbox must overlap the visible screen (+slack); fully-off-screen tris (upper
            // deck / skybox above the frame) are dropped — they never touch the viewport.
            if (bxMax < -BBOX_SLACK || bxMin > SCREEN_W + BBOX_SLACK
                || byMax < -BBOX_SLACK || byMin > SCREEN_H + BBOX_SLACK) continue;

            // PER-VERTEX RGB MODULATE (re_kb finding:stage_deck_texture_closed): the bake now
            // carries the TYPE-CORRECT per-vertex RGB (the dark-grey/grid ramp for the deck,
            // 0..252). The renderer modulates the texture by it (ShadInstr=1). The OLD
            // floor-to-white (from the pre-deck-fix stage_black era) WASHED the dark grid to
            // full-bright green and hid the real blue floor — DISPROVEN by the engine-TA ground
            // truth raster (GROUNDTRUTH_engine_ta_STG0B.png = a DARK grid + blue floor). Use the
            // real per-vertex RGB verbatim, exactly as stage-client.mjs _buildFromTA now does.

            // --- Polygon global param (32B) ---
            wU32(out, pcw); wU32(out, m.isp); wU32(out, m.tsp); wU32(out, m.tcw);
            wU32(out, 0); wU32(out, 0); wU32(out, 0); wU32(out, 0);   // face/reserved

            // --- 3 vertices (TA_Vertex3, 32B each). 3rd vertex sets PCW end-of-strip 0xF0. ---
            for (int k=0;k<3;k++) {
                uint32_t vpcw = (k == 2) ? 0xF0000000u : 0xE0000000u;  // last vtx ends the strip
                uint8_t r = tv[k]->rgb[0], g = tv[k]->rgb[1], b = tv[k]->rgb[2];
                uint32_t baseCol = 0xFF000000u | ((uint32_t)r<<16) | ((uint32_t)g<<8) | (uint32_t)b;
                wU32(out, vpcw);
                wF32(out, fx[k]); wF32(out, fy[k]); wF32(out, fz[k]);
                wF32(out, tv[k]->uv[0]); wF32(out, tv[k]->uv[1]);
                wU32(out, baseCol);     // BaseCol (ARGB8888)
                wU32(out, 0);           // OffsCol
            }
            emittedTris++;
        }
    }

    if (out.size() == startBytes) return 0;
    static uint64_t _sn = 0;
    if ((_sn++ % 120) == 0)
        printf("[GSTA-STAGE] emitted %zu tris (%zuB) cam=%d\n",
               emittedTris, out.size()-startBytes, (int)haveCam);
    return out.size() - startBytes;
}

#endif // MAPLECAST_GSTA_CLIENT_BUILD
