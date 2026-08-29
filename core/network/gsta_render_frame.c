/* gsta_render_frame.c — amalgamation TU that compiles the byte-exact transpiled
 * MVC2 render path (tools/render-replica-poc) into the native GSTA client.
 *
 * The render-replica PoC is a set of plain-C source files over a flat uint8_t* RAM
 * (sh4ctx.h, LITTLE-ENDIAN, area-3). They cross-reference each other via extern
 * declarations and are individually byte-exact (params 16/16, corners 0.00px, 100%
 * pixel match — re_kb finding:render_replica_phase1_codederived). We #include them ALL
 * into ONE translation unit here so:
 *   - the project's CMake only lists THIS file (no per-file build wiring), and
 *   - every cross-TU symbol resolves locally (no link order concerns).
 *
 * Each PoC function is DEFINED exactly once across these files (render_frame.c owns the
 * SceneQuad/PolyParam types + the leaf_e2e0/e860 stubs; gen_leaf.c owns leaf_e460;
 * gen_*.c own their one routine each), so the amalgamation has no duplicate definitions.
 *
 * The C++ side (maplecast_mirror.cpp, gsta client section) calls these via the
 * extern "C" prototypes in gsta_render_frame.h:
 *   void render_frame(Sh4Ctx*); int render_frame_nscene(void);
 *   const SceneQuad* render_frame_scene(void); + the per-quad sel/gfx1/colrow/mirror getters.
 *
 * NOTE: the POC dir is added as an include dir in core/network/CMakeLists.txt so
 * "sh4ctx.h" (and the .c paths below) resolve.
 */

/* The PoC root + all transpiled routines. The POC dir is added to this file's
 * include path in core/network/CMakeLists.txt (COMPILE_OPTIONS -I), so these
 * bare names + the "sh4ctx.h" inside them resolve. */
#include "render_frame.c"        /* defines PolyParam, TileCap, SceneQuad + the root */
#include "gen_walker_root.c"
#include "gen_render_object.c"
#include "gen_render_satellite.c"
#include "gen_transform_obj.c"
#include "gen_walker.c"
#include "gen_walker_scale.c"   /* bit15 SCALE walker (loc_8c0348c8) — effect-node dispatch (re_kb/50) */
/* render_frame.c already typedef'd PolyParam — suppress the identical one in
 * gen_submit_params.c so the single amalgamation TU has no C2371 redefinition. */
#define GSTA_POLYPARAM_DEFINED
#include "gen_submit_params.c"
#include "gen_leaf.c"

/* ---- per-quad metadata getters (the wasm_entry_frame.c *_impl wrappers, but plain so
 * the C++ client reads them without the EMSCRIPTEN entry). render_frame.c defines the
 * _impl functions; expose thin C-callable shims here. ---- */
unsigned int render_frame_quad_colrow_impl(int* out_cr, unsigned int cap);
unsigned int render_frame_quad_mirror_impl(unsigned char* out_m, unsigned int cap);
unsigned int render_frame_quad_is_effect_impl(unsigned char* out_e, unsigned int cap);
unsigned int render_frame_quad_srcdesc_impl(unsigned char* out, unsigned int cap);

unsigned int gsta_quad_colrow(int* out_cr, unsigned int cap){ return render_frame_quad_colrow_impl(out_cr, cap); }
unsigned int gsta_quad_mirror(unsigned char* out_m, unsigned int cap){ return render_frame_quad_mirror_impl(out_m, cap); }
unsigned int gsta_quad_is_effect(unsigned char* out_e, unsigned int cap){ return render_frame_quad_is_effect_impl(out_e, cap); }
/* per-quad [m,cx,ry,flags] source-descriptor snapshot (emit-time, clobber-proof) — the
 * carve key that supersedes rank colrow (render_frame.c render_frame_quad_srcdesc_impl). */
unsigned int gsta_quad_srcdesc(unsigned char* out, unsigned int cap){ return render_frame_quad_srcdesc_impl(out, cap); }

/* render_frame_body_count() lives in wasm_entry_frame.c (the EMSCRIPTEN entry, not
 * amalgamated here); g_body_count is the underlying global (render_frame.c). Re-expose it. */
extern int g_body_count;
unsigned int render_frame_body_count(void){ return (unsigned int)g_body_count; }

/* ============================================================================
 * ABI-STABLE render entry — takes ONLY the RAM base pointer, never the fragile
 * Sh4Ctx struct across the FFI boundary. Construct the ctx HERE (C-side), where
 * sh4ctx.h's real layout is authoritative.
 *
 * WHY THIS EXISTS (2026-07-25, native-client render_frame crash):
 *   sh4ctx.h's Sh4Ctx is 240 bytes with `ram` at offset 232 (it carries sr_q/sr_m,
 *   the div0s/div1 bits used by the game-tick divide idiom). The native client's
 *   Rust mirror struct (native-client-tdw-exec/src/ffi.rs GstaSh4Ctx) had drifted
 *   out of sync — it was MISSING sr_q/sr_m, so it was 232 bytes with `ram` at
 *   offset 224. The client wrote the RAM pointer at 224; render_frame read c->ram
 *   from 232 (8 bytes past the field, past the end of the 232-byte struct) -> a
 *   garbage pointer -> ACCESS VIOLATION on the FIRST guest memory access, every
 *   frame. The offline C harness never hit it (it uses the real Sh4Ctx, ram@232).
 *   Rust's `size_of::<GstaSh4Ctx>() == 232` compile assert passed because it only
 *   checks Rust's own size, so the mismatch shipped silently.
 *
 * Passing only the pointer removes the entire struct-ABI-drift failure class: the
 * one field that crosses the boundary is a bare `uint8_t*`. Callers should use this
 * instead of building a ctx and calling render_frame() directly.
 * ==========================================================================*/
#include <string.h>
void render_frame_ram(unsigned char *ram){
    Sh4Ctx c;
    memset(&c, 0, sizeof c);
    c.ram   = ram;
    c.r[15] = 0x8CFF0000u;   /* same seed the offline harnesses use */
    render_frame(&c);
}
