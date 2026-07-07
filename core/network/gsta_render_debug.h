// gsta_render_debug.h — LIVE-tunable render-debug globals for the native GSTA client.
//
// A set of atomics the GSTA emit path (gsta_stage.cpp gstaStageEmitTA + the body/HUD emit in
// maplecast_mirror.cpp gstaApplyFrame + the render-thread clientReceiveGsta) reads EVERY FRAME.
// Driven live over the control WebSocket (:7211) from web/gsta-render-debug.html via the
// hud_get / hud_set commands (maplecast_control_ws.cpp) — so render iteration needs NO rebuild.
//
// Design: EVERY knob defaults to the shipping behavior (zero visual change when untouched).
// Ints are used for all values (JSON numbers round-trip cleanly); bit-flag/enum knobs documented.
#pragma once
#include <atomic>
#include <cstdint>

namespace gsta_render_debug {

struct Globals {
    // ============================ WHOLE-FRAME / TA-LIST ============================
    std::atomic<int> stageOn{1};          // emit stage geometry (floor/skybox + baked frames)
    std::atomic<int> bodyOn{1};           // emit body/scene TA (render_frame output)
    std::atomic<int> hudSynthOn{1};       // emit the procedural HUD synthesis pass
    std::atomic<int> hudqOn{1};           // emit the legacy HUDQ-reshape pass (gstaBuildHudTA)
    // Per-TA-LIST isolation (applied at render-thread: skip a whole list's params on parse).
    std::atomic<int> listOpaqueOn{1};
    std::atomic<int> listPunchOn{1};
    std::atomic<int> listTransOn{1};
    // SOLO mode: 0=off; 1=stage only; 2=body only; 3=HUD synth only (overrides the on/off above).
    std::atomic<int> soloMode{0};

    // ============================ BODY (render_frame) =============================
    std::atomic<int> bodyEffectsOn{1};    // emit bit15 EFFECT quads (supers/projectiles)
    std::atomic<int> bodySatellitesOn{1}; // emit cat 1..4 SATELLITE quads (capes/drones/assists)
    std::atomic<int> bodyCull85xxx{1};    // run the never-engine 0x85xxx block cull (re_kb/51)
    std::atomic<int> bodyEffectTcwFix{1}; // run the per-frame effect-TCW post-pass (re_kb/50)
    std::atomic<int> bodyForceColorOn{0}; // tint ALL body quads one color (isolate body coverage)
    std::atomic<uint32_t> bodyForceColorARGB{0xFF00FF00u};   // default bright green

    // ============================ STAGE ===========================================
    std::atomic<int> stageReproject{1};   // reproject world-authored meshes through the live camera
    std::atomic<int> stageFramesOn{1};    // emit baked HUD FRAMES/plates band (0x9be00)
    std::atomic<int> stageForceColorOn{0};// tint ALL stage quads one color (isolate stage coverage)
    std::atomic<uint32_t> stageForceColorARGB{0xFF3366FFu};  // default blue
    // Baked-dynamic-fill drop (procedural synthesis replaces them). 1 = drop baked life+meter fills.
    std::atomic<int> filterBakedDyn{1};

    // ============================ HUD SYNTH =======================================
    std::atomic<int> hudBarsOn{1};        // procedural life bars
    std::atomic<int> hudMeterOn{1};       // procedural super meter
    std::atomic<int> hudFramesOn{1};      // (mirrors stageFramesOn for the panel) baked frames band
    std::atomic<int> hudGradientOn{1};    // life-bar warm->team gradient (0 = flat team color)
    std::atomic<int> hudChipOn{1};        // red_health chip trail
    std::atomic<int> hudHighlightOn{1};   // top-edge highlight sheen
    // fill-color override (test): force ALL synth fills to this ARGB (bypass team/gradient/chip).
    std::atomic<int> fillColorOverrideOn{0};
    std::atomic<uint32_t> fillColorARGB{0xFFFF0000u};        // default bright red
    std::atomic<int> zBiasMicro{30};      // per-row nearer Z bias, ×1e-6 (default 0.00003)
    std::atomic<int> posDX{0};            // synth HUD screen X offset (px)
    std::atomic<int> posDY{0};            // synth HUD screen Y offset (px)
    std::atomic<int> forceTestQuad{0};    // giant bright quad at near depth (bisect emit vs render)
    std::atomic<int> hudListType{0};      // synth quads list type: 0=OPAQUE, 1=TRANSLUCENT
};

Globals& g();

// Apply a hud_set key=value from the control WS. Returns true if the key is recognized.
bool setKey(const char* key, double value);

// Serialize all globals into (key, value) pairs for hud_get (panel initial sync).
void forEach(void (*emit)(void* ctx, const char* key, double value), void* ctx);

} // namespace gsta_render_debug
