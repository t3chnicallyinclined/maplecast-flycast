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
#include "gsta_render_debug.h"

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

    // ---- LIVE render-debug globals (control-WS driven, web/gsta-render-debug.html) ----
    auto& DBG = gsta_render_debug::g();
    auto RB = [&](std::atomic<int>& a){ return a.load(std::memory_order_relaxed) != 0; };
    const int  dbg_solo        = DBG.soloMode.load(std::memory_order_relaxed);   // 0 off,1 stage,2 body,3 hud
    // SOLO overrides: solo=1 (stage only) keeps stage+synth-off? -> stage-only = baked geometry only.
    const bool solo_stage      = (dbg_solo == 1), solo_body = (dbg_solo == 2), solo_hud = (dbg_solo == 3);
    const bool dbg_stageOn     = RB(DBG.stageOn)    && !solo_body && !solo_hud;
    const bool dbg_synthOn     = RB(DBG.hudSynthOn) && !solo_stage && !solo_body;
    const bool dbg_filterBaked = RB(DBG.filterBakedDyn);
    const bool dbg_barsOn      = RB(DBG.hudBarsOn);
    const bool dbg_meterOn     = RB(DBG.hudMeterOn);
    const bool dbg_framesOn    = RB(DBG.stageFramesOn) && RB(DBG.hudFramesOn);
    const bool dbg_reproject   = RB(DBG.stageReproject);
    const bool dbg_stageForceC  = RB(DBG.stageForceColorOn);
    const uint32_t dbg_stageColARGB = DBG.stageForceColorARGB.load(std::memory_order_relaxed);
    const bool dbg_gradientOn  = RB(DBG.hudGradientOn);
    const bool dbg_chipOn      = RB(DBG.hudChipOn);
    const bool dbg_highlightOn = RB(DBG.hudHighlightOn);
    const bool dbg_colOverride = RB(DBG.fillColorOverrideOn);
    const uint32_t dbg_colARGB = DBG.fillColorARGB.load(std::memory_order_relaxed);
    const float dbg_zBias      = (float)DBG.zBiasMicro.load(std::memory_order_relaxed) * 1e-6f;
    const float dbg_dx         = (float)DBG.posDX.load(std::memory_order_relaxed);
    const float dbg_dy         = (float)DBG.posDY.load(std::memory_order_relaxed);
    const bool dbg_forceTest   = RB(DBG.forceTestQuad);
    const int  dbg_listType    = DBG.hudListType.load(std::memory_order_relaxed);   // 0 OP, 1 TR

    // XMTRX = M1 . M2 (col-major). Re-project world-authored meshes through it.
    float X[16]; bool haveCam = false;
    if (M1 && M2) {
        // sanity: a degenerate camera (all-zero) -> skip reprojection (use baked screen pos)
        bool m1z = true, m2z = true;
        for (int i=0;i<16;i++){ if (M1[i]!=0.f) m1z=false; if (M2[i]!=0.f) m2z=false; }
        if (!m1z && !m2z) { matmulColMaj(M1, M2, X); haveCam = true; }
    }

    // ====================================================================================
    // PROCEDURAL HUD (state-reconstruction) — the matchup/stage-independent, zero-bandwidth fix.
    // The engine-TA stage bake (bake_stage_from_ta.py) baked the ENTIRE MVC2 HUD overlay into
    // STG0B_ta.json as static screen-space meshes, FROZEN at the capture frame's HP/meter AND the
    // capture matchup's team color. PROVEN (live verify): this baked stage HUD is the ONLY HUD that
    // renders on the native GSTA client (the HUDQ/gstaBuildHudTA path never reaches the screen).
    // Reshaping the frozen art depletes width but cannot fix the team COLOR (which retints per
    // active point char, loc_8c15FFB0) nor the MULTI-LEVEL meter. So we:
    //   (1) DROP the baked DYNAMIC fill meshes here: life-bar fills (0x80000) + meter fills
    //       (0x9de00..0x9e900). (isLifeFill/isMeterFill below; the per-mesh loop `continue`s them.)
    //   (2) KEEP baked: bar FRAMES/backing (0x9be00) + name plates + portraits (correct for this
    //       matchup) + the real stage (0x9fc00/0xa0000).
    //   (3) SYNTHESIZE the life bars + multi-level super meter from _gstaRam state as TA quads
    //       AFTER the loop (emitHudBars block), at the MEASURED STG0B positions, ROM team colors
    //       + gradient + red_health chip + live HP/meter. Ported from web/render-replica/
    //       hud-client.mjs _lifeBar/_meter (re_kb/27,/36,/55). Zero bandwidth (state already shipped).
    // (QUEUED generality: reconstruct plates/names from char_id; strip HUD in bake_stage_from_ta.py
    //  + re-bake so the baked frames also go procedural.)
    //
    // STATE: per-char page-616 bases; active +0x000 (point-char detect), HP +0x420 (max 144),
    // red_health/chip +0x424. Globals page-649: meter_fill u16 0x289646(P1)/648(P2),
    // meter_level u8 0x28964A(P1)/64B(P2) (0..5), METER_MAX=144/level (loc_8C0F0FDC).
    static const uint32_t CHAR_BASE[6] = { 0x268340,0x2688E4,0x268E88,0x26942C,0x2699D0,0x269F74 };
    static const int P1_SLOTS[3] = {0,2,4}, P2_SLOTS[3] = {1,3,5};
    const uint8_t HP_MAX = 144;
    const bool   haveRam = (ram != nullptr);
    const bool   hudDiag = (std::getenv("MAPLECAST_HUD_DIAG") != nullptr);   // =1 force 25% (reach test)
    auto rd8  = [&](uint32_t a)->uint8_t  { return ram[a & 0x00FFFFFFu]; };
    auto rd16 = [&](uint32_t a)->uint16_t { uint32_t b=a&0x00FFFFFFu; return (uint16_t)(ram[b]|(ram[b+1]<<8)); };

    // Per-mesh classifier: is it a baked DYNAMIC HUD FILL that the procedural synthesis replaces?
    // CORRECTED 2026-07-02 (measured vs the live HUDQ oracle + the STG0B bake band histogram):
    // BOTH the life-bar fill AND the super-meter fill are baked at tcw band 0x080000 (the white
    // texel, tcw=0x08080000) — 12 meshes total (6 life + 6 meter). The OLD isMeterFill range
    // 0x9de00..0x9e900 was WRONG: that band is the baked PORTRAITS (0x9de00..0x9e600, tcw 0809deXX)
    // + the LEVEL/EXP icons (0x9e700..0x9e900), NOT the meter fill — so it was DROPPING the baked
    // portraits (native-path "missing portrait"/monogram-fallback root cause). The portraits are
    // byte-exact vs HUDQ (pcw 0x808c002c tsp 0x2009a452 tcw 0x0809deXX, resident VRAM 0x4efXXX) and
    // MUST be KEPT. So isMeterFill now matches the REAL meter fill band (0x080000), same as the life
    // fill; both are dropped + re-synthesized. The 0x9deXX portrait band is no longer dropped.
    // (re_kb finding:hud_p2a_portrait_dropfilter_fix + source:hudq_bar_meter_bytetarget.)
    auto bandOf = [](const StageMesh& m){ return m.tcw & 0x1FFFFFu; };
    auto isLifeFill  = [&](const StageMesh& m){ return bandOf(m) == 0x80000u; };
    auto isMeterFill = [&](const StageMesh& m){ return bandOf(m) == 0x80000u; };

    size_t startBytes = out.size();
    size_t emittedTris = 0;

    for (auto& m : g_stage.meshes) {
        if (m.verts.empty()) continue;

        // ---- LIVE render-debug: whole-stage / frames isolation ----
        if (!dbg_stageOn) continue;                        // hide ALL baked stage geometry
        // hide the baked HUD FRAMES/plates band (0x9be00) when toggled off (A/B the synth alone)
        if (!dbg_framesOn && (m.tcw & 0x1FFFFFu) == 0x9be00u) continue;

        // ---- DROP the baked DYNAMIC HUD-fill meshes (procedural synthesis replaces them) ----
        // The baked life-bar fills (0x80000) and super-meter fills (0x9de00..0x9e900) are FROZEN
        // at the capture frame's HP/meter AND frozen at the capture matchup's TEAM COLOR (loc_
        // 8c15FFB0 per-vertex modulate). They cannot deplete correctly, cannot retint when the
        // active point char changes, and (meter) cannot render the multi-level gauge. We DROP them
        // here and SYNTHESIZE the life bars + meter procedurally from _gstaRam state AFTER this
        // loop (emitHudBars below), at the SAME measured positions. KEPT baked (correct as-is for
        // this matchup): the bar frames/backing (0x9be00), name plates, portraits. (The live
        // `filterBakedDyn` knob can DISABLE this drop to A/B the frozen baked fills vs the synth.)
        if (dbg_filterBaked && haveRam && (isLifeFill(m) || isMeterFill(m))) continue;

        const size_t nTris = m.verts.size() / 3;

        // PCW: keep the engine ParaType(4)/ListType(0)/depth bits, but FORCE obj_ctrl to
        // Texture-only (0x08) -> the FSM selects vertex type 3 (packed colour, UV32),
        // matching the TA_Vertex3 layout we write below. Clear the low obj_ctrl byte first.
        uint32_t pcw = (m.pcw & 0xFFFFFF00u) | 0x08u;     // Texture=1, Col_Type=0, UV16=0, Gouraud=0
        // guarantee ParaType=4 (Polygon) + ListType=0 (Opaque) on the high bits:
        pcw = (pcw & 0x00FFFFFFu) | (4u << 29) | (0u << 24);

        const bool reproject = haveCam && dbg_reproject && meshIsWorldAuthored(m);

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
                if (dbg_stageForceC) baseCol = dbg_stageColARGB;   // isolate stage coverage (test tint)
                wU32(out, vpcw);
                wF32(out, fx[k]); wF32(out, fy[k]); wF32(out, fz[k]);
                wF32(out, tv[k]->uv[0]); wF32(out, tv[k]->uv[1]);
                wU32(out, baseCol);     // BaseCol (ARGB8888)
                wU32(out, 0);           // OffsCol
            }
            emittedTris++;
        }
    }

    // ====================================================================================
    // PROCEDURAL HUD SYNTHESIS (life bars + super meter) — the matchup/stage-independent,
    // zero-bandwidth fix. Replaces the DROPPED baked dynamic meshes with quads synthesized
    // from _gstaRam state at the MEASURED STG0B positions. Ported from the proven Canvas2D
    // reference web/render-replica/hud-client.mjs (_lifeBar / _meter), re_kb/27,/36,/55.
    //
    // Every quad is a WHITE-swatch OPAQUE poly (the same resident texel + control words the
    // engine bars use: pcw=808c001f isp=93400000 tsp=20880440 tcw=08080000, U=V=0) modulated
    // by a per-vertex ARGB8888 base color — exactly the engine's loc_8c15FFB0 white-tex*vertex-
    // color model. Emitted here in the OPAQUE (ListType 0) list, so it lands behind the bodies
    // like the rest of the stage/baked-frame HUD; the baked FRAMES (0x9be00) still draw around it.
    if (haveRam && dbg_synthOn) {
        // -- swatch control words (reused from the baked life fill) --
        // CRITICAL (empty-bars root cause): the baked life-fill PCW is 0x808c001f, whose obj_ctrl
        // low byte 0x1f = UV16|Gouraud|Offset|Texture|ColType=1. That tells the flycast TA FSM
        // (ta.cpp ta_handle_cmd) to expect a Type-5/6 vertex (16-bit UV + FLOAT/intensity color +
        // offset) — a DIFFERENT byte layout than the 32B Type-3 (pcw,x,y,z,u,v,basecol,offscol) we
        // actually write below. Feeding Type-3 bytes under a Type-5 PCW => the FSM misparses stride/
        // fields => our quads land off-screen / degenerate / drop => INVISIBLE (frames render because
        // the baked mesh loop REWRITES its pcw to 0x08 to match its own Type-3 verts, gsta_stage.cpp
        // per-mesh loop `pcw = (m.pcw & 0xFFFFFF00)|0x08`). We do the SAME rewrite here: force
        // obj_ctrl=0x08 (Texture=1, ColType=0, UV32, no gouraud/offset) => Type-3, matching our verts.
        // (This is why the depth fix was necessary-but-not-sufficient: even in front, a mis-typed
        // vertex stream draws nothing.)
        // base PCW: force obj_ctrl=0x08 (Type-3). LIVE list-type knob: 0=OPAQUE (ListType 0,
        // default/correct), 1=TRANSLUCENT (ListType 2) to test drawing over the opaque backing.
        uint32_t HUD_PCW = (0x808c001fu & 0xFFFFFF00u) | 0x08u;         // = 0x808c0008 (Type-3, OPAQUE)
        if (dbg_listType == 1) HUD_PCW = (HUD_PCW & ~(7u<<24)) | (2u<<24);   // ListType = Translucent
        const uint32_t HUD_ISP = 0x93400000u,
                       HUD_TSP = 0x20880440u, HUD_TCW = 0x08080000u;
        // HUD-PLANE DEPTH (Deliverable B fix). PVR Z = 1/W (LARGER = NEARER). The old hardcoded
        // HUD_Z=0.0002 was FAR-plane -> the OPAQUE synth fills were depth-culled BEHIND the co-
        // located baked backing (0x9be00, real Z ~0.0053..0.0078) -> invisible. MEASURED (both
        // the STG0B bake pos[2] AND the _live_hud MIRROR vertex Z agree, B1): the HUD rows do NOT
        // share one depth — row0 (cy52) sits at Z~0.00768, rows1/2 (cy78/98) at Z~0.00525, and
        // the fill at each row matches its OWN backing's Z. So we PASS Z PER QUAD (inherited from
        // the co-located baked backing per row), with a tiny nearer bias (+Z) so the fill always
        // wins the depth test against its frame. Meter rows likewise carry their measured Z.
        const float Z_BIAS = dbg_zBias;   // LIVE per-row nearer bias (default 0.00003, control-WS)

        // Emit one flat-color parallelogram (2 tris) at depth `z` from screen quad corners
        // TL(x0,y0) TR(x1,y0) BR(x1+skew,y1) BL(x0+skew,y1), all verts colored `argb`.
        // LIVE knobs applied here: global position nudge (dbg_dx/dy) + fill-color override.
        auto emitQuad = [&](float x0, float x1, float y0, float y1, float skew, float z, uint32_t argb){
            if (dbg_colOverride) argb = dbg_colARGB;               // force test color (bypass team/gradient)
            if (x1 < x0) { float t=x0; x0=x1; x1=t; }
            const float vx[4] = { x0+dbg_dx, x1+dbg_dx, x1+skew+dbg_dx, x0+skew+dbg_dx };
            const float vy[4] = { y0+dbg_dy, y0+dbg_dy, y1+dbg_dy,      y1+dbg_dy      };
            const int   tri[2][3] = { {0,1,2}, {0,2,3} };
            for (int t=0;t<2;t++){
                wU32(out, HUD_PCW); wU32(out, HUD_ISP); wU32(out, HUD_TSP); wU32(out, HUD_TCW);
                wU32(out, 0); wU32(out, 0); wU32(out, 0); wU32(out, 0);
                for (int j=0;j<3;j++){
                    int k = tri[t][j];
                    uint32_t vpcw = (j==2) ? 0xF0000000u : 0xE0000000u;
                    wU32(out, vpcw);
                    wF32(out, vx[k]); wF32(out, vy[k]); wF32(out, z);
                    wF32(out, 0.f);  wF32(out, 0.f);        // U=V=0 (solid swatch texel)
                    wU32(out, argb); wU32(out, 0);
                }
                emittedTris++;
            }
        };

        // ================================================================================
        // ENGINE-EXACT life-bar strip (byte-matches the engine's real HUD fill quad).
        // MEASURED from _live_hud.mirror.zcst: the engine emits each life bar as ONE 4-vertex
        // STRIP with pcw=0x808c001f (obj_ctrl 0x1f = Textured, Col_Type=1 FLOATING color, 16-bit
        // UV, Gouraud, Offset) -> flycast TA Type-6 = SZ64 vertices (TA_Vertex6A geometry record +
        // TA_Vertex6B float-ARGB color record = 64 bytes/vertex). The gradient is done via PER-VERTEX
        // gouraud FLOAT ARGB (NOT stacked layers): inner corners = opaque team color (A=1), outer
        // corners = warm color with A=0 (transparent) -> the yellow->team fade + the depletion is a
        // single translucent-alpha strip. Offset color = 0. So we emit the SAME control words + the
        // SAME 64B vertex layout with our live per-corner float ARGB.
        //   Vertex6A (32B): vpcw, x, y, z, [u16 v][u16 u], ign, ign, ign
        //   Vertex6B (32B): BaseA,BaseR,BaseG,BaseB (f32), OffsA,OffsR,OffsG,OffsB (f32)
        // corners a=TL b=TR c=BR d=BL (strip order TR,TL,BR,BL matches the engine's e001b1b0 set:
        // (269.5,57.7)(46.6,57.7)(269.5,46.0)(46.6,46.0) = C,D,B,A). We use the strip order
        // v0=BR v1=BL v2=TR v3=TL, each with its own float ARGB. `colA/colB/colC/colD` are ARGB8888
        // (a=alpha). Emits the engine PCW 0x808c001f so it byte-matches.
        const uint32_t FILL_PCW6 = (dbg_listType==1) ? ((0x808c001fu & ~(7u<<24)) | (2u<<24)) : 0x808c001fu;
        auto argbF = [&](uint32_t argb, int idx)->float{ return (float)((argb >> (24-8*idx)) & 0xFF) / 255.f; };
        auto emitVtx6 = [&](uint32_t vpcw, float x, float y, float z, uint32_t argb){
            // 6A geometry
            wU32(out, vpcw); wF32(out, x+dbg_dx); wF32(out, y+dbg_dy); wF32(out, z);
            wU32(out, 0);              // [u16 v][u16 u] = 0 (white swatch)
            wU32(out, 0); wU32(out, 0); wU32(out, 0);   // ign1..3
            // 6B float ARGB base + zero offset
            wF32(out, argbF(argb,0)); wF32(out, argbF(argb,1)); wF32(out, argbF(argb,2)); wF32(out, argbF(argb,3));
            wF32(out, 0.f); wF32(out, 0.f); wF32(out, 0.f); wF32(out, 0.f);
        };
        // One engine-faithful gouraud strip. Corners TL(x0,y0) TR(x1,y0) BR(x1,y1) BL(x0,y1),
        // strip submit order matching the engine (v0=BR,v1=BL,v2=TR,v3=TL); last vtx vpcw ends strip.
        auto emitStrip6 = [&](float x0,float x1,float y0,float y1,float z,
                              uint32_t cTL,uint32_t cTR,uint32_t cBR,uint32_t cBL){
            if (dbg_colOverride) { cTL=cTR=cBR=cBL=dbg_colARGB; }
            // global param (32B): pcw isp tsp tcw + 4 pad
            wU32(out, FILL_PCW6); wU32(out, HUD_ISP); wU32(out, HUD_TSP); wU32(out, HUD_TCW);
            wU32(out, 0); wU32(out, 0); wU32(out, 0); wU32(out, 0);
            emitVtx6(0xE0000000u, x1, y1, z, cBR);   // BR
            emitVtx6(0xE0000000u, x0, y1, z, cBL);   // BL
            emitVtx6(0xE0000000u, x1, y0, z, cTR);   // TR
            emitVtx6(0xF0000000u, x0, y0, z, cTL);   // TL (ends strip)
            emittedTris += 2;
        };
        // emitStrip6 is the engine-exact life-bar fill (called from drawSide below).

        // BYTE-EXACT engine RED CHIP/drain quad. The engine emits the chip as its OWN Type-6 strip
        // with DISTINCT control words from the HP fill: pcw=0x808c002d (obj_ctrl 0x2d, no gouraud
        // gate diff), tsp=0x20080440 (NOTE: 0x20080440 not the fill's 0x20880440 — bit 0x00800000
        // clear), tcw=0x08080000, flat col=0xfffe0000 (A=0xFF). CONFIRMED HUDQ oracle quads [4]/[10]/
        // [16]/[22]/[28]/[34]. Same 4-vert strip vertex layout as emitStrip6, just different global
        // param + a single flat color. (re_kb source:hudq_bar_meter_bytetarget.)
        const uint32_t CHIP_PCW6 = (dbg_listType==1) ? ((0x808c002du & ~(7u<<24)) | (2u<<24)) : 0x808c002du;
        const uint32_t CHIP_TSP6 = 0x20080440u;
        auto emitChip6 = [&](float x0,float x1,float y0,float y1,float z,uint32_t argb){
            if (dbg_colOverride) argb = dbg_colARGB;
            wU32(out, CHIP_PCW6); wU32(out, HUD_ISP); wU32(out, CHIP_TSP6); wU32(out, HUD_TCW);
            wU32(out, 0); wU32(out, 0); wU32(out, 0); wU32(out, 0);
            emitVtx6(0xE0000000u, x1, y1, z, argb);   // BR
            emitVtx6(0xE0000000u, x0, y1, z, argb);   // BL
            emitVtx6(0xE0000000u, x1, y0, z, argb);   // TR
            emitVtx6(0xF0000000u, x0, y0, z, argb);   // TL (ends strip)
            emittedTris += 2;
        };

        // FORCE TEST QUAD (bisect emit-vs-render): a big bright opaque quad at NEAR depth,
        // center-screen. If this shows but the bars don't -> bar geometry/state wrong; if even
        // this is invisible -> the emit/TA-structure/list is broken. (Uses emitQuad so it shares
        // the exact synth control words; color-override still applies if set.)
        if (dbg_forceTest)
            emitQuad(220.f, 420.f, 200.f, 280.f, 0.f, 0.0090f, 0xFFFF00FFu);   // bright magenta
        auto lerp8 = [](uint8_t a, uint8_t b, float t)->uint8_t{
            float v = (float)a + ((float)b-(float)a)*t; if(v<0)v=0; if(v>255)v=255; return (uint8_t)(v+0.5f); };
        auto mkARGB = [](int r,int g,int b)->uint32_t{
            return 0xFF000000u | ((uint32_t)(r&0xFF)<<16) | ((uint32_t)(g&0xFF)<<8) | (uint32_t)(b&0xFF); };

        // ---- TEAM COLORS (loc_8c15FFB0): active point char C1/C2/C3 -> magenta/green/cyan.
        //      [bright HP inner-stop, light highlight].  gradOuter = warm yellow (outer anchor). ----
        struct RGB { int r,g,b; };
        static const RGB TEAM_HP[3]  = { {255,64,255}, {60,255,60}, {51,200,255} };  // C1/C2/C3 bright
        static const RGB TEAM_HI[3]  = { {255,156,255},{187,255,170},{174,235,255} }; // highlight
        static const RGB FRAME_RGB   = {26,29,36};     // dark empty channel (#1a1d24)
        // (GRAD_WARM/CHIP_RGB removed — superseded by the byte-exact ARGB stops below.)

        // ---- BYTE-EXACT engine gouraud stops (ARGB8888), for the Type-6 fill strip. These are the
        //      loc_8c15FFB0 table words after the engine's intensity map (channel c -> c*254/255 trunc:
        //      ff->fe, 40->3f, c0->bf, 00->00), CONFIRMED against the live HUDQ oracle
        //      (_hud_cap/hudq_inventory.txt): C1 inner fefe3ffe / outer fefefe00; C3/super inner
        //      fe00bffe. Alpha 0xFE on the fill (matched). Indexed by pointColIdx C1/C2/C3.
        //      (re_kb finding:hud_p2a_bars_already_byteexact + source:hudq_bar_meter_bytetarget.)
        static const uint32_t TEAM_HP_ARGB[3] = { 0xfefe3ffeu, 0xfe00fe00u, 0xfe00bffeu }; // magenta/green/cyan inner
        static const uint32_t GRAD_WARM_ARGB  = 0xfefefe00u;   // engine outer gouraud anchor (yellow, A=0xfe)
        static const uint32_t CHIP_ARGB       = 0xfffe0000u;   // engine red chip/drain (flat, A=0xff)

        // active point-char slot index (0/1/2 within the side) -> team color idx; -1 if none active.
        auto pointColIdx = [&](const int* slots)->int{
            for (int i=0;i<3;i++) if (rd8(CHAR_BASE[slots[i]]+0x000)) return i;
            return -1;
        };

        // ---- LIFE BARS: 3 rows per side, active-first order (row0=active point on top). ----
        // MEASURED STG0B fill rects (P1 anchored LEFT, P2 anchored RIGHT; flat, skew 0):
        //   row0 y[46..58] x P1[46.6..269.5] P2[370.5..593.4]   (w~223 — the point-char bar)
        //   row1 y[74..82] x P1[48.6..200.7] P2[439.3..591.4]   (w~152 — reserve)
        //   row2 y[94..102]x P1[48.6..200.7] P2[439.3..591.4]   (w~152 — reserve)
        // `z` = the MEASURED per-row HUD-plane 1/W (bake pos[2] == mirror vertex Z, B1): life row0
        // (cy52)=0.00768, rows1/2 (cy78/98)=0.00525. The fill inherits its co-located backing's Z.
        struct BarRect { float x0,x1,y0,y1,z; };
        static const BarRect P1BARS[3] = {
            {46.6f,269.5f,46.0f,57.7f,0.00768f}, {48.6f,200.7f,74.3f,82.3f,0.00525f}, {48.6f,200.7f,94.3f,102.3f,0.00525f} };
        static const BarRect P2BARS[3] = {
            {370.5f,593.4f,46.0f,57.7f,0.00768f}, {439.3f,591.4f,74.3f,82.3f,0.00525f}, {439.3f,591.4f,94.3f,102.3f,0.00525f} };

        auto drawSide = [&](const int* slots, const BarRect* bars, bool p1){
            int col = pointColIdx(slots); if (col < 0) col = 0;
            const RGB& hi = TEAM_HI[col];   // highlight sheen (HP inner stop now via TEAM_HP_ARGB)
            // active-first row order: active point char on row0, then the other two in slot order.
            int order[3]; int n=0, rest[3], nr=0;
            for (int i=0;i<3;i++){ if (rd8(CHAR_BASE[slots[i]]+0x000)) order[n++]=slots[i]; else rest[nr++]=slots[i]; }
            for (int i=0;i<nr;i++) order[n++]=rest[i];
            for (int row=0; row<3; row++){
                const BarRect& b = bars[row];
                int slot = order[row];
                float hpF   = (float)rd8(CHAR_BASE[slot]+0x420) / (float)HP_MAX;
                float chipF = (float)rd8(CHAR_BASE[slot]+0x424) / (float)HP_MAX;   // recoverable red
                if (hudDiag) hpF = 0.25f;                    // HUD_DIAG=1 reach test
                if (hpF<0)hpF=0; if (hpF>1)hpF=1; if (chipF<0)chipF=0; if (chipF>1)chipF=1;
                if (chipF < hpF) chipF = hpF;                // chip never shorter than live HP
                float fullW = b.x1 - b.x0;
                float outer = p1 ? b.x0 : b.x1;              // portrait-side anchor
                float zBar  = b.z + Z_BIAS;                  // this row's HUD-plane depth (nearer than backing)
                auto innerX = [&](float frac){ return p1 ? (outer + fullW*frac) : (outer - fullW*frac); };
                // PER-LAYER DEPTH STEP (the "invisible bar" root cause). Every quad here is in the
                // OPAQUE list with ISP DepthCompare=GREATER + ZWrite ON (isp=0x93400000). Stacking
                // the frame->chip->fill->highlight layers at the SAME z means each layer's GREATER
                // test vs the one below is `z > z` = FALSE -> only the FIRST (dark frame) quad draws
                // and every colored fill/chip on top is depth-REJECTED -> the bar shows as an empty
                // dark channel = "invisible". MEASURED in the dumped fr.ta: 6+ synth quads all at
                // z=0.00771 over the bar center. FIX: step each successive layer slightly NEARER
                // (larger z) so it passes GREATER over the previous layer. Step is tiny (well under
                // the ~0.0002 gap to the next HUD row) but resolvable at this z magnitude (~0.0077).
                const float ZL = 0.00002f;   // per-layer nearer step
                int _layer = 0;
                auto zLayer = [&](){ return zBar + (float)(_layer++) * ZL; };
                // 1) dark empty channel (full width) -- BOTTOM layer
                emitQuad(b.x0, b.x1, b.y0, b.y1, 0.f, zLayer(), mkARGB(FRAME_RGB.r,FRAME_RGB.g,FRAME_RGB.b));
                // 2) recoverable-red chip TRAIL behind (outer -> chipF)  [live: hudChipOn]
                //    BYTE-EXACT: engine chip is its OWN Type-6 strip (pcw 0x808c002d / tsp 0x20080440
                //    / flat col 0xfffe0000), NOT emitQuad+mkARGB. Corners: outer edge -> chipF inner.
                if (dbg_chipOn && chipF > 0.001f) {
                    float zChip = zLayer();
                    float xl = p1 ? outer : innerX(chipF), xr = p1 ? innerX(chipF) : outer;
                    emitChip6(xl, xr, b.y0, b.y1, zChip, CHIP_ARGB);
                }
                // 3) bright HP fill ON TOP (outer -> hpF) as the ENGINE-EXACT gouraud strip.
                //    MEASURED (engine 0x808c001f fill): a single Type-6 4-vertex strip with per-
                //    vertex FLOAT ARGB gouraud — OUTER corners = warm yellow (R=1,G=1,B=0,A=1),
                //    INNER corners = team color (e.g. R=1,G=0.25,B=1,A=1). We emit exactly that via
                //    emitStrip6 (engine PCW/verts/offset-color). hudGradientOn=0 -> flat team color.
                if (hpF > 0.001f) {
                    float zFill = zLayer();
                    float xOuter = outer, xInner = innerX(hpF);
                    // per-corner ARGB (BYTE-EXACT engine gouraud stops, A=0xFE): outer=warm yellow
                    // fefefe00 (or team stop if gradient off), inner=team stop TEAM_HP_ARGB[col].
                    uint32_t cInner = TEAM_HP_ARGB[col];
                    uint32_t cOuter = dbg_gradientOn ? GRAD_WARM_ARGB : cInner;
                    // map outer/inner (which is left/right depending on side) to TL/TR/BR/BL.
                    // P1: outer=left(x0side), inner=right. P2: outer=right, inner=left.
                    float xl = p1 ? xOuter : xInner, xr = p1 ? xInner : xOuter;
                    uint32_t cl = p1 ? cOuter : cInner, cr = p1 ? cInner : cOuter;
                    // emitStrip6(x0,x1,y0,y1,z, cTL,cTR,cBR,cBL): left col = cl, right col = cr.
                    emitStrip6(xl, xr, b.y0, b.y1, zFill, cl, cr, cr, cl);
                    // 4) thin top highlight sheen (upper ~1/3 of the fill span)  [live: hudHighlightOn]
                    if (dbg_highlightOn) {
                        float hy1 = b.y0 + (b.y1-b.y0)*0.34f;
                        emitQuad(outer, innerX(hpF), b.y0, hy1, 0.f, zLayer(), mkARGB(hi.r,hi.g,hi.b));
                    }
                }
            }
        };
        if (dbg_barsOn) { drawSide(P1_SLOTS, P1BARS, true); drawSide(P2_SLOTS, P2BARS, false); }

        // ---- SUPER METER: multi-level. meter_level u8 (0x28964A P1/0x28964B P2)=stocked levels
        //      0..5; meter_fill u16 (0x289646/648)=current-level partial 0..METER_MAX(144). The
        //      3 meter fill-bar rows are the level bars; we light `level` of them FULL + the next
        //      one to fill/144, team-tinted. NOTE: these engine meter bands (0x9e1/2/3xx P1,
        //      0x9e7/8/9xx P2) render in the TOP HUD area (cy 66/89/109) interleaved with the life
        //      bars — that is the real MVC2 top-HUD layout, NOT a bottom-of-screen bar. Positions
        //      below are the EXACT measured engine mirror rects (_live_hud, per-band pixel-exact).
        //      Per-row Z (measured bake: row0 cy66=0.00794, rows1/2 cy89/109=0.00588).
        static const BarRect P1MET[3] = {
            {39.7f,183.8f,61.9f,70.9f,0.00794f}, {44.5f,151.3f,85.3f,91.9f,0.00588f}, {44.5f,151.3f,105.4f,112.0f,0.00588f} };
        static const BarRect P2MET[3] = {
            {455.8f,600.7f,61.9f,70.9f,0.00794f}, {488.4f,595.8f,85.3f,91.9f,0.00588f}, {488.4f,595.8f,105.4f,112.0f,0.00588f} };
        static const float   P1MET_SKEW[3] = { 4.4f, 3.3f, 3.3f };
        static const float   P2MET_SKEW[3] = { -4.4f, -3.3f, -3.3f };
        const float METER_MAX = 144.f;

        auto drawMeter = [&](const int* slots, const BarRect* mrows, const float* mskew,
                             bool p1, uint32_t levelAddr, uint32_t fillAddr){
            int col = pointColIdx(slots); if (col < 0) col = 0;
            const RGB& mc = TEAM_HP[col]; const RGB& mh = TEAM_HI[col];
            int   level = rd8(levelAddr); if (level > 3) level = 3;   // 3 baked meter rows available
            float part  = (float)rd16(fillAddr) / METER_MAX;
            if (hudDiag) part = 0.25f;
            if (part < 0) part = 0; if (part > 1) part = 1;
            for (int row=0; row<3; row++){
                const BarRect& b = mrows[row]; float sk = mskew[row];
                float fullW = b.x1 - b.x0;
                float outer = p1 ? b.x0 : b.x1;
                float zBar  = b.z + Z_BIAS;
                auto innerX = [&](float frac){ return p1 ? (outer + fullW*frac) : (outer - fullW*frac); };
                // PER-LAYER DEPTH STEP (same OPAQUE GREATER+ZWrite tie fix as the life bars):
                // empty channel -> fill -> highlight must each step slightly nearer or the fill/
                // highlight get depth-rejected over the channel and the meter shows empty.
                const float ZLM = 0.00002f; int _ml = 0;
                auto zMet = [&](){ return zBar + (float)(_ml++) * ZLM; };
                // dark empty channel (bottom layer)
                emitQuad(b.x0, b.x1, b.y0, b.y1, sk, zMet(), mkARGB(21,24,30));
                // this row's fill fraction: full if below the stocked level, partial at the level,
                // empty above. (row is a whole level segment; higher rows = higher levels.)
                float f = (row < level) ? 1.f : (row == level ? part : 0.f);
                if (f > 0.001f) {
                    // BYTE-EXACT: the engine meter FILL bar is the SAME white-texel gouraud strip as
                    // the life bar (pcw 0x808c001f / tsp 0x20880440 / tcw 0x08080000, inner=team stop
                    // / outer=warm yellow), FLAT (no skew) — CONFIRMED HUDQ oracle quads [14]/[20]/
                    // [26]/[32]. The segmented notch ART (fmt=5 PAL4 tcw 2a489xxx) is a SEPARATE quad
                    // family fixed by the VRAM dirty-page agent, not here.
                    float zFill = zMet();
                    float xInner = innerX(f);
                    uint32_t cInner = TEAM_HP_ARGB[col];
                    uint32_t cOuter = dbg_gradientOn ? GRAD_WARM_ARGB : cInner;
                    float xl = p1 ? outer : xInner, xr = p1 ? xInner : outer;
                    uint32_t cl = p1 ? cOuter : cInner, cr = p1 ? cInner : cOuter;
                    emitStrip6(xl, xr, b.y0, b.y1, zFill, cl, cr, cr, cl);
                    // highlight sheen
                    float hy1 = b.y0 + (b.y1-b.y0)*0.4f;
                    emitQuad(outer, innerX(f), b.y0, hy1, sk, zMet(), mkARGB(mh.r,mh.g,mh.b));
                }
            }
        };
        if (dbg_meterOn) {
            drawMeter(P1_SLOTS, P1MET, P1MET_SKEW, true,  0x28964Au, 0x289646u);
            drawMeter(P2_SLOTS, P2MET, P2MET_SKEW, false, 0x28964Bu, 0x289648u);
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
