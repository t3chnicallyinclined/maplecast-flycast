/* gsta_render_frame.h — C++-visible interface to the transpiled MVC2 render path
 * compiled in gsta_render_frame.c. Mirrors the PoC's Sh4Ctx (sh4ctx.h) + SceneQuad
 * (render_frame.c) layout EXACTLY — these structs are passed across the C/C++ boundary,
 * so the field order/types here MUST stay in lockstep with the PoC headers.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* === Sh4Ctx — must match tools/render-replica-poc/sh4ctx.h byte-for-byte. The only
 * field the GSTA client sets is .ram (the flat 16MB area-3 image render_frame reads). === */
typedef struct {
    uint32_t r[16];
    float    fr[16];
    float    xf[16];
    uint32_t fpscr, fpul, pr, macl, mach, sr_t, gbr;
    uint32_t _pool;
    uint8_t *ram;
} GstaSh4Ctx;

/* === SceneQuad — must match render_frame.c. One emitted body tile (PVR2 sprite). === */
typedef struct {
    uint32_t pcw, isp, tsp, tcw, recidx;
    float    Ax, Ay, Bx, By, Cx, Cy, Dx, Dy, u1;
    uint32_t sel;       /* GFX1 cell sel (decode key) */
    uint32_t gfx1;      /* owning node GFX1 base (node+0x15C) */
    uint32_t mirror;    /* texU mirror bit (facing XOR per-part 0x4000) */
    uint32_t facing;    /* owning body facing (node+0x110) */
    float    z;         /* per-object PVR depth (node+0xE8 = 1/w) */
} GstaSceneQuad;

/* Slot-walk the seeded RAM, render every on-screen BODY into the scene accumulator. */
void render_frame(GstaSh4Ctx *c);
int  render_frame_nscene(void);
const GstaSceneQuad* render_frame_scene(void);
uint32_t render_frame_body_count(void);

/* Per-quad metadata for the body texture decode (M3). */
unsigned int gsta_quad_colrow(int* out_cr, unsigned int cap);
unsigned int gsta_quad_mirror(unsigned char* out_m, unsigned int cap);

#ifdef __cplusplus
}
#endif
