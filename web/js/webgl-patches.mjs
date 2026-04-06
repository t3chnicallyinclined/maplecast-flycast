// ============================================================================
// WEBGL-PATCHES.MJS — WebGL2 context monkey-patches
//
// MUST be imported BEFORE WASM renderer loads. Patches enable/disable to
// filter invalid GL enums (GL_FOG, GL_ALPHA_TEST) that WebGL2 rejects.
// Uses Set-based cap filtering instead of try/catch for zero V8 deopt.
// ============================================================================

import { state } from './state.mjs';

const origGetContext = HTMLCanvasElement.prototype.getContext;
HTMLCanvasElement.prototype.getContext = function(type, attrs) {
  const ctx = origGetContext.call(this, type, attrs);
  if (ctx && (type === 'webgl2' || type === 'webgl') && !ctx._patched) {
    state.glCtx = ctx;
    ctx._patched = true;

    // Valid WebGL2 enable/disable caps — Set.has() is O(1), zero overhead
    const VALID = new Set([ctx.BLEND, ctx.CULL_FACE, ctx.DEPTH_TEST, ctx.DITHER,
      ctx.POLYGON_OFFSET_FILL, ctx.SAMPLE_ALPHA_TO_COVERAGE, ctx.SAMPLE_COVERAGE,
      ctx.SCISSOR_TEST, ctx.STENCIL_TEST, ctx.RASTERIZER_DISCARD]);

    const origEnable = ctx.enable.bind(ctx);
    ctx.enable = (cap) => { if (VALID.has(cap)) origEnable(cap); };

    const origDisable = ctx.disable.bind(ctx);
    ctx.disable = (cap) => { if (VALID.has(cap)) origDisable(cap); };

    const origGetError = ctx.getError.bind(ctx);
    ctx.getError = () => { const err = origGetError(); return err === ctx.INVALID_ENUM ? ctx.NO_ERROR : err; };

    const origTexParameteri = ctx.texParameteri.bind(ctx);
    ctx.texParameteri = (t, p, v) => { if (p !== 0x84FE) origTexParameteri(t, p, v); };

    const origTexParameterf = ctx.texParameterf.bind(ctx);
    ctx.texParameterf = (t, p, v) => { if (p !== 0x84FE) origTexParameterf(t, p, v); };

    console.log('[webgl] Patches installed — Set-based cap filtering, zero try/catch');
  }
  return ctx;
};
