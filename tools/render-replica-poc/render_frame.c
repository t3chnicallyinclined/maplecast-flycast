/* PHASE 2 — render_frame(ram): walk the slot table and render EVERY on-screen BODY
 * object, with the per-object rectab allocation base COMPUTED by running the engine's
 * own submit-allocation cursor (NOT RAM-discovered).
 *
 * THE CURSOR MODEL — fully traced from marvelous2 (3 confirming sites), the load-bearing
 * Phase-2 correctness piece:
 *
 *   Arena base   *(0x8C1F9D94) = 16 or 400  (per-frame ping-pong by frame parity;
 *                set in the frame-setup loc_8c033950, bank03:8460-8469). This is the
 *                START index into idxtab for the frame's first body object.
 *   Per-object   loc_8c033b0a (bank03:8751-8772) deposits  node+0xDC = *(0x8C1F9D98)
 *                (snapshot the running cursor), then per GFX2 record advances
 *                *(0x8C1F9D98) += record_tile_count  (loc_8c033d44, bank03:8057-8059:
 *                `r3=*r11; r3+=r9; *r11=r3`, r11=*(0x8C1F9D98), r9=count). So across the
 *                frame, node+0xDC = PREFIX-SUM of all prior bodies' tile counts.
 *   Render walk  loc_8c0344d4 (bank03:10325-10330): the per-tile alloc index
 *                *r13[k] = node+0xDC + *(0x8C1F9D94) + k, incremented +1/tile (10752).
 *   Submit       loc_8C1244B0 (bank12:9853-9874): rectab[ idxtab[*r13] ].
 *
 *   => For body object n, tile k:  idxtab_index = prefix_sum(n) + arena_base + k
 *      where prefix_sum(n) = Σ ntiles(0..n-1)  and arena_base = *(0x8C1F9D94).
 *
 * In the full walk the cursor advances NATURALLY as we render objects in slot order:
 * render_frame keeps a `running_cursor` (= the engine's node+0xDC), seeded 0, advanced
 * by each object's emitted tile count. The engine's resident node+0xDC is the GROUND
 * TRUTH for this prefix-sum, so we ASSERT computed prefix == resident node+0xDC per
 * object — the proof the cursor generalizes Phase-1's single object to all objects.
 *
 * NOTE on validation scope: the matched dump (mc_ram_dump.bin / mc_engine_ta.bin) is a
 * single-character-on-screen frame — only cid23/P2C1 (Cable, 9 tiles, pal=24) is in the
 * slot table (L06) and the engine TA. So the byte-exact diff covers ONE real body; the
 * MULTI-object cursor advance is proven by (a) the resident node+0xDC == computed prefix
 * check, and (b) a synthetic 2-body test (render_frame_test.c) that confirms object 2's
 * base = object 1's base + object 1's ntiles per the ROM formula.
 */
#include "sh4ctx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Phase-1 pieces (reused verbatim) ---- */
void render_object_setup_03093c(Sh4Ctx *c);          /* gen_render_object.c   */
void render_object_setup_030af8(Sh4Ctx *c);          /* gen_render_satellite.c (cat 1..4) */
void transform_object_122560(Sh4Ctx *c, u32 node);   /* gen_transform_obj.c   */
void walker_0344d4(Sh4Ctx *c);                       /* gen_walker.c          */
void walker_0348c8(Sh4Ctx *c);                       /* gen_walker_scale.c (bit15 scale walker) */
static u32 s_running_cursor;                          /* fwd (tentative); defined below */

/* ============================================================================
 * EFFECT idxtab REMAP (re_kb/50 — char-pass-transient idxtab). The bit15 SCALE walker
 * (loc_8c0348c8) resolves its per-record TCW via rectab[idxtab[alloc_index]]. The idxtab
 * EFFECT entries are written by the char-pass submit and reverted by the HUD pass, so the
 * wire ships them STALE -> they point at BODY rectab entries -> wrong (body) texture.
 * MEASURED (live ASMTRACE 0x8C034BA4 + _live_fx6): the FRESH rectab DOES hold the effect
 * textures in a CONTIGUOUS block (effect-band TCWs 0x89000..0x8bfff), and the engine maps
 * the frame's contiguous effect index range 1:1 onto it. So the CORRECT entry for an effect
 * alloc_index = effect_block_start + (idx - frame_min_effect_idx). The remap is done as a
 * PER-FRAME POST-PASS (render_frame_fix_effect_tcws) using the PER-FRAME min — NOT a match-
 * global min, which drifts across different supers (a later super with lower indices lands
 * quads BELOW the block in the 0x85xxx band = the floating gray/white BLOCKS during motion).
 * VALIDATED OFFLINE vs the 7200 mirror on _live_fx6: effect TCWs match (0 fabricated) across
 * all super frames incl distinct supers. Effect quads only; body quads untouched.
 * ==========================================================================*/

/* Scan the resident rectab for the start of the contiguous effect-TCW (0x89000..0x8bfff)
 * block. Returns the entry index, or 0 if none. Effect textures are the lightning/super
 * sprites; the block is contiguous (engine allocates it in one run). */
static u32 find_effect_block_start(Sh4Ctx *c){
    u32 rectab = r32(c, 0x8C2DAD4C);
    if(((rectab>>24)&0x7Fu) != 0x0Cu) return 0;
    int run = 0, start = -1;
    for(u32 e = 0; e < 2048; e++){
        u32 tcw = r32(c, rectab + e*0x20 + 0x0C) & 0x1FFFFF;
        if(tcw >= 0x89000u && tcw < 0x8C000u){ if(run==0) start=(int)e; run++; }
        else { if(run >= 8) break; run=0; start=-1; }
    }
    return (run >= 8 && start >= 0) ? (u32)start : 0;
}
typedef struct { u32 pcw, isp, tsp, tcw; } PolyParam;
void submit_params(Sh4Ctx *c, u32 rec_index, u32 palbank, PolyParam *out); /* gen_submit_params.c */

/* leaves the walker links against */
void leaf_e460(Sh4Ctx*);
/* SIN/COS LEAVES (loc_8c11e860 = sin, loc_8c11e2e0 = cos) — TRANSPILED SEMANTICS 2026-07-02
 * (finding:effect_sincos_leaves). These were STUBBED to no-ops, so fr0 held stale garbage on the
 * scale walker's ROTATED effect path (node+0x104=0x8000 = a 180deg-rotated bolt): the anchor
 * fr15 + cos*fr14 (gen_walker_scale.c:602) ran away to ~10000px = the last 9 Lightning-Storm bolts.
 * DISASM (marvelous2 bank11.asm loc_8C11E860): the input ANGLE is r4 as a 16-bit unsigned
 * (extu.w r4), 65536 == a full circle; the routine computes angle_rad = (r4/65536)*2*PI (constants
 * 0x40C90FDb=2*PI, 0x47800000=65536, 0x3FC90FDb=PI/2, 0x3F000000=0.5) via a poly sin, returning
 * fr0. loc_8C11E2E0 (cos) range-reduces r4 then falls through to sin (cos(x)=sin(x+90deg)). Host
 * sinf/cosf reproduce the poly to far under 0.5px. Return fr0 = trig(angle) * fr4 (fr4 = radius/1.0). */
#include <math.h>
void leaf_e860(Sh4Ctx*c){                         /* sin */
    u32 ang = c->r[4] & 0xFFFFu;                   /* extu.w r4 — 16-bit angle, 65536=2*PI */
    float rad = (float)ang * (6.2831853071795862f / 65536.0f);
    c->fr[0] = sinf(rad);
}
void leaf_e2e0(Sh4Ctx*c){                         /* cos (= sin shifted a quarter circle) */
    u32 ang = c->r[4] & 0xFFFFu;
    float rad = (float)ang * (6.2831853071795862f / 65536.0f);
    c->fr[0] = cosf(rad);
}
void helper_1294bc(Sh4Ctx*c){ (void)c; }

/* ============================================================================
 * Per-object emitted-quad capture (the walker calls submit_1244b0 per tile with
 * r4 = the per-tile stack record @(r15+0x2C); screenX@+0x04, screenY@+0x08 = the
 * BOTTOM-left anchor). We also need the per-tile ALLOC INDEX *r13 = *(r15+0x2C),
 * captured here so render_frame can map tile k -> rectab without re-deriving it.
 * ==========================================================================*/
#define MAXQ 256
/* per-tile capture. `flip4000` = the GFX2 cell record's per-part 0x4000 X-mirror flag (read
 * from the cell flags u16 @ r11+0x4). The body's facing (node+0x110) is attached per-object
 * in render_object_full_ex (one facing per object). texU mirror = facing XOR (flip4000!=0),
 * the engine's loc_8c0346c4 `neg r8` gate (re_kb finding:field_semantics_from_setter /
 * routine:loc_8c0346c4). */
typedef struct { float bx, by; u32 alloc_index; u32 sel; u32 flip4000;
                 /* EFFECT sel: the scale walker loc_8c0348c8 reads its per-record GFX1 sel from
                  * *(r13+6) (gen_walker_scale.c:265), NOT *(r11+6) like the body walker. Captured
                  * here so the effect W/H can index GFX1[efx_sel] for the logical dims. */
                 u32 efx_sel; } TileCap;
static TileCap g_cap[MAXQ];
static int g_ncap = 0;

/* ---- SHIP-RESOLVED-BODY-TCW (2026-07-03, finding:ship_resolved_body_tcw) ----
 * The engine's RESOLVED per-tile body tcw (r12+0x0C at the submit) is STABLE; render_frame's own
 * rectab[idxtab[alloc_index]] resolution FLIPS every frame with the arena_base parity (16<->400)
 * -> the frame-to-frame texture bounce. The headless captures the resolved tcw per (body node, tile
 * index in render order) and ships it (BTCW wire tail). render_frame_set_body_tcws() hands us that
 * buffer; for BODY tiles we look up (node, k) and use the shipped tcw VERBATIM, bypassing the
 * parity-sensitive table lookup. Layout: per body [u32 node(0x0C..)][u32 ntiles][ntiles u32 tcw]. */
#ifdef BODYTCW_DEBUG
#include <stdio.h>
#endif
static const u32* g_bodyTcwBuf = 0;
static int        g_bodyTcwWords = 0;
void render_frame_set_body_tcws(const u32* buf, int nWords){
    g_bodyTcwBuf = (buf && nWords > 0) ? buf : 0;
    g_bodyTcwWords = (buf && nWords > 0) ? nWords : 0;
#ifdef BODYTCW_DEBUG
    fprintf(stderr,"[BTSET] buf=%p nWords=%d -> g_words=%d\n", (void*)buf, nWords, g_bodyTcwWords);
#endif
}
/* Look up the shipped resolved tcw for (node, tileIdx). Returns 1 + writes *out on hit, else 0.
 * node is the render_frame node base (may be 0x8C.. or 0x0C..); the wire keys on the 0x0C.. alias. */
#ifdef BODYTCW_DEBUG
static int g_btDbg = 0;
#endif
static int body_tcw_lookup(u32 node, int tileIdx, u32* out){
    if(!g_bodyTcwBuf) {
#ifdef BODYTCW_DEBUG
        if(g_btDbg++ < 3) fprintf(stderr,"[BTDBG] buf NULL (node=0x%08X words=%d)\n", node, g_bodyTcwWords);
#endif
        return 0;
    }
    u32 key = (node & 0x0FFFFFFFu) | 0x0C000000u;   /* normalize to the 0x0C.. alias the wire uses */
    int i = 0;
#ifdef BODYTCW_DEBUG
    if(key==0x0C268340u && tileIdx==0) fprintf(stderr,"[BTDBG] CHARBODY lookup node=0x%08X key=0x%08X words=%d buf0=0x%08X buf1=%u\n",
        node, key, g_bodyTcwWords, g_bodyTcwBuf[0], g_bodyTcwBuf[1]);
#endif
    while(i + 2 <= g_bodyTcwWords){
        u32 bnode = g_bodyTcwBuf[i];
        u32 nt    = g_bodyTcwBuf[i+1];
        if(((bnode & 0x0FFFFFFFu) | 0x0C000000u) == key){
            if(tileIdx >= 0 && (u32)tileIdx < nt && (i + 2 + tileIdx) < g_bodyTcwWords){
                *out = g_bodyTcwBuf[i + 2 + tileIdx];
                return 1;
            }
            return 0;   /* body found but tile index out of range -> no override (fall back) */
        }
        i += 2 + (int)nt;   /* skip to next body record */
    }
    return 0;
}

void submit_1244b0(Sh4Ctx *c){
    u32 r4 = c->r[4];                 /* = r15+0x2C, the per-tile stack record */
    /* SOURCE CELL SEL for this tile. The walker (gen_walker.c loc_8c0344d4) reads the
     * GFX1 part selector ONCE per GFX2 cell record at `mov.w @(0x6,r11),r0` and then emits
     * `count = u8(desc[r13+1])+1` tiles for that ONE cell via this submit — r11 is NOT
     * advanced inside the inner tile loop (r11+=8 happens only after the inner loop, at
     * loc_8c03488e). So at EVERY submit call c->r[11]+6 == the current cell's sel, shared
     * by all of that cell's tiles. Capturing it here lets the client decode the RIGHT
     * sprite per quad instead of walking sels 1:1 with quads (which slips under tiling). */
    u32 cell_sel = r16u(c, c->r[11] + 0x6);
    /* ---- PER-RECORD sel==0xFF -> EMIT NO QUAD (re_kb finding:replica_effect_overemit_sel_ff) ----
     * A GFX2 cell record whose sel field (@(r11+6)) == 0xFF is an authored BLANK/padding record:
     * the engine renders ZERO visible tiles for it but STILL advances the per-tile descriptor
     * cursor (r13) and the per-record cursors past its allocated slots, so the records AFTER it
     * read the correct (in-phase) descriptor entries. EMPIRICAL GROUND TRUTH (prod ASMTRACE
     * PC 0x8C034864, _efxdiag.mcrr): node 0c268340 vf295515 -> recs 0..7 = 21 tiles then rec 8
     * sel=0xFF -> 0 tiles; node 0c2688e4 vf295439 -> rec 12 sel=0xFF -> 0 tiles yet recs 13..16
     * (sels 230,256,260,258) render normally and IN PHASE. The faithful walker (gen_walker.c) is
     * left UNTOUCHED so it advances every cursor exactly as the engine does; we suppress only the
     * VISIBLE quad capture here. Suppressing in the walker (forcing count 0) wrongly froze r13 and
     * phase-shifted later records by 1 tile each (256:2->1, 260:1->2, 258:2->1) — over/under-emit.
     * Without ANY guard the 0xFF record tiled into stray quads (e.g. +4 [255,255,255,255]) that
     * shifted every later shared-gfx1 group quad -> the user-visible "effects missing/misplaced".
     * 487 node-frames in the capture carry a mid-stream sel==0xFF. */
    if((cell_sel & 0xFFFFu) == 0xFFu) return;   /* blank record: no quad, cursors already advanced */
    /* per-part X-mirror flag: GFX2 cell record flags u16 @ r11+0x4, bit 0x4000 (the per-part
     * texture-U mirror authored into the cell). XORed with the body facing at draw time. */
    u32 cell_flip = (r16u(c, c->r[11] + 0x4) & 0x4000u) ? 1u : 0u;
    /* ROBUSTNESS (live multi-object): g_cap holds at most MAXQ tiles. On a normal body
     * the walker emits ~9-40 tiles; a STALE/corrupt GFX2 (e.g. a body whose art was not
     * resident at the streamed prefix-build, or a per-frame region the read-set under-ships)
     * can drive the walker's in-RAM record loop into a runaway, emitting thousands of tiles.
     * We must NOT count past MAXQ — render_object_full() reads g_cap[k] for k<ntiles, so an
     * unbounded g_ncap caused OOB reads of g_cap[] AND filled g_scene to its 1024 cap with
     * garbage quads (the "quads=1024 Ax=0" frames). Clamping the COUNT here bounds both. */
    if(g_ncap < MAXQ){
        u32 bx = r32(c, r4 + 0x04);
        u32 by = r32(c, r4 + 0x08);
        g_cap[g_ncap].bx = *(float*)&bx;
        g_cap[g_ncap].by = *(float*)&by;
        g_cap[g_ncap].alloc_index = r32(c, r4 + 0x00);  /* *r13 = stack[r15+0x2C] */
        g_cap[g_ncap].sel = cell_sel;                    /* the cell's GFX1 sel (per-quad) */
        g_cap[g_ncap].flip4000 = cell_flip;              /* per-part 0x4000 X-mirror flag */
        /* EFFECT sel from *(r13+6) (scale walker loc_8c0348c8:265) — used ONLY on the effect
         * path for the GFX1-logical W/H lookup. For the body walker r13 is the tiledesc cursor
         * (not a record), so this value is meaningless there and simply unused (guarded by _is_effect). */
        g_cap[g_ncap].efx_sel = (u32)(u16)r16u(c, c->r[13] + 0x6);
        g_ncap++;                       /* only advance while in-bounds: ntiles<=MAXQ */
    }
    /* else: drop the tile (over-read guard). A real body never exceeds MAXQ; reaching it
     * means this object's geometry source is corrupt — emitting it would garble the scene. */
}

/* ---- arena-control globals (traced) ---- */
#define G_ARENA_BASE  0x8C1F9D94u   /* *(.) = 16 or 400 (frame ping-pong base)     */
#define G_OBJ_CURSOR  0x8C1F9D98u   /* *(.) = running prefix-sum, snapshotted to +0xDC */

static float rf(Sh4Ctx*c, u32 a){ u32 w=r32(c,a); return *(float*)&w; }

/* ============================================================================
 * render_object_full(c, node) — the Phase-1 per-object body render, now driven
 * with the CURSOR-DERIVED allocation base (node+0xDC + arena_base), and writing
 * its emitted sprite quads into the global scene TA accumulator (g_scene_*).
 * Returns the number of body tiles it emitted (so the caller advances the cursor).
 * Honors the +0x12C visibility gate via render_object_setup_03093c (which `bra`s to
 * skip when node+0x12C==0 — in that case the walker emits 0 tiles and we add 0).
 * ==========================================================================*/

/* the running scene TA: each tile -> one fully-computed sprite descriptor */
typedef struct {
    u32 pcw, isp, tsp, tcw, recidx;
    float Ax,Ay,Bx,By,Cx,Cy,Dx,Dy, u1, v1;
    u32 sel;                 /* SOURCE GFX1 cell sel for this tile (per-quad, tiling-safe) */
    u32 gfx1;                /* owning node's GFX1 base (node+0x15C) — decode key with sel */
    u32 mirror;              /* texU mirror bit = facing XOR per-part 0x4000 (loc_8c0346c4) */
    u32 facing;             /* owning body facing (node+0x110); carve storage-col key */
    float z;                 /* per-object PVR depth = node+0xE8 = 1/w (the persp-divide
                              * reciprocal deposited by transform_object_122560 /
                              * loc_8C122560). The engine submits THIS as every vertex's z
                              * (probe2: Az=Bz=Cz=0.00924 per part). Drives the translucent
                              * back-to-front sort + depth-write occlusion in pvr2-renderer.
                              * WAS hardcoded 1.0 -> all objects flattened to one plane ->
                              * cape-through-body / random overlap (DEPTH FIX 2026-06-14). */
} SceneQuad;
#define MAXSCENE 1024
static SceneQuad g_scene[MAXSCENE];
static int g_nscene = 0;
static u8  g_scene_is_effect[MAXSCENE];   /* 1 = bit15 effect quad (post-pass re-resolves TCW) */
/* PER-QUAD SOURCE-DESC snapshot (emit-time; see the capture in render_object_full_ex and
 * render_frame_quad_srcdesc_impl): m px / storage cx / (rows-row) / flags(valid|flip4000<<1). */
static u8  g_scene_src_m [MAXSCENE];
static u8  g_scene_src_cx[MAXSCENE];
static u8  g_scene_src_ry[MAXSCENE];
static u8  g_scene_src_fl[MAXSCENE];

/* palette bank for a node: slot formula 16*(char_pair+1)+8*player_side. We derive it
 * from the node's char-struct identity when it is one of the 6 fighter bodies; for a
 * generic body node we default to the P2C1 bank used by the validated object. (The
 * palbank only affects the TCW PalSelect inject; resident TCW already carries it.) */
static const struct { u32 base; u32 palbank; } CHAR_SLOT[6] = {
    {0x8C268340u,16},{0x8C2688E4u,24},{0x8C268E88u,32},
    {0x8C26942Cu,40},{0x8C2699D0u,48},{0x8C269F74u,56},
};
static u32 palbank_for(u32 node){
    for(int i=0;i<6;i++) if(CHAR_SLOT[i].base==node) return CHAR_SLOT[i].palbank;
    return 24; /* default (the validated P2C1 object) */
}

/* TILE_M (tile pixel size m = descriptor byte0) per emitted tile — read straight from
 * the resident 0x8C1F9F9C descriptor table the walker already consumes for geometry.
 * The walker reads r13 = *(node+0xDC)*4 + 0x8C1F9F9C; descriptor byte0 = m. We mirror
 * that here so the screen tile extent W=m*scaleX, H=m*scaleY matches the walker. */
#define DESC_TABLE 0x8C1F9F9Cu

/* ============================================================================
 * rebuild_tile_grid — GFX1-DERIVED tile-grid regeneration (the racy-tiledes fix).
 *
 * ROOT CAUSE: the per-frame descriptor table @0x8C1F9F9C (built by the engine's
 * loc_8c033ba8->loc_8c033ce0 from the RESIDENT GFX1 part headers) ships TORN/STALE
 * over the wire — a prior pose's slice bleeds in. The body walker loc_8c0344d4 is
 * FAITHFUL: it reads count + per-tile (col,row) steps from that table and emits one
 * tile per entry. On a torn slice an 8x8 part (physically ONE 8x8 tile) carries a
 * stale 8-tile count with a marching Y column -> the walker faithfully lays 8 tiles
 * marching off-screen (the "column past cy 480" garble). The walker is right; its
 * INPUT is stale.
 *
 * FIX: before running the walker, REGENERATE this body's descriptor slice from the
 * RESIDENT GFX1 headers (which ARE pose-correct — resident, not per-frame torn),
 * reproducing the engine builder byte-for-byte:
 *   sel=u16(rec+6); hdr=GFX1base+u32(GFX1base+sel*4); sw=u8(hdr+2), sh=u8(hdr+3)
 *   W=sw*8, H=sh*8; d=min(W,H); m=(d==8?8:d==16?16:32)      (bank03 loc_8c033be0)
 *   cols=W/m, rows=H/m, count=cols*rows   (== engine (W*H/2)/{0x20,0x80,0x200})
 *   per-tile [0]=m, [1]=count-1, [2]=col, [3]=rows-row, emitted in the engine's
 *   2-row-band twiddle order (by 2-row band from top; col mid; row inner).
 * VALIDATED byte-exact (m,count,col,row all 4 bytes) vs the shipped table on the
 * fully-clean frames f96/f98 (46/46 records, 0 mismatch), and == the dominant
 * (clean) shipped value across 850+ frames per sel (_genval/_stale probes); on torn
 * frames it REPLACES the stale slice with the pose-correct one. Refs: bank03
 * loc_8c033ba8..loc_8c033ce0; re_kb finding:inner_tile_loop_descriptor (22),
 * finding:tiling_geometry_byte_faithful (17). Body-only (bit15 effects use the
 * separate scale-walker path). This feeds BOTH the walker position pen AND the
 * per-tile extent read (render_object_full_ex m=u8(DESC_TABLE+(dc+k)*4)). */
static void rebuild_tile_grid(Sh4Ctx *c, u32 node){
    u32 gfx2 = r32(c, node+0x160);
    u32 gfx1 = r32(c, node+0x15C);
    if(!gfx2 || !gfx1) return;
    u32 sid  = r16u(c, node+0x144) & 0x7FFFu;
    u32 dc   = r16u(c, node+0xDC);
    u32 cell = r32(c, gfx2 + (sid<<2)) + gfx2;
    u32 nrec = r16u(c, cell);
    if(nrec==0 || nrec>256) return;          /* guard against a corrupt cell */
    u32 rec  = cell + 2;
    u32 idx  = dc;                            /* tile cursor into DESC_TABLE (== walker seed) */
    for(u32 r=0; r<nrec; r++, rec+=8){
        u32 sel = r16u(c, rec+6);
        if((sel & 0xFFFFu) == 0xFFu) continue;   /* blank record: emits no tile (walker skips 0xFF) */
        u32 off = r32(c, gfx1 + (sel<<2));
        u32 hdr = gfx1 + off;
        u32 sw = r8u(c, hdr+2), sh = r8u(c, hdr+3);
        u32 W = sw*8u, H = sh*8u;
        if(W==0 || H==0) continue;
        u32 d = (W<H)?W:H;
        u32 m = (d==8u)?8u : (d==16u)?16u : 32u;
        u32 cols = W/m, rows = H/m;
        u32 cnt  = cols*rows; if(cnt==0) cnt=1;
        if(idx + cnt > 768u) return;             /* table is 768 entries; never overrun it */
        u32 t = 0;
        for(u32 by=0; by<rows; by+=2){
            u32 bh = (rows - by < 2u) ? (rows - by) : 2u;
            for(u32 cx=0; cx<cols; cx++){
                for(u32 ry=0; ry<bh; ry++){
                    u32 row = by + ry;
                    u32 a = DESC_TABLE + (idx + t)*4u;
                    w8(c, a+0, (u8)m);
                    w8(c, a+1, (u8)(cnt-1));
                    w8(c, a+2, (u8)cx);
                    w8(c, a+3, (u8)(rows - row));
                    t++;
                }
            }
        }
        idx += cnt;
    }
}

/* +0xDC budget-table helpers (defined below with the g_dc_table state) — forward-declared
 * so render_object_full_ex can clamp the body tile count to its arena budget. */
static void dc_table_reset(void);
static void dc_table_add(Sh4Ctx *c, u32 node);
static u32  dc_budget(Sh4Ctx *c, u32 node);

/* render_object_full_ex: the shared per-object body render. `is_sat` selects which engine
 * setup routine deposits the per-frame walker fields:
 *   cat==0 BODY      -> render_object_setup_03093c (loc_8c03093c "Render Main Sprite")
 *   cat 1..4 SATELLITE -> render_object_setup_030af8 (loc_8c030af8, bank03:1526)
 * EVERYTHING after the setup is IDENTICAL: the satellite emits its sprite through the SAME
 * body walker loc_8c0344d4 reading the SAME deposited node fields (+0xE0/E4 anchor, +0xEC/F0
 * scale, +0xDC cursor, +0x104/0x130/0x134/0x136), per the field-by-field disasm comparison
 * (gen_render_satellite.py). So we run the matching setup, then the one shared walker/submit/
 * scene path. (loc_8c030af8 deposits are byte-identical to loc_8c03093c for the non-zoom
 * path the validated Cable drone node 0x8C271E54 takes; the only deltas are the gated-off
 * owner-char zoom table and the skipped frame-global proj-setup calls — both faithful here.) */
static int render_object_full_ex(Sh4Ctx *c, u32 node, int is_sat){
    /* ---- run the per-cat setup: computes node+0xE0/E4 (anchor) + 0xEC/F0 (scale) ---- */
    c->r[4]=node; c->r[14]=node; c->r[15]=0x0C480000u; c->pr=0xDEADBEEFu;
    if(is_sat) render_object_setup_030af8(c);   /* loc_8c030af8 (cat 1..4), honors +0x12C gate */
    else       render_object_setup_03093c(c);   /* loc_8c03093c (cat==0), honors +0x12C gate  */

    /* ---- run the proven walker to emit this object's body tiles (corners) ---- */
    Sh4Ctx wc; memcpy(&wc, c, sizeof wc); wc.ram=c->ram;
    wc.r[4]=node; wc.r[15]=0x0C480000u; wc.pr=0xDEADBEEFu;
    g_ncap=0;

    /* SPRITE_ID bit15 DISPATCH (re_kb/50; engine loc_8c034bea & mask loc_8c034c34=0x8000). The
     * engine routes (sel & 0x8000)==0 -> loc_8c0344d4 (multi-tile body walker, character bodies)
     * and (sel & 0x8000)!=0 -> loc_8c0348c8 (SCALE walker, ONE scaled sprite per cell record).
     * MVC2 super/projectile EFFECT parts carry bit15-set sels (Lightning Storm bolts 0x8006..0x801d);
     * the engine emits 1 sprite per part (A/B: ~90 quads on the super, each effect TCW once).
     * render_frame previously tiled them ~34x -> the phantom-body garble. The scale walker''s per-
     * record rectab alloc-index = 0x390 + read.b(*(node+0x180)+0x220+ctr), where node+0x180 is a
     * PER-CHARACTER effect display-list template that the engine rebuilds every frame — now shipped
     * in the per-frame read-set ("efxtmpl", maplecast_replica_live.cpp) so this read sees the LIVE
     * per-record indices (was stale -> wrong TCW). */
    u32 _sel = (u32)(u16)( (u32)wc.ram[(node & 0x00FFFFFFu) + 0x144]
                         | ((u32)wc.ram[(node & 0x00FFFFFFu) + 0x145] << 8) );
    int _is_effect = (_sel & 0x8000u) != 0;
    if (_is_effect) {
        walker_0348c8(&wc);              /* effect node: single scaled sprite per record */
    } else {
        /* CURSOR: trust the RESIDENT node+0xDC — the engine's OWN authoritative arena alloc index
         * (0x8C1F9D98 snapshot, loc_8c033b0a), shipped in the live wire (char_str + objpool). The
         * old "super-freeze repair" overwrote it with s_running_cursor when `resident==0 &&
         * cursor!=0`, but +0xDC==0 is CORRECT for the arena's FIRST body — an effect rendering
         * before it makes cursor!=0, so the guard fired on a legitimate first-body-0 and shifted
         * every P1C1 tile's idxtab index -> wrong TCWs / stale-texture blocks during motion.
         * MEASURED (garble_diff vs engine TA, vframe97425 + motion70): removing the repair takes the
         * body from 4/75 to 70/70 full-quad exact; resident +0xDC is authoritative every frame.
         * (finding:gsta_cursor_repair_first_body_regression, 2026-07-02.) */
        /* RACY-TILEDES FIX: regenerate this body's descriptor slice from the RESIDENT
         * GFX1 headers so the walker consumes a pose-correct grid instead of the torn
         * per-frame table (cures the marching phantom column). Writes into wc.ram ==
         * c->ram at DESC_TABLE+dc*4, exactly where the walker (and the extent read
         * below) will look. See rebuild_tile_grid header for the byte-exact validation. */
        /* A/B TOGGLE (user-confirmable): MAPLECAST_TILEGRID=0 disables the descriptor
         * regeneration so the walker consumes the raw (torn) per-frame tiledes — i.e.
         * the OLD behavior. Default ON. Lets the user flip the fix in/out live. */
        { const char* _tg = getenv("MAPLECAST_TILEGRID");
          if (!(_tg && _tg[0]=='0')) rebuild_tile_grid(&wc, node); }
        walker_0344d4(&wc);              /* character body: multi-tile walker (resident +0xDC) */
    }
    int ntiles = g_ncap;

    /* ---- CORRUPTION GATE (live multi-object robustness) ----
     * submit_1244b0 now clamps g_ncap at MAXQ. If the walker SATURATED that clamp, this
     * object's geometry source (its GFX2 cell stream / descriptor table) is corrupt or
     * stale — almost certainly a body whose art was not resident at the streamed prefix
     * snapshot, or a per-frame region the read-set under-ships. A real MVC2 body never
     * emits MAXQ tiles. Emitting these would paint a wall of garbage quads (the "quads=1024"
     * frames). DROP the object's tiles entirely; the rest of the scene renders clean. */
    if(ntiles >= MAXQ){
        return 0;   /* report 0 tiles: cursor advance unaffected (engine still owns +0xDC) */
    }

    /* ---- +0xDC BUDGET CLAMP: REMOVED (superseded by rebuild_tile_grid) ---------------
     * The old clamp cut the body's tile count to (next active +0xDC - this +0xDC) to hide
     * the OVER-emission that a torn/inflated per-frame tiledesc caused. rebuild_tile_grid
     * now regenerates each body's descriptor slice from the RESIDENT GFX1 headers, so the
     * walker self-terminates at the correct GFX1-derived count and the clamp has nothing to
     * cut. VERIFIED: with rebuild_tile_grid in place the clamp is a NO-OP on all 1762 frames
     * of _cape_live (A/B build with/without -DMC_DC_CLAMP: byte-identical _posviz scan
     * 1754/1762, worst 213px@f850, median 69px; Storm 24/24, Cable 27/27; ZERO new runaway
     * or off-screen column on any frame). The MAXQ corruption gate above remains the runaway
     * backstop. (The clamp was the descriptor bug's band-aid; the fix removes the wound.) */
    (void)dc_budget;

    /* ---- per-tile: compute params from resident rectab[idxtab[alloc_index]] + UV ---- */
    float sxs = rf(c, node+0xEC), sys = rf(c, node+0xF0);
    /* per-object PVR depth = the RECIPROCAL of node+0xE8. EMPIRICAL (probe2 PC 0x0C1248CC
     * vs render_frame node+0xE8 readback, 2026-06-14): node+0xE8 holds the homogeneous
     * W (~106..108 for on-screen bodies), and the engine submits z = 1/W (~0.00924) into
     * every PVR sprite vertex (Az=Bz=Cz). 1/108.2637 = 0.0092367 == engine probe Az 0.0092370.
     * PVR depth convention is 1/W (larger = NEARER), so we MUST emit 1/W, not W — emitting W
     * directly would INVERT the near/far sort and swap occlusion. All of this object's body
     * tiles share this z. This is the cape/body/projectile occlusion fix (replaces z=1.0). */
    float ow = rf(c, node+0xE8);
    float oz = (ow > 1e-6f && ow == ow) ? (1.0f / ow) : 0.5f;  /* z = 1/W; guard NaN/0 */
    u32 palbank = palbank_for(node);
    u32 node_gfx1 = r32(c, node+0x15C);   /* this body's GFX1 base (per-quad decode key) */
    /* body facing (node+0x110) — drives the texU mirror in lockstep with the position pen
     * the walker already reflected (re_kb routine:loc_8c0346c4 / loc_8c034548): one byte,
     * texture and position can never decouple. facing!=0 <=> faces RIGHT (P1 default). */
    u32 node_facing = r32(c, node+0x110) ? 1u : 0u;
    /* the walker's per-tile alloc_index already = node+0xDC + arena_base + k (it read the
     * resident node+0xDC and *(0x8C1F9D94)); we use it directly — that IS the cursor-
     * derived base in action. (render_frame separately ASSERTS node+0xDC == prefix-sum.) */
    for(int k=0; k<ntiles && g_nscene<MAXSCENE; k++){
        PolyParam pp;
        /* submit_params resolves TCW via rectab[idxtab[alloc]] — which is shipped STALE for
         * EFFECT quads (char-pass-transient idxtab, re_kb/50) -> the wrong (earlier-tile) TCW
         * -> 19 mispaired bolts sampling the wrong tile (pink/salmon). MEASURED 2026-07-05
         * (defect #3). The BTCW tail ships the engine's RESOLVED per-(node,k) tcw for effect
         * nodes too (verified: BTCW effect-addr multiset == engine 102/102, all 19 present),
         * so the override below is applied to effects AND bodies. */
        submit_params(c, g_cap[k].alloc_index, palbank, &pp);

        /* SHIP-RESOLVED-TCW OVERRIDE (bodies AND effects): replace the parity-flipping /
         * stale-idxtab rectab[idxtab[alloc]] tcw with the engine's RESOLVED per-tile tcw
         * shipped for (node, k). Falls back to pp.tcw when nothing was shipped for this tile. */
        {
            u32 shippedTcw;
            int hit = body_tcw_lookup(node, k, &shippedTcw);
            if(hit) {
#ifdef BODYTCW_DEBUG
                if(((node&0x0FFFFFFFu)|0x0C000000u)==0x0C268340u && k<2)
                    fprintf(stderr,"[BTOVR] node char tile=%d pp.tcw 0x%08X -> shipped 0x%08X (tex 0x%X->0x%X)\n",
                        k, pp.tcw, shippedTcw, (pp.tcw&0x1FFFFF)<<3, (shippedTcw&0x1FFFFF)<<3);
#endif
                pp.tcw = shippedTcw;
            }
            /* 3b-ii (MEASURED 2026-07-05): drop effect quads whose tcw is the stale-idxtab
             * FALLBACK (no shipped tcw for this tile) — that fallback is known-wrong for
             * effects (the yellow-wedge / phantom-band class). `continue` skips before
             * g_nscene++ so no phantom quad / g_scene_is_effect entry is emitted; k still
             * advances. Bodies untouched.
             * REVISED (2026-07-05, _live4 gate): cull on !hit ONLY. The original band test
             * (require 0x60xxxx) predates the BTCW palsel recompose + the snapshot-ordering
             * fix; on the coherent wire the engine LEGITIMATELY renders bit15 tiles with
             * body-arena tcws (engine-TA proof: LATTACK b17 arena quads on screen), and the
             * band test discarded 65-78 REAL tiles/frame on those moves. */
            if(_is_effect){
                if(!hit) continue;
            }
        }

        /* tile m: the descriptor byte0 for this tile. Re-derive from the resident table
         * exactly as the walker did: idx into DESC_TABLE = node+0xDC + (tile's record).
         * For a faithful extent we read m from the descriptor the walker used. The walker
         * leaves r13 advanced; simplest faithful source = the same descriptor stream.
         * We recompute m per tile from DESC_TABLE using node+0xDC as the record base. */
        u32 dc = r16u(c, node+0xDC);
        u32 m  = r8u(c, DESC_TABLE + (dc + k)*4);   /* byte0 = tile pixel size */
        if(m==0) m=8;                               /* guard: never 0 (8px min tile) */

        u32 texu = (pp.tsp>>3)&7; float tile=(float)(8u<<texu);
        /* TILE EXTENT source differs BODY vs EFFECT (MEASURED 2026-07-02 on the Lightning-Storm
         * super, finding:effect_quad_size_from_gfx1_logical). BODY: W=m*scale, m = body descriptor
         * byte0 (DESC_TABLE) per-tile pixel size. EFFECT (scale walker loc_8c0348c8): the sprite is
         * a SCALED single sprite whose UNSCALED extent is the GFX1 part header's LOGICAL dims
         * (lw*8 x lh*8, header +0/+1), NOT the body descriptor m and NOT the TSP tile. PROOF: all 47
         * bolts' unscaled W/H are exact multiples of 8 == lw*8/lh*8 for the record's sel (e.g. sel70
         * lw=6 -> W=48*sxs=80.0 == engine; sel66 lw=5 -> 40*sxs=66.7 == engine). The body m gave
         * 8*sxs=13.3 on the multi-tile bolts = the mis-sized/"missing" Lightning Storm bolts. */
        float W, H;
        float eff_uSpan = 1.0f, eff_vSpan = 1.0f;
        if (_is_effect) {
            u32 e_gfx1 = r32(c, node+0x15C);
            u32 e_off  = r32(c, e_gfx1 + g_cap[k].efx_sel*4) & 0x00FFFFFFu; /* GFX1[efx_sel] header */
            u32 lw = r8u(c, e_gfx1 + e_off + 0);                          /* logical tile width  */
            u32 lh = r8u(c, e_gfx1 + e_off + 1);                          /* logical tile height */
            if (lw==0) lw=1; if (lh==0) lh=1;
            W = (float)(lw*8u) * sxs;  H = (float)(lh*8u) * sys;
            /* PER-AXIS UV span = logical content / power-of-2 texture (MEASURED vs 7200:
             * q0 lw2/lh3, tex 16x32 -> uSpan 1.0 x vSpan 0.75). The single-scalar u1 (U=V) was
             * the 2nd effect defect: it mis-sampled non-square tiles -> salmon/brown. */
            u32 usize = 8u << ((pp.tsp >> 3) & 7u);
            u32 vsize = 8u << ( pp.tsp       & 7u);
            eff_uSpan = (float)(lw*8u) / (float)usize;
            eff_vSpan = (float)(lh*8u) / (float)vsize;
        } else {
            W = (float)m * sxs;        H = (float)m * sys;
        }
        float bx=g_cap[k].bx, by=g_cap[k].by;       /* walker anchor */
        g_scene_is_effect[g_nscene] = (u8)(_is_effect ? 1 : 0);  /* tag for the effect TCW post-pass */
        /* PER-QUAD SOURCE DESC CAPTURE (2026-07-05, the satellite-fragmentation fix; see
         * render_frame_quad_srcdesc_impl). Snapshot the walker's OWN per-tile descriptor
         * (DESC_TABLE entry dc+k: [0]=m px, [2]=cx STORAGE column, [3]=rows-row) AT EMIT
         * TIME — the table is a shared scratch the NEXT node's rebuild_tile_grid may
         * CLOBBER (measured: torn +0xDC made two nodes' slices overlap, so a decode-time
         * re-read of idx 464 returned the LATER node's entry). flags: bit0 = valid (body
         * tile), bit1 = the per-record flip4000 (the walker's own texU-flip bit, g_cap —
         * NOT facing): a flip4000 record pairs its tiles with storage columns DESCENDING.
         * Pure metadata capture — emission behavior unchanged. */
        g_scene_src_m [g_nscene] = (u8)(_is_effect ? 0 : (m > 255 ? 255 : m));
        g_scene_src_cx[g_nscene] = (u8)(_is_effect ? 0 : r8u(c, DESC_TABLE + (dc + k)*4 + 2));
        g_scene_src_ry[g_nscene] = (u8)(_is_effect ? 0 : r8u(c, DESC_TABLE + (dc + k)*4 + 3));
        g_scene_src_fl[g_nscene] = (u8)((_is_effect ? 0u : 1u) | ((g_cap[k].flip4000 & 1u) << 1));
        SceneQuad *q = &g_scene[g_nscene++];
        q->pcw=pp.pcw; q->isp=pp.isp; q->tsp=pp.tsp; q->tcw=pp.tcw;
        q->recidx=g_cap[k].alloc_index;
        q->sel=g_cap[k].sel;          /* per-quad source sel (tiling-safe pairing key) */
        q->gfx1=node_gfx1;            /* per-quad owning-body GFX1 base (decode with sel) */
        q->facing=node_facing;        /* owning body facing (carve storage-col disambiguation) */
        /* PER-PART depth (re_kb/38): the engine bumps W by +0.001 per submitted tile
         * (loc_8c034864, BEFORE each submit) so within ONE sprite Z=1/W strictly
         * DECREASES in submission order -> first tile FRONTMOST under the sprite list's
         * Greater/GEqual depth test. Emitting the constant per-object oz flattened all
         * parts to one plane, so intra-body order fell to paint/submission (last-on-top)
         * = the INVERSE of the engine -> cape-in-front. Feed 1/(W + 0.001*(k+1)): node_W
         * cancels for intra-sprite ordering, and the per-object ow band is preserved so
         * cross-object occlusion is unchanged. EMPIRICALLY constant-z before this fix
         * (frame 300: body0 23 parts all z=0.00940869); distinct per part after. */
        q->z = (ow > 1e-6f && ow == ow) ? (1.0f / (ow + 0.001f * (float)(k + 1))) : oz;
        /* texU mirror = facing XOR per-part 0x4000 (loc_8c0346c4 neg r8 gate). One bit.
         * MEASURED (A/B vs real :7200 TA, slot-0 deterministic): this facing-XOR formula
         * mismatches the real engine U on only 1.46% of body tiles (599/41163), whereas
         * flip4000-ALONE mismatches 35.04% (4551/12989) — the engine DOES fold facing into
         * the texel-U direction (the screen-X span and texU mirror are BOTH facing-coupled,
         * in lockstep). flip4000-alone was DISPROVEN by the diff despite the loc_8c0347bc
         * `tst r8 -> neg r5` disasm reading; the diff wins. The 1.46% residual is a separate,
         * smaller per-pose effect (pal-16 / one body), NOT the gross facing rule. */
        q->mirror = node_facing ^ (g_cap[k].flip4000 & 1u);
        /* SCREEN-X SPAN DIRECTION is set by the OWNING BODY FACING (node+0x110), in
         * lockstep with the walker's position pen and the texU mirror. The captured
         * submit anchor `bx` (r15+0x2C+0x04) is the LEFT edge when facing==0 and the
         * RIGHT edge when facing==1 — the engine's bank12 cell-processor builds the
         * quad AWAY from the anchor in the facing direction. CONFIRMED from _probe2.log
         * (Storm cid42 node 0x8c282354 facing=1: footAnchor.x=140.67 == engine corner
         * C.x[right], engine A.x=114.0=bx-W; Cable cid23 node 0x8c2688e4 facing=0:
         * footAnchor.x=511.67 == engine corner A.x[left]). Hardcoding bx-as-left gave a
         * uniform +W (one-tile, ~26.7px) X shift on every facing==1 body part.
         * left = facing ? bx-W : bx ; right = left + W. */
        float xl = node_facing ? (bx - W) : bx;
        float xr = xl + W;
        /* Y ANCHOR DIRECTION differs BODY vs EFFECT (MEASURED 2026-07-02, finding:effect_quad_y_anchor
         * on the Lightning-Storm super). The BODY walker loc_8c0344d4 is BOTTOM-anchored -> lays the
         * quad UPWARD (Ay=by-H, Cy=by). The EFFECT scale walker loc_8c0348c8 submits `by` as the quad
         * TOP -> lays DOWNWARD (Ay=by, Cy=by+H). PROOF: for every bolt whose corner A = the captured
         * (bx,by), engine Ay-by=0.00 and Cy-by=+H. */
        float ytop = _is_effect ? by       : (by - H);
        float ybot = _is_effect ? (by + H) : by;
        /* ROTATED EFFECT (node+0x104 & 0x8000 = 180deg, the scale walker's sin/cos leaf-flip path
         * loc_8c034b66): the quad is laid REVERSED on BOTH axes. MEASURED (Lightning-Storm bolts,
         * finding:effect_quad_180_rotation): with the sin/cos leaves now transpiled the anchor bx/by
         * is correct (== engine C.x/A.y), and the engine builds A=(bx+W, by), C=(bx, by-H) — i.e.
         * X-reversed (A=right) AND Y-reversed (A=top-most, box grows UP) vs the normal effect lay.
         * PROOF: engine A(396,405) C(369.3,371.1), bx=369.3 by=405 W=26.7 H=34.3 -> Ax=bx+W=396,
         * Cx=bx=369.3, Ay=by=405, Cy=by-H=370.7. Scoped to _is_effect + the 0x8000 flag so the 38
         * non-rotated bolts and the body path are untouched. */
        if (_is_effect && (r32(c, node+0x104) & 0x8000u)) {
            /* Y is reversed for both facings (box grows UP: A at by, C at by-H). X reversal is
             * FACING-DEPENDENT — the anchor bx is a different corner per facing (MEASURED node
             * 800c facing=1: bx==engine C.x[left] -> A=bx+W; node 8012 facing=0: bx==engine
             * A.x[right] -> A=bx, C=bx-W). So: facing=1 -> A=bx+W,C=bx ; facing=0 -> A=bx,C=bx-W. */
            xl = node_facing ? (bx + W) : bx;       /* A corner X */
            xr = node_facing ? bx       : (bx - W); /* C corner X */
            ytop = by;     ybot = by - H;           /* A at by (top), grows UP */
        }
        q->Ax=xl;     q->Ay=ytop;
        q->Bx=xr;     q->By=ytop;
        q->Cx=xr;     q->Cy=ybot;
        q->Dx=xl;     q->Dy=ybot;
        if (_is_effect) { q->u1 = eff_uSpan; q->v1 = eff_vSpan; }
        else            { q->u1 = ((float)m < tile) ? ((float)m/tile) : 1.0f; q->v1 = q->u1; }
    }
    return ntiles;
}

/* PUBLIC body render (cat==0): the slot-walk body hook calls this. */
int render_object_full(Sh4Ctx *c, u32 node){ return render_object_full_ex(c, node, 0); }

/* PUBLIC satellite render (cat 1..4): runs loc_8c030af8's setup then the SHARED body
 * walker/submit/scene path. This is the client half of the missing-sprites fix — a
 * body-sprite satellite (Cable drone/projectile, an assist, a cape, an extra limb)
 * now renders through render_object_full_ex with the satellite's exact field reads. */
int render_object_full_satellite(Sh4Ctx *c, u32 node){ return render_object_full_ex(c, node, 1); }

/* ============================================================================
 * The transpiled root slot-walk (gen_walker_root.c) calls render_frame_body_hook for
 * each cat==0 node and render_effect_030af8 for each cat 1..4 node.
 * ==========================================================================*/
void render_sprites_0308c2(Sh4Ctx *c);   /* gen_walker_root.c */

/* +0xDC BUDGET pre-pass: walk the on-screen slot table exactly like render_sprites_0308c2
 * (gen_walker_root.c, loc_8c0308c2) but ONLY collect each active node's resident +0xDC into
 * g_dc_table, so dc_budget() can clamp each body to (next active +0xDC - this +0xDC). Kept
 * HERE in render_frame.c (NOT the auto-generated gen_walker_root.c) so the generator can''t
 * clobber it. Node-enumeration gates are byte-identical to the render walk. */
static void collect_dc_0308c2(Sh4Ctx *c){
    const u32 COUNT_BASE = 0x8C2895E0u, PTR_BASE = 0x8C287DE0u, LAYER_STRIDE = 0x180u;
    u32 r8 = COUNT_BASE + 0x10u, r12 = PTR_BASE, r13 = COUNT_BASE;
    for(;;){
        u32 r10 = r12, r14 = 0;
        for(;;){
            s32 count = r8s(c, r13);
            if(!((s32)r14 < count)) break;
            if(count > 0x60) break;
            u32 node = r32(c, r10 + (r14 << 2));
            if(node == 0 || (((node >> 24) & 0x7Fu) != 0x0Cu)){ r14++; continue; }
            /* VISIBILITY GATE (node+0x12C LOW BYTE, nonzero-only) — must match render_sprites_0308c2
             * (gen_walker_root.c) exactly, or the +0xDC budget prefix-sum would count tiles for
             * objects the render walk skips (or vice-versa). The engine's real per-node gate is
             * node+0x12C, NOT node+0x00 (which the slot-walk never reads); swapped in lockstep with
             * gen_walker_root.c. TEST ONLY THE LOW BYTE (+0x12E=hit-flash, +0x130=xflip). */
            if(r8u(c, node + 0x12Cu) == 0){ r14++; continue; }
            dc_table_add(c, node);   /* both cat==0 body and cat 1..4 satellite share the arena */
            r14++;
        }
        r13 += 1; r12 += LAYER_STRIDE;
        if(!(r13 < r8)) break;
    }
}

/* CAT 1..4 dispatch (loc_8c030af8). The slot-walk routes node+0x3 in [1,5) here. The
 * routine itself (bank03:1538-1552) re-gates 0 < cat < 5 and reads node+0x12C; a body-
 * sprite satellite then deposits the walker fields and emits through loc_8c0344d4. We run
 * the transpiled setup + the shared walker via render_object_full_satellite, and advance
 * the SAME submit cursor (node+0xDC prefix-sum) the body path uses — satellites consume
 * idxtab/rectab slots from the very same arena as bodies (the walker loc_8c0344d4 reads
 * node+0xDC + arena_base regardless of cat), so the running cursor must include them.
 * Pure effects are NOT a separate path. CORRECTION (2026-06-14, re_kb finding:
 * replica_effect_walker_faithful): loc_8c1294c8 is a 20-byte MEMCPY (the anim-cell loader,
 * bank12:21885), NOT a cell processor — that label was a mislabel. The REAL effect path is
 * loc_8c030af8 -> loc_8c034bea (sel node+0x144 dispatch) -> loc_8c0344d4 (THIS walker),
 * reading cell records from GFX2 node+0x160 by (sel&0x7FFF) — IDENTICAL to a body. So a pure
 * effect with a valid +0x144 sel + +0x160 GFX2 (in the Effect-Poly bank 0x0CED0000) renders
 * faithfully right here; it emits 0 tiles only when culled (+0x12C!=0), terminated (sel==0xFF),
 * or its GFX2 art is not resident. bit15 sel (-> loc_8c0348c8 scaled twin) is the one OPEN
 * sub-case (per-part-scale dispatch, tracked separately). Body satellites + effects both render. */
void render_effect_030af8(Sh4Ctx *c, u32 node){
    extern void render_frame_satellite_hook(Sh4Ctx *c, u32 node);
    render_frame_satellite_hook(c, node);
}

/* ---- the cursor-advance bookkeeping the slot-walk needs ----
 * render_object_full uses the walker's resident-read alloc_index directly, but Phase 2's
 * proof obligation is that the per-object base GENERALIZES via the running cursor. We
 * therefore ALSO maintain render_frame's own running_cursor and verify it tracks the
 * engine's resident node+0xDC for every body object. These are exposed for the harness. */
int   g_body_count = 0;          /* how many body objects render_frame rendered      */
int   g_sat_count  = 0;          /* how many cat 1..4 satellites render_frame rendered */

/* ---- +0xDC BUDGET TABLE (the arena prefix-sum, engine-authoritative) --------------
 * The engine deposits node+0xDC as a per-object PREFIX-SUM into the tile-alloc arena
 * (loc_8c033b0a; body0 dc=0, body1 dc=38, ...). Each object's ALLOCATED tile budget is
 * therefore (the next-larger active +0xDC) - (this object's +0xDC). The engine's EFFECTIVE
 * per-body output equals that budget (measured: Storm body renders 36 == its 38-slot budget;
 * the tiledesc byte1-sum captured over the wire is inflated to 59-70 because our snapshot
 * grabs the descriptor table at a DIFFERENT instant than the ROM's walk). Clamping the
 * walker's emitted tile count to this budget REPRODUCES the ROM's effective output (it does
 * NOT diverge — the ROM at its own walk instant reads a descriptor already == the budget).
 * We collect every active body/satellite node's +0xDC in a pre-pass so dc_budget() can find
 * the next-larger value order-independently (the slot walk is NOT in +0xDC order). */
static u32 g_dc_table[128];      /* all active nodes' resident +0xDC this frame */
static int g_dc_count = 0;
static void dc_table_reset(void){ g_dc_count = 0; }
static void dc_table_add(Sh4Ctx *c, u32 node){
    if(g_dc_count < 128){ g_dc_table[g_dc_count++] = r16u(c, node + 0xDCu); }
}
/* budget for `node` = (smallest active +0xDC strictly greater than this node's dc) - dc.
 * Returns a large sentinel if this node has the maximum dc (last in the arena -> no clamp). */
static u32 dc_budget(Sh4Ctx *c, u32 node){
    u32 dc = r16u(c, node + 0xDCu);
    u32 next = 0xFFFFFFFFu;
    for(int i=0;i<g_dc_count;i++){ u32 v = g_dc_table[i]; if(v > dc && v < next) next = v; }
    if(next == 0xFFFFFFFFu) return 0xFFFFFFFFu;   /* last object: no upper bound */
    return next - dc;
}

u32   g_obj_dc_resident[64];     /* resident node+0xDC per body (engine prefix-sum)   */
u32   g_obj_dc_computed[64];     /* render_frame's running-cursor prefix-sum          */
int   g_obj_ntiles[64];          /* tiles each body emitted                           */
u32   g_obj_node[64];

/* render_frame_body_hook: called by the slot-walk for each BODY node (cat==0). It runs
 * the per-object render AND advances the running cursor, recording both the engine's
 * resident node+0xDC and our computed prefix-sum for the per-object proof. */
/* s_running_cursor: tentative-defined up top */
void render_frame_body_hook(Sh4Ctx *c, u32 node){
    if(g_body_count < 64){
        g_obj_node[g_body_count]       = node;
        g_obj_dc_resident[g_body_count]= r16u(c, node+0xDC);   /* engine's prefix-sum */
        g_obj_dc_computed[g_body_count]= s_running_cursor;     /* our running prefix  */
    }
    int nt = render_object_full(c, node);
    if(g_body_count < 64) g_obj_ntiles[g_body_count] = nt;
    s_running_cursor += (u32)nt;     /* advance cursor by this object's tile count    */
    g_body_count++;
}

/* render_frame_satellite_hook: called by the slot-walk for each cat 1..4 node (the
 * loc_8c030af8 dispatch). Runs the satellite render through the SAME shared walker/submit
 * path and advances the SAME submit cursor — satellites and bodies draw from one arena
 * (the walker reads node+0xDC + arena_base for every cat). Recorded under g_obj_* with the
 * bodies for the per-object proof; counted separately in g_sat_count for the harness. */
void render_frame_satellite_hook(Sh4Ctx *c, u32 node){
    if(g_body_count < 64){
        g_obj_node[g_body_count]       = node;
        g_obj_dc_resident[g_body_count]= r16u(c, node+0xDC);   /* engine's prefix-sum */
        g_obj_dc_computed[g_body_count]= s_running_cursor;     /* our running prefix  */
    }
    int nt = render_object_full_satellite(c, node);
    if(g_body_count < 64) g_obj_ntiles[g_body_count] = nt;
    s_running_cursor += (u32)nt;
    g_body_count++;
    if(nt > 0) g_sat_count++;         /* a body-sprite satellite that actually emitted */
}

/* PUBLIC ENTRY: render_frame(ram) — reset cursor, walk all slots, render all bodies into
 * the scene TA accumulator. Caller reads g_scene[0..g_nscene) and the per-object proof. */
void render_frame_reset(void){
    g_nscene=0; g_body_count=0; g_sat_count=0; s_running_cursor=0;
    dc_table_reset();               /* clear the +0xDC budget table for this frame */
    /* EFFECT TCW post-pass state is PER-FRAME (recomputed in render_frame_fix_effect_tcws from
     * g_scene_is_effect + the per-frame min recidx) — nothing to reset here. */
}
int  render_frame_nscene(void){ return g_nscene; }
const SceneQuad* render_frame_scene(void){ return g_scene; }

/* PER-QUAD INTRA-PART TILE (col,row) — the WIDE-PART carve key. col MUST be the part's
 * STORAGE column (facing-INDEPENDENT), so the carve always slices the same fixed storage
 * chunk; the visual L/R flip is then applied SEPARATELY via the texU mirror (q->mirror,
 * matching the engine loc_8c0346c4 neg-r8 gate). The part is stored ONCE in VRAM in
 * storage order; facing mirrors at DRAW time, never re-stores.
 *
 * THE FACING FIX (2026-06-13, traced to the ASMTRACE per-tile screenX vs r13 emission order
 * on prod /dev/shm/mc_assembly.log): for the SAME multi-column part the storage column ->
 * screen-X direction REVERSES with facing —
 *   facing=0 (faces LEFT, e.g. Sentinel cid52 sel124): storage-col-0 lands at the HIGHEST
 *     screenX (emission r13-ascending = screenX DESCENDING). So storage col = rank Ax DESC.
 *   facing=1 (faces RIGHT, e.g. Cable cid23 sel232/231): storage-col-0 lands at the LOWEST
 *     screenX (emission r13-ascending = screenX ASCENDING). So storage col = rank Ax ASC.
 * The OLD code always ranked Ax DESC -> for facing=1 it carved the COLUMN-MIRRORED storage
 * chunk -> the per-side scramble the user saw (garbled on the facing=1 side). The walker's
 * position pen already reflects under facing (0.00px), so col is the ONLY remaining facing-
 * coupled decode term besides the texU mirror. Row (Y) is unaffected by facing (the pen only
 * reflects X) -> row stays rank Ay DESC. CITE: ASMTRACE col5/col10/col17; re_kb
 * routine:loc_8c0346c4 / loc_8c034548 / field:facing. */
/* count DISTINCT values (within +-0.5px) strictly on the `gt` side of `v` among the run's
 * anchors on one axis -> the 0-based rank of `v`. gt=1 -> descending rank (count strictly
 * greater), gt=0 -> ascending rank (count strictly less). */
static int distinct_rank(u32 kg, u32 ks, int axis /*0=Ax,1=Ay*/, float v, int gt){
    int rank = 0;
    for(int r=0;r<g_nscene;r++){
        if(g_scene[r].gfx1!=kg || g_scene[r].sel!=ks) continue;
        float rv = axis ? g_scene[r].Ay : g_scene[r].Ax;
        if(gt ? (rv <= v + 0.5f) : (rv >= v - 0.5f)) continue;  /* not strictly on the gt side */
        /* count rv only the first time it appears (distinct) */
        int first = 1;
        for(int t=0;t<r;t++){
            if(g_scene[t].gfx1!=kg||g_scene[t].sel!=ks) continue;
            float tv = axis ? g_scene[t].Ay : g_scene[t].Ax;
            if((gt ? (tv > v + 0.5f) : (tv < v - 0.5f)) && fabsf(tv-rv) < 0.5f){ first = 0; break; }
        }
        if(first) rank++;
    }
    return rank;
}
u32 render_frame_quad_colrow_impl(int* out_cr, u32 cap){
    u32 w = 0;
    for(int q=0; q<g_nscene && w<cap; q++,w++){
        u32 kg = g_scene[q].gfx1, ks = g_scene[q].sel;
        /* STORAGE column = rank Ax ASCENDING for BOTH facings (col-0 at lowest screen X).
         * CORRECTED 2026-06-14 (END-TO-END atlas-render proof, _oracle/tmp/validate_e2e.py +
         * tools/render-replica-poc/_test_colrev.mjs): the prior facing-DESC branch for facing==0
         * DOUBLE-applied the L/R flip — once via the storage-column reversal and once via the
         * texU mirror (render_frame_quad_mirror) — splitting every multi-column body on the
         * facing==0 side (the _scramble_actual Cable rendered as two horizontally-offset halves;
         * forcing ASC assembles it byte-clean). The carve stores cells in fixed storage order;
         * the SINGLE source of the visual L/R flip is the texU mirror alone (loc_8c0346c4). So
         * col is facing-INDEPENDENT ascending and the mirror does the flip. (facing==1 was already
         * ASC and rendered clean — _satlive Cable — so this is a no-op there and a fix for facing==0;
         * closes finding:emitter_flip_unvalidated for multi-column bodies.) */
        out_cr[2*w]   = distinct_rank(kg, ks, 0, g_scene[q].Ax, 0); /* col: storage, Ax ASC */
        out_cr[2*w+1] = distinct_rank(kg, ks, 1, g_scene[q].Ay, 1); /* row: Ay desc */
    }
    return w;
}

/* PER-QUAD texU MIRROR bit (facing XOR per-part 0x4000). The client swaps the quad U coords
 * when set, mirroring each tile horizontally (the engine loc_8c0346c4 neg-r8 texture-U flip).
 * out_m[k] = 0/1. Decoupled from col (storage) so a wide part is stored ONCE and flipped here. */
u32 render_frame_quad_mirror_impl(uint8_t* out_m, u32 cap){
    u32 w = 0;
    for(int q=0; q<g_nscene && w<cap; q++,w++) out_m[w] = (uint8_t)(g_scene[q].mirror & 1u);
    return w;
}

/* Per-quad bit15 effect tag (g_scene_is_effect), exposed so the client's body private-bank
 * palette repoint can EXCLUDE effect quads. The repoint matches by gfx1 alone, and effect
 * quads share the owning character's gfx1, so without this they get swept into a body's warm
 * private bank (palsel->0) => yellow. CONFIRMED-BY-MEASUREMENT 2026-07-05 (engine effect
 * palsel=18; live-only repoint clobbered it to 0). */
u32 render_frame_quad_is_effect_impl(uint8_t* out_e, u32 cap){
    u32 w = 0;
    for(int q=0; q<g_nscene && w<cap; q++,w++) out_e[w] = (uint8_t)(g_scene_is_effect[q] & 1u);
    return w;
}

/* PER-QUAD SOURCE DESCRIPTOR (2026-07-05 — the satellite-fragmentation/texel fix, offline
 * byte-gate texel_gate.cpp). out[4*q..4*q+3] = [m, cx, ry, flags] snapshotted AT EMIT TIME
 * from the walker's OWN DESC_TABLE entry (idx = node+0xDC + k):
 *   m     tile pixel size (8/16/32) — the descriptor byte0 the walker itself consumed;
 *   cx    STORAGE column (facing-independent; rebuild_tile_grid byte2);
 *   ry    rows - row (rebuild_tile_grid byte3; row = (H/m) - ry);
 *   flags bit0 = valid (0 for bit15 effect quads — their recidx is a scale-walker alloc),
 *         bit1 = per-record flip4000 (the walker's texU-flip bit): pairs storage columns
 *                DESCENDING (col = pCols-1-cx). Keyed on flip4000 alone, NOT the effective
 *                texU mirror: facing does not reorder storage (MEASURED 98/98 on the
 *                facing-mirrored pal17 satellites vs engine VRAM; flip4000 sel 0xD4C
 *                engine slot0 holds col1). Emit-time capture is CLOBBER-PROOF: the desc
 *                table is shared scratch that a later node's rebuild_tile_grid overwrites
 *                (measured overlap on torn +0xDC).
 * This SUPERSEDES rank-based colrow as the carve key: global Ax/Ay ranks merge/interleave
 * when 2+ satellite nodes draw the same (gfx1,sel) per frame (typhoon), collapsing the
 * rank grid (sel 0xDEC ranked 16 cols on an 8-col part). Rank colrow remains exported for
 * consumers/diagnostics. The decoders (maplecast_mirror.cpp gstaDecodeBodies +
 * body_decoder.mjs ensureBodyTextures) consume this in lockstep. */
u32 render_frame_quad_srcdesc_impl(uint8_t* out, u32 cap){
    u32 w = 0;
    for(int q=0; q<g_nscene && w<cap; q++,w++){
        out[4*w+0] = g_scene_src_m [q];
        out[4*w+1] = g_scene_src_cx[q];
        out[4*w+2] = g_scene_src_ry[q];
        out[4*w+3] = g_scene_src_fl[q];
    }
    return w;
}

/* EFFECT TCW POST-PASS (re_kb/50 — the per-super-correct effect idxtab remap). The wire ships
 * the idxtab effect entries STALE (char-pass-transient), so every effect quad's TCW was resolved
 * to the wrong (body) texture during the walk. The FRESH rectab DOES hold the effect textures in
 * a CONTIGUOUS block (effect-band TCWs 0x89000..0x8bfff), and the engine maps the frame's
 * contiguous effect index range 1:1 onto it. So: find this frame's effect alloc_index range,
 * find the rectab effect-block start, and re-resolve each effect quad's TCW =
 * rectab[block_start + (recidx - frame_min)] + 0x0C. Using the PER-FRAME (per-super) min is the
 * fix for the "blocks" — a single match-global min drifts across different supers (a later super
 * with lower indices lands quads BELOW the block, in the 0x85xxx band = the floating blocks).
 * Runs only when effect quads exist (zero cost otherwise); body quads untouched. */
static void render_frame_fix_effect_tcws(Sh4Ctx *c){
    /* frame min effect alloc_index */
    u32 fmin = 0xFFFFFFFFu; int any = 0;
    for(int i=0;i<g_nscene;i++) if(g_scene_is_effect[i]){ any=1; if(g_scene[i].recidx < fmin) fmin = g_scene[i].recidx; }
    if(!any) return;
    u32 block = find_effect_block_start(c);
    if(block == 0) return;                          /* no effect rectab block this frame */
    u32 rectab = r32(c, 0x8C2DAD4C);
    if(((rectab>>24)&0x7Fu) != 0x0Cu) return;
    for(int i=0;i<g_nscene;i++){
        if(!g_scene_is_effect[i]) continue;
        u32 entry = block + (g_scene[i].recidx - fmin);   /* contiguous map into the effect block */
        if(entry >= 2048u) continue;                      /* out of rectab — leave as-is */
        u32 tcw = r32(c, rectab + entry*0x20 + 0x0C);
        /* only override with a real effect-band TCW (guards a stale/zero entry from corrupting) */
        u32 band = tcw & 0x1FFFFFu;
        if(band >= 0x89000u && band < 0x8C000u) g_scene[i].tcw = tcw;
    }
}

/* render_frame_cull_85xxx REMOVED 2026-07-09. It was a band-aid that collapsed render_frame's
 * OWN wrong quads (the ~7-15 body/satellite parts that resolve to the never-engine 0x85xxx band
 * during supers — re_kb/51 motion blocks) to zero-area so they emit no pixels. That masks the
 * underlying walker geometry divergence instead of fixing it, and was validated on only 3 frames
 * ("no-op on correct geom"). Removed so the ground-truth (ASMTRACE) diff sees the real
 * divergence; the fix is correct walker geometry, after which nothing lands in 0x85xxx anyway. */

void render_frame(Sh4Ctx *c){
    render_frame_reset();
    /* +0xDC BUDGET PRE-PASS: collect every active node's resident +0xDC BEFORE rendering, so
     * dc_budget() can clamp each body to (next-larger active +0xDC - this dc) = its arena-
     * allocated tile count. This reproduces the engine's EFFECTIVE per-body output (== budget)
     * and cures the over-emission caused by the wire's inflated tiledesc byte1-sum. */
    collect_dc_0308c2(c);
    render_sprites_0308c2(c);   /* the transpiled loc_8c0308c2; calls render_object_full */
    /* EFFECT-TCW POST-PASS REMOVED (2026-07-02, finding:effect_tcw_postpass_obsolete). The
     * per-frame render_frame_fix_effect_tcws forced effect quads into a "contiguous effect block"
     * (band 0x89000..0x8C000) — a heuristic that was a BAND-AID for the pre-fix wrong effect
     * geometry. Now that the effect quads carry the correct native TCW (via the 4 effect terms:
     * GFX1-logical size, Y-down anchor, transpiled sin/cos leaves, 180deg-rotation corner-lay),
     * the post-pass is HARMFUL: MEASURED on BOTH supers it CORRUPTS the pal distribution — Lightning
     * Storm pal18 47->~14, Projectile pal 66/47/17 -> 90/2/12/26 (94/130). With it OFF both supers
     * match the engine (lstorm 153/154 + 47/47 bolts; projectile 129/130; both pal-exact) and body
     * motion70 stays 70/70. So it is REMOVED, not scoped. render_frame_cull_85xxx is KEPT as a
     * harmless safety net (MEASURED: 0 quads land in the never-engine 0x85xxx band on all 3 frames
     * with correct geometry, so it is a strict no-op today; retained to collapse any future stale
     * 0x85xxx quad to zero-area, re_kb/51). */
    /* (render_frame_cull_85xxx removed 2026-07-09 — unmask re_kb/51 walker divergence; see above) */
}
