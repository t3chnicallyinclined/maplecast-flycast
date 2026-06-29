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
void leaf_e2e0(Sh4Ctx*c){ (void)c; }
void leaf_e860(Sh4Ctx*c){ (void)c; }
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
typedef struct { float bx, by; u32 alloc_index; u32 sel; u32 flip4000; } TileCap;
static TileCap g_cap[MAXQ];
static int g_ncap = 0;

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
    float Ax,Ay,Bx,By,Cx,Cy,Dx,Dy, u1;
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
        /* SUPER-FREEZE CURSOR REPAIR for the bit15-CLEAR body walker (re_kb/50): on a freeze frame
         * the resident node+0xDC is stale-0 for the real bodies too, so the tiling walker mis-indexes
         * the tiledesc and over-tiles. render_frame''s s_running_cursor IS the engine prefix-sum
         * (advanced by emitted tile count in the hooks); substitute it when resident==0 && cursor!=0
         * (the unambiguous stale-non-first case) — a strict no-op on normal frames. */
        u32 resident_dc = (u32)(u16)( (u32)wc.ram[(node & 0x00FFFFFFu) + 0xDC]
                                    | ((u32)wc.ram[(node & 0x00FFFFFFu) + 0xDD] << 8) );
        if (resident_dc == 0 && s_running_cursor != 0) {
            u32 lo = (node & 0x00FFFFFFu) + 0xDC;
            wc.ram[lo]     = (u8)(s_running_cursor & 0xFF);
            wc.ram[lo + 1] = (u8)((s_running_cursor >> 8) & 0xFF);
        }
        walker_0344d4(&wc);              /* character body: multi-tile walker */
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
        /* EFFECT idxtab is shipped STALE (char-pass-transient, re_kb/50), so submit_params
         * resolves the wrong (body) TCW for effect quads. We DEFER the fix to a per-frame
         * POST-PASS (render_frame_fix_effect_tcws) which has ALL effect alloc_indices and can
         * use the PER-FRAME (per-super) min as the base — a single inline match-global min
         * drifts across different supers and lands quads in the wrong rectab band (the blocks).
         * Here we just emit the body-resolved quad and TAG it (g_scene_is_effect) so the
         * post-pass re-resolves its TCW from the contiguous effect rectab block. */
        submit_params(c, g_cap[k].alloc_index, palbank, &pp);

        /* tile m: the descriptor byte0 for this tile. Re-derive from the resident table
         * exactly as the walker did: idx into DESC_TABLE = node+0xDC + (tile's record).
         * For a faithful extent we read m from the descriptor the walker used. The walker
         * leaves r13 advanced; simplest faithful source = the same descriptor stream.
         * We recompute m per tile from DESC_TABLE using node+0xDC as the record base. */
        u32 dc = r16u(c, node+0xDC);
        u32 m  = r8u(c, DESC_TABLE + (dc + k)*4);   /* byte0 = tile pixel size */
        if(m==0) m=8;                               /* guard: never 0 (8px min tile) */

        u32 texu = (pp.tsp>>3)&7; float tile=(float)(8u<<texu);
        float W = (float)m * sxs, H = (float)m * sys;
        float bx=g_cap[k].bx, by=g_cap[k].by;       /* walker BOTTOM-left anchor */
        g_scene_is_effect[g_nscene] = (u8)(_is_effect ? 1 : 0);  /* tag for the effect TCW post-pass */
        SceneQuad *q = &g_scene[g_nscene++];
        q->pcw=pp.pcw; q->isp=pp.isp; q->tsp=pp.tsp; q->tcw=pp.tcw;
        q->recidx=g_cap[k].alloc_index;
        q->sel=g_cap[k].sel;          /* per-quad source sel (tiling-safe pairing key) */
        q->gfx1=node_gfx1;            /* per-quad owning-body GFX1 base (decode with sel) */
        q->facing=node_facing;        /* owning body facing (carve storage-col disambiguation) */
        q->z=oz;                      /* per-object depth (node+0xE8 = 1/w) for correct sorting */
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
        q->Ax=xl;     q->Ay=by-H;     /* lay the quad UPWARD from the bottom-left */
        q->Bx=xr;     q->By=by-H;
        q->Cx=xr;     q->Cy=by;
        q->Dx=xl;     q->Dy=by;
        q->u1 = ((float)m < tile) ? ((float)m/tile) : 1.0f;
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

/* CULL PROVABLY-WRONG 0x85xxx QUADS (re_kb/51 — the motion BLOCKS). The 0x85xxx TCW band is
 * NEVER used by the engine (MEASURED: 0 quads across all 1077 mirror frames of _live_fx6),
 * yet during a super render_frame emits ~7-15 BODY/satellite quads that resolve to 0x85xxx —
 * the gray/white/pink floating BLOCKS the user sees. Their idxtab entry is char-pass-transient
 * (the body satellite parts the engine GATES OFF at the super peak; the mirror super-peak has
 * only effect bands, no 0x82-85 satellite body parts). We cannot reconstruct a "correct" entry
 * (the stale idxtab lost it and the engine draws nothing there), so DEGENERATE these quads to a
 * zero-area triangle (collapse all 3 verts to A) so they emit no pixels — matching the engine,
 * which draws nothing in 0x85xxx. Provably safe: 0x85xxx is never-engine, so culling can only
 * remove garbage. Rare (3/360 frames sampled, super-only); normal frames untouched. */
static void render_frame_cull_85xxx(void){
    for(int i=0;i<g_nscene;i++){
        u32 band = g_scene[i].tcw & 0x1FFFFFu;
        if(band >= 0x85000u && band < 0x86000u){
            /* collapse the quad: B=C=D=A so the rasterizer emits nothing. */
            g_scene[i].Bx = g_scene[i].Cx = g_scene[i].Dx = g_scene[i].Ax;
            g_scene[i].By = g_scene[i].Cy = g_scene[i].Dy = g_scene[i].Ay;
        }
    }
}

void render_frame(Sh4Ctx *c){
    render_frame_reset();
    render_sprites_0308c2(c);   /* the transpiled loc_8c0308c2; calls render_object_full */
    render_frame_fix_effect_tcws(c);   /* re-resolve effect quad TCWs (per-frame correct) */
    render_frame_cull_85xxx();          /* drop the never-engine 0x85xxx blocks (re_kb/51) */
}
