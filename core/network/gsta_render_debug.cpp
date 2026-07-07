// gsta_render_debug.cpp — see gsta_render_debug.h.
#include "gsta_render_debug.h"
#include <cstring>
#include <cstdint>

namespace gsta_render_debug {

Globals& g() { static Globals s; return s; }

bool setKey(const char* key, double v) {
    Globals& s = g();
    auto I = [&](std::atomic<int>& a){ a.store((int)(v + (v<0?-0.5:0.5)), std::memory_order_relaxed); return true; };
    auto U = [&](std::atomic<uint32_t>& a){ a.store((uint32_t)(int64_t)v, std::memory_order_relaxed); return true; };
    // whole-frame / list
    if (!std::strcmp(key,"stageOn"))              return I(s.stageOn);
    if (!std::strcmp(key,"bodyOn"))               return I(s.bodyOn);
    if (!std::strcmp(key,"hudSynthOn"))           return I(s.hudSynthOn);
    if (!std::strcmp(key,"hudqOn"))               return I(s.hudqOn);
    if (!std::strcmp(key,"listOpaqueOn"))         return I(s.listOpaqueOn);
    if (!std::strcmp(key,"listPunchOn"))          return I(s.listPunchOn);
    if (!std::strcmp(key,"listTransOn"))          return I(s.listTransOn);
    if (!std::strcmp(key,"soloMode"))             return I(s.soloMode);
    // body
    if (!std::strcmp(key,"bodyEffectsOn"))        return I(s.bodyEffectsOn);
    if (!std::strcmp(key,"bodySatellitesOn"))     return I(s.bodySatellitesOn);
    if (!std::strcmp(key,"bodyCull85xxx"))        return I(s.bodyCull85xxx);
    if (!std::strcmp(key,"bodyEffectTcwFix"))     return I(s.bodyEffectTcwFix);
    if (!std::strcmp(key,"bodyForceColorOn"))     return I(s.bodyForceColorOn);
    if (!std::strcmp(key,"bodyForceColorARGB"))   return U(s.bodyForceColorARGB);
    // stage
    if (!std::strcmp(key,"stageReproject"))       return I(s.stageReproject);
    if (!std::strcmp(key,"stageFramesOn"))        return I(s.stageFramesOn);
    if (!std::strcmp(key,"stageForceColorOn"))    return I(s.stageForceColorOn);
    if (!std::strcmp(key,"stageForceColorARGB"))  return U(s.stageForceColorARGB);
    if (!std::strcmp(key,"filterBakedDyn"))       return I(s.filterBakedDyn);
    // hud synth
    if (!std::strcmp(key,"hudBarsOn"))            return I(s.hudBarsOn);
    if (!std::strcmp(key,"hudMeterOn"))           return I(s.hudMeterOn);
    if (!std::strcmp(key,"hudFramesOn"))          return I(s.hudFramesOn);
    if (!std::strcmp(key,"hudGradientOn"))        return I(s.hudGradientOn);
    if (!std::strcmp(key,"hudChipOn"))            return I(s.hudChipOn);
    if (!std::strcmp(key,"hudHighlightOn"))       return I(s.hudHighlightOn);
    if (!std::strcmp(key,"fillColorOverrideOn"))  return I(s.fillColorOverrideOn);
    if (!std::strcmp(key,"fillColorARGB"))        return U(s.fillColorARGB);
    if (!std::strcmp(key,"zBiasMicro"))           return I(s.zBiasMicro);
    if (!std::strcmp(key,"posDX"))                return I(s.posDX);
    if (!std::strcmp(key,"posDY"))                return I(s.posDY);
    if (!std::strcmp(key,"forceTestQuad"))        return I(s.forceTestQuad);
    if (!std::strcmp(key,"hudListType"))          return I(s.hudListType);
    return false;
}

void forEach(void (*emit)(void* ctx, const char* key, double value), void* ctx) {
    Globals& s = g();
    #define E(k) emit(ctx, #k, (double)s.k.load(std::memory_order_relaxed))
    E(stageOn); E(bodyOn); E(hudSynthOn); E(hudqOn);
    E(listOpaqueOn); E(listPunchOn); E(listTransOn); E(soloMode);
    E(bodyEffectsOn); E(bodySatellitesOn); E(bodyCull85xxx); E(bodyEffectTcwFix);
    E(bodyForceColorOn); E(bodyForceColorARGB);
    E(stageReproject); E(stageFramesOn); E(stageForceColorOn); E(stageForceColorARGB);
    E(filterBakedDyn);
    E(hudBarsOn); E(hudMeterOn); E(hudFramesOn); E(hudGradientOn); E(hudChipOn); E(hudHighlightOn);
    E(fillColorOverrideOn); E(fillColorARGB); E(zBiasMicro); E(posDX); E(posDY);
    E(forceTestQuad); E(hudListType);
    #undef E
}

} // namespace gsta_render_debug

// ---- plain-C shims so the transpiled render_frame.c (amalgamation, C not C++) can read the
//      body-render knobs. Default (all 1 / cull on) == shipping behavior. ----
extern "C" int gsta_dbg_body_effects_on(void){ return gsta_render_debug::g().bodyEffectsOn.load(std::memory_order_relaxed); }
extern "C" int gsta_dbg_body_satellites_on(void){ return gsta_render_debug::g().bodySatellitesOn.load(std::memory_order_relaxed); }
extern "C" int gsta_dbg_body_cull_85xxx(void){ return gsta_render_debug::g().bodyCull85xxx.load(std::memory_order_relaxed); }
extern "C" int gsta_dbg_body_effect_tcw_fix(void){ return gsta_render_debug::g().bodyEffectTcwFix.load(std::memory_order_relaxed); }
