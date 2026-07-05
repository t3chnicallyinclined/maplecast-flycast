/* wasm_entry_frame.c — PHASE 2 EMSCRIPTEN entry: render the WHOLE FRAME (all on-screen
 * BODY objects) from a flat 16MB SH4 RAM image.
 *
 *   uint32_t render_frame_ta(uint8_t* ram16mb, uint8_t* out_ta, uint32_t out_cap)
 *
 * Runs the transpiled root slot-walk (render_sprites_0308c2 = loc_8c0308c2) over the
 * caller's ram16mb: it enumerates every BODY node in the slot table (count@0x8C2895E0,
 * ptrs@0x8C287DE0, cat@+0x3==0), renders each via render_object_full (Phase-1 chain:
 * setup->transform->scale->walker->submit, with the cursor-derived rectab base), and
 * accumulates every body tile into one full-scene PVR2 sprite TA written to out_ta.
 *
 * Unlike wasm_entry.c (single object from a baked image), THIS consumes the real passed-in
 * RAM — so it scales to N bodies with no baked descriptors. The per-object allocation base
 * advances via the running cursor (= the engine's node+0xDC prefix-sum), validated
 * byte-exact in render_frame_test.exe (params 9/9, corners 0.00px, 100% pixel match).
 *
 * Build: see build_wasm_frame.sh (emcc gen_walker_root.c render_frame.c gen_render_object.c
 * gen_transform_obj.c gen_submit_params.c gen_walker.c gen_leaf.c wasm_entry_frame.c).
 */
#include "sh4ctx.h"
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

void render_frame(Sh4Ctx *c);
typedef struct {
    u32 pcw, isp, tsp, tcw, recidx;
    float Ax,Ay,Bx,By,Cx,Cy,Dx,Dy, u1, v1;
    u32 sel;                 /* SOURCE GFX1 cell sel for this tile (per-quad, tiling-safe) */
    u32 gfx1;                /* owning node's GFX1 base (node+0x15C) — decode key with sel */
    u32 mirror;              /* texU mirror bit = facing XOR per-part 0x4000 (loc_8c0346c4) */
    u32 facing;             /* owning body facing (node+0x110) */
    float z;                 /* per-object PVR depth = node+0xE8 = 1/w (MUST match render_frame.c) */
} SceneQuad;
int  render_frame_nscene(void);
const SceneQuad* render_frame_scene(void);
extern int g_body_count;

/* ---- PVR2 paraType=5 sprite writer (LE) — same byte format the project's
 * converge_full_computed.mjs / converge_frame.mjs feed ta-parser.mjs. ---- */
static u16 h16(float f){ u32 u; memcpy(&u,&f,4); return (u>>16)&0xFFFF; }

EXPORT
uint32_t render_frame_ta(uint8_t* ram16mb, uint8_t* out_ta, uint32_t out_cap){
    static Sh4Ctx c;
    memset(&c,0,sizeof c);
    c.ram = ram16mb;                 /* the caller's real 16MB area-3 RAM image */

    render_frame(&c);                /* slot-walk -> all bodies into the scene accumulator */

    int n = render_frame_nscene();
    const SceneQuad* S = render_frame_scene();

    u32 o = 0;
    for(int k=0;k<n;k++){
        if(o + 96 > out_cap) break;
        const SceneQuad* q=&S[k];
        u8* p = out_ta + o; memset(p,0,96);
        #define W32(off,v) do{ u32 _v=(v); p[off]=_v; p[off+1]=_v>>8; p[off+2]=_v>>16; p[off+3]=_v>>24; }while(0)
        #define WF(off,f)  do{ float _f=(f); u32 _u; memcpy(&_u,&_f,4); W32(off,_u); }while(0)
        W32(0,q->pcw); W32(4,q->isp); W32(8,q->tsp); W32(12,q->tcw);
        /* Sprite BASE COLOR (TA_Sprite global param +16, "sprite_base_color"). MUST be
         * opaque white: MVC2 body sprites use shadInstr=MODULATE (TSP bits6-7=3) so the
         * shader does c = faceColor * texColor. A zero base color (the memset default)
         * zeroes every fragment -> c.a<0.004 -> discard -> NOTHING DRAWS. 0xFFFFFFFF is the
         * modulate identity (the engine's own sprite base color for these tiles). */
        W32(16,0xFFFFFFFFu);                         /* sprite base color (opaque white) */
        W32(32,0xE0000000u);                         /* sprite vtx PCW */
        /* per-VERTEX depth z = q->z = the owning object's node+0xE8 (1/w). The engine
         * submits Az=Bz=Cz=this for every body tile (probe2 PC 0x0C1248CC: Az/Bz/Cz all
         * ~0.00924). Emitting the real per-object z (instead of the old constant 1.0) is
         * what lets pvr2-renderer's translucent depth-write + back-to-front sort occlude
         * cape vs body vs projectile correctly (DEPTH FIX 2026-06-14). */
        WF(36,q->Ax); WF(40,q->Ay); WF(44,q->z);
        WF(48,q->Bx); WF(52,q->By); WF(56,q->z);
        WF(60,q->Cx);
        WF(64,q->Cy); WF(68,q->z);
        WF(72,q->Dx); WF(76,q->Dy);
        { float U=q->u1, V=q->v1;
          /* texU MIRROR (engine loc_8c0346c4 neg-r8): when facing XOR per-part 0x4000, swap
           * the left/right U so the tile draws horizontally mirrored. uLo/uHi default 0..U;
           * mirrored -> U..0. The carve writes STORAGE-order pixels (facing-independent); this
           * mirror is the ONLY place the L/R flip is applied — exactly like the engine. */
          float uLo = q->mirror ? U : 0.0f;
          float uHi = q->mirror ? 0.0f : U;
          u16 v0=h16(V),u0=h16(uLo),v1=h16(V),u1=h16(uHi),v2=h16(0.0f),u2=h16(uHi);
          p[84]=v0;p[85]=v0>>8; p[86]=u0;p[87]=u0>>8;
          p[88]=v1;p[89]=v1>>8; p[90]=u1;p[91]=u1>>8;
          p[92]=v2;p[93]=v2>>8; p[94]=u2;p[95]=u2>>8; }
        #undef W32
        #undef WF
        o += 96;
    }
    if(o + 32 <= out_cap){ memset(out_ta+o,0,32); o+=32; } /* EndOfList */
    return o;
}

EXPORT uint32_t render_frame_body_count(void){ return (uint32_t)g_body_count; }
extern int g_sat_count;
/* cat 1..4 satellites that actually emitted body tiles this frame (the missing-sprites fix). */
EXPORT uint32_t render_frame_sat_count(void){ return (uint32_t)g_sat_count; }
EXPORT uint32_t render_frame_quad_count(void){ return (uint32_t)render_frame_nscene(); }

/* PER-QUAD SOURCE SEL (tiling-safe texture pairing). The body walker expands ONE GFX2 cell
 * record into N tiles (N = desc tile count), all sharing the cell's GFX1 sel. The emitted TA
 * therefore has MORE quads than cell records, so the client must NOT pair quad[i]<->sel[i] 1:1
 * (that slips after the first tiled cell -> right colors, wrong quad = the scramble). This
 * fills out_sels[k] = the GFX1 sel the walker actually used for TA quad k (k in render order,
 * == ensureBodyTextures' qcur). The client decodes THAT sel's sprite to quad k's TCW. The sels
 * are u16 in MVC2's namespace; we write them as u16 LE. Returns the number written. */
EXPORT uint32_t render_frame_quad_sels(uint16_t* out_sels, uint32_t cap){
    int n = render_frame_nscene();
    const SceneQuad* S = render_frame_scene();
    uint32_t w = 0;
    for(int k=0;k<n && w<cap;k++,w++) out_sels[w] = (uint16_t)S[k].sel;
    return w;
}

/* PER-QUAD OWNING-BODY GFX1 BASE. So the client decodes each quad's sel against the RIGHT
 * character's GFX1 (no slot-attribution / run-length re-derivation needed — fully tiling-proof:
 * a tiled cell's N tiles carry the SAME (sel,gfx1), so they decode the same sprite to N TCWs).
 * out_gfx1[k] = the GFX1 base (a P1/P0 RAM pointer) the walker's body node used for TA quad k. */
EXPORT uint32_t render_frame_quad_gfx1s(uint32_t* out_gfx1, uint32_t cap){
    int n = render_frame_nscene();
    const SceneQuad* S = render_frame_scene();
    uint32_t w = 0;
    for(int k=0;k<n && w<cap;k++,w++) out_gfx1[w] = S[k].gfx1;
    return w;
}


/* PER-QUAD INTRA-PART TILE (col,row) — wide-part carve key (re_kb
 * finding:wide_part_tile_storage_order_v2 + finding:per_side_facing_fix). out_cr[2*q]=col
 * (FACING-INDEPENDENT STORAGE column), out_cr[2*q+1]=row. col = rank of the per-tile screen
 * Ax DESCENDING for facing==0 / ASCENDING for facing==1 (the storage-col->screenX direction
 * REVERSES with facing — finding:per_side_storage_col_reverses), so the carve always slices
 * the same fixed storage chunk; the L/R flip is the texU mirror alone (render_frame_quad_mirror).
 * row = Ay-desc (facing only reflects X). */
uint32_t render_frame_quad_colrow_impl(int* out_cr, uint32_t cap);
EXPORT uint32_t render_frame_quad_colrow(int* out_cr, uint32_t cap){
    return render_frame_quad_colrow_impl(out_cr, cap);
}

/* PER-QUAD texU MIRROR (facing XOR per-part 0x4000). out_m[k]=0/1. The client carve writes
 * storage-order pixels; this bit tells the renderer to mirror U so the L/R flip matches the
 * engine (loc_8c0346c4). Exposed so replay.html can A/B the mirror live. */
uint32_t render_frame_quad_mirror_impl(uint8_t* out_m, uint32_t cap);
EXPORT uint32_t render_frame_quad_mirror(uint8_t* out_m, uint32_t cap){
    return render_frame_quad_mirror_impl(out_m, cap);
}

/* PER-QUAD SOURCE DESCRIPTOR [m, cx, ry, flags] x quadCount (4 bytes/quad; flags bit0=valid,
 * bit1=per-record flip4000 — storage columns pair DESCENDING), snapshotted at EMIT time from
 * the walker's own DESC_TABLE entry (clobber-proof vs the shared-scratch rebuild overwrite).
 * The carve key that SUPERSEDES rank colrow (2026-07-05 satellite-fragmentation fix — global
 * Ax/Ay ranks merge/interleave when 2+ satellite nodes draw the same (gfx1,sel) per frame).
 * body_decoder.mjs ensureBodyTextures consumes this in lockstep with the native client. */
uint32_t render_frame_quad_srcdesc_impl(uint8_t* out, uint32_t cap);
EXPORT uint32_t render_frame_quad_srcdesc(uint8_t* out, uint32_t cap){
    return render_frame_quad_srcdesc_impl(out, cap);
}

/* PER-QUAD bit15 effect tag — needed by the client's palette-repoint discriminator to
 * exclude effect quads (was defined in render_frame.c but never exported to the wasm). */
uint32_t render_frame_quad_is_effect_impl(uint8_t* out_e, uint32_t cap);
EXPORT uint32_t render_frame_quad_is_effect(uint8_t* out_e, uint32_t cap){
    return render_frame_quad_is_effect_impl(out_e, cap);
}
