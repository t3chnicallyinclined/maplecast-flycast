// gsta_stage.h — native MVC2 STAGE/BACKGROUND renderer for the GSTA client.
//
// Faithful C++ port of web/webgpu/stage-client.mjs _buildFromTA (the grounded
// engine-TA bake path, re_kb finding:stage_pernode_matrices_closed /
// replica_live_stage_black_fix). Loads atlas/stages/STGxx_ta.json (a bake of the
// engine's OWN stage TA: real PCW/ISP/TSP/TCW + per-vertex screen `pos`, un-projected
// `world`, `uv`, `rgb`), re-projects the world-authored meshes through the LIVE camera
// (XMTRX = M1.M2, transform_object_122560), and emits an OPAQUE polygon-TA pass that
// flycast's renderer draws BEHIND the translucent body sprites (OP -> PT -> TR).
//
// Stage TEXTURES are already resident in the seeded vram[] at each mesh TCW TexAddr
// (the GSTA prefix ships the full 8MB engine VRAM), so no PNG decode is needed natively.
#pragma once
#include <stdint.h>
#include <vector>
#include <cstddef>

#ifdef MAPLECAST_GSTA_CLIENT_BUILD

// Global-namespace functions; maplecast_mirror.cpp calls them with the ::gstaStage*
// qualifier (it is inside namespace maplecast_mirror).

// Load the stage bake for `stageId` (the wire stage_id @0x8C289638). Resolves the
// STGxx_ta.json via the stage_id map + a base dir (MAPLECAST_GSTA_STAGE_DIR env or a
// default). Cheap no-op if the same stage is already loaded. Returns true if a stage is
// ready to emit. Thread: WS thread only.
bool gstaStageEnsureLoaded(uint32_t stageId);

// Append the stage's OPAQUE polygon-TA (ParaType=4 polygons, ListType=0) to `out`,
// re-projecting world-authored meshes through the live camera matrices M1 (16 f32,
// col-major) and M2 (16 f32). `out` is the TA byte stream being assembled; the stage
// MUST be emitted BEFORE the body sprite list so the FSM opens the OP list first.
// `ram` = the seeded 16MB area-3 image (_gstaRam.data()); used to RESHAPE the baked HUD
//   life-bar / super-meter fill meshes by LIVE state (health/meter), so the engine's
//   pixel-exact baked HUD geometry depletes from the per-frame wire state (zero bandwidth,
//   no HUDQ dependency). May be null (then HUD fill meshes emit baked / un-reshaped).
// Returns the number of bytes appended (0 if no stage / not loaded).
size_t gstaStageEmitTA(std::vector<uint8_t>& out, const float* M1, const float* M2,
                       const uint8_t* ram = nullptr);

// True once a stage bake is loaded and has emittable geometry.
bool gstaStageReady();

#endif // MAPLECAST_GSTA_CLIENT_BUILD
