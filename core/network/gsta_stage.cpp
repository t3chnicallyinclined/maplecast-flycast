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
#include "../deps/json/json.hpp"

using json = nlohmann::json;

// ---- TA emit layout constants (match core/hw/pvr/ta.cpp ta_handle_cmd FSM) ----
// Polygon global param (ParaType 4) is 32B: PCW, ISP, TSP, TCW, then 4 reserved/face words.
// Vertex (ParaType 7) for Polygon Type 3 (Col_Type=0, Textured, UV32) is 32B:
//   PCW(0xE0..), xyz[3] f32, u f32, v f32, BaseCol u32, OffsCol u32 (=TA_Vertex3).
// We force obj_ctrl = Texture only (0x08) so the FSM selects vertex type 3 regardless of
// the engine mesh's original Col_Type — we always carry a packed ARGB8888 base colour.
static constexpr float SCREEN_W = 640.f, SCREEN_H = 480.f;
static constexpr float MARGIN   = 800.f;     // viewport slack for the degenerate-tri cull

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

    // base dir: env override, else a few sensible defaults relative to the cwd / exe.
    std::string base;
    if (const char* e = std::getenv("MAPLECAST_GSTA_STAGE_DIR")) base = e;
    const char* candBases[] = { base.c_str(), "atlas/stages",
                                "../atlas/stages", "../../atlas/stages" };
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
    printf("[GSTA-STAGE] loaded STG%02X (engine-TA grounded): %zu meshes, %zu tris, hasWorld=%d\n",
           fileIdx & 0xFF, g_stage.meshes.size(), triTotal, (int)g_stage.hasWorld);
    return g_stage.ready;
}

bool gstaStageReady() { return g_stage.ready; }

// =============================================================================
// EMIT: append the stage OPAQUE polygon-TA, re-projected through the live camera.
// =============================================================================
size_t gstaStageEmitTA(std::vector<uint8_t>& out, const float* M1, const float* M2)
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

    size_t startBytes = out.size();
    size_t emittedTris = 0;

    for (auto& m : g_stage.meshes) {
        if (m.verts.empty()) continue;
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
            // resolve final screen verts first so we can reject the whole tri atomically
            float fx[3], fy[3], fz[3]; bool ok = true;
            for (int k=0;k<3;k++) {
                float px = tv[k]->pos[0], py = tv[k]->pos[1], pz = tv[k]->pos[2];
                if (reproject && tv[k]->hasWorld) {
                    projectEngine(X, tv[k]->world[0], tv[k]->world[1], tv[k]->world[2], px, py, pz);
                }
                if (!std::isfinite(px) || !std::isfinite(py)
                    || px < -MARGIN || px > SCREEN_W + MARGIN
                    || py < -MARGIN || py > SCREEN_H + MARGIN) { ok = false; break; }
                fx[k]=px; fy[k]=py; fz[k]=pz;
            }
            if (!ok) continue;

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
