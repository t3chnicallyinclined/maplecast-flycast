/* END-TO-END CHAIN: input RAM snapshot -> executor game-tick -> render_frame -> scene quads.
 * Proves the full off-emulator pipeline: run the transpiled game-logic tick (gen_tick_all.c)
 * to advance the RAM one frame, then run the transpiled render walker (render_frame.c) over
 * the resulting state to produce the drawn scene. Compare against render_frame over the REAL
 * next-frame snapshot to confirm our executor's output renders identically.
 *
 *   chain_render <ram.bin> <tick|notick> <out_scene.bin>
 *
 * tick   = run the executor first (input->next-frame state), then render  == our pipeline
 * notick = render the RAM as-is (used on the real _ram_f91 as the reference)               */
#include "sh4ctx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void tick_entry(Sh4Ctx *c);
void call_addr(Sh4Ctx *c, u32 a);   /* executor dispatch — call any transpiled fn by address */
void render_frame_reset(void);
void render_frame(Sh4Ctx *c);
int  render_frame_nscene(void);
const void* render_frame_scene(void);
unsigned long render_frame_quad_bytes(void);

/* --- minimal executor runtime deps (no debug hooks) --- */
static long g_calls = 0;
int  mc_call_guard(void){ return (++g_calls > 5000000L) ? 1 : 0; }
void mc_unknown_call(u32 a){ (void)a; }
void mc_unk_regs(u32 *r){ (void)r; }
u32  mc_curfn = 0;
void mc_push(u32 a){ (void)a; }
void mc_pop(void){}

/* 32 MiB: the 16 MiB area-3 image + 16 MiB zeroed headroom, matching the live client's
 * rep.ram — the executor/render_frame address past 16 MiB and a bare 16 MiB Vec segfaults. */
static u8 ram[32u*1024u*1024u];
static u8 scratch[32u*1024u*1024u];   /* render over THIS copy — render_frame writes to RAM */
static int load(const char *p){ FILE *f=fopen(p,"rb"); if(!f) return 0;
    size_t n=fread(ram,1,RAM_SIZE,f); fclose(f); return n==RAM_SIZE; }

int main(int argc, char **argv){
    const char *rp   = argc>1 ? argv[1] : "_ram_f90.bin";
    const char *mode = argc>2 ? argv[2] : "tick";
    const char *out  = argc>3 ? argv[3] : "scene_out.bin";
    if(!load(rp)){ printf("cannot load %s\n", rp); return 2; }

    Sh4Ctx c; memset(&c,0,sizeof c); c.ram=ram; c.r[15]=0x8CFF0000u;
    if(!strcmp(mode,"driveN")){
        /* CONTINUOUS DRIVE + RENDER: the live MC_LOCAL loop, offline. Each frame: tick the game
         * logic, recompose the render proj (tickproj), then render_frame the driven state. Prints
         * every frame + flushes, so if render_frame segfaults the last printed frame IS the crash
         * frame. This reproduces the live ~frame-180 crash where we can localize it. */
        int N = argc>3 ? atoi(argv[3]) : 300;
        printf("driveN: %d frames of tick+compose+render on %s\n", N, rp); fflush(stdout);
        static unsigned char projsave[64];
        for(int f=0; f<N; f++){
            g_calls = 0;
            /* PROJCARRY: capture the SEED's valid render projection @0x2D6AD8 once (frame 0, before
             * any tick zeroes it). The tick-only drive can't rebuild the proj (the render pass does
             * that, and compose alone yields zeros), so for a short idle/animation demo we CARRY the
             * seed proj into every frame's render copy -> finite screen coords across the drive. */
            if(f==0) memcpy(projsave, ram+0x002D6AD8u, 64);
            /* Arena frame-setup (loc_8c033950, render-pass-side, NOT in the tick): reset the
             * tile-arena running cursor so the tick-side tile assembler (loc_8c033b0a) rebuilds
             * correct per-object +0xDC prefix-sums each frame instead of accumulating -> collapsing
             * to 0x180 -> render_frame DESC-base wrong -> quad fade to 0. THE render-state refresh. */
            if(!getenv("NOARENA")){ *(u32*)(ram+0x1F9D98)=0; *(u32*)(ram+0x1F9D94)=16; }
            /* INJECT=1: drive P1's latched Input_DEC (@0x2681DC) with a scripted demo sequence so
             * the executor VISIBLY drives the match (walk/jump/attack) — the "drivable pure-local"
             * proof. CPS2 active-high bits (dc_to_cps2): R=0x0400 L=0x0800 U=0x2000 D=0x1000,
             * LP=0x0200 HP=0x0100 LK=0x0040 HK=0x0020. cur@+0, prev@+2, pressed@+4, released@+6. */
            if(getenv("INJECT")){
                static unsigned short prev = 0;
                unsigned short cur = 0;
                if      (f < 30)  cur = 0x0400;              /* walk FORWARD (Right) */
                else if (f < 42)  cur = 0x0400 | 0x2000;     /* jump forward (Right+Up) */
                else if (f < 55)  cur = 0x0100;              /* HP punch */
                else if (f < 70)  cur = 0x0400 | 0x0100;     /* forward + HP */
                else if (f < 85)  cur = 0x0800;              /* walk BACK (Left) */
                else if (f < 100) cur = 0x1000 | 0x0040;     /* crouch LK (Down+LK) */
                else              cur = 0x0100 | 0x0200;     /* HP+LP */
                unsigned char *idec = ram + 0x2681DCu;       /* P1 Input_DEC */
                *(unsigned short*)(idec+0) = cur;
                *(unsigned short*)(idec+2) = prev;
                *(unsigned short*)(idec+4) = (unsigned short)(cur & ~prev);
                *(unsigned short*)(idec+6) = (unsigned short)(prev & ~cur);
                prev = cur;
            }
            tick_entry(&c);                 /* game-logic tick, in place (persistent game state) */
            /* FIX (re expert): the deferred-render-queue counter @0x8C289F80 is reset only by the
             * STUBBED render flush -> it jams at 16. Reset it each frame so the enqueue keeps working. */
            if(!getenv("NORESET")){ ram[0x289F80]=0; ram[0x289F81]=0; ram[0x289F82]=0; ram[0x289F83]=0; }
            /* Render over a COPY: render_frame writes ~170 sites incl screen-coord write-backs
             * (+0xE0/E4) THROUGH slot pointers — on the persistent image that corrupts the state
             * the next tick reads (the fast ~5f drain). proj + render both run on the copy. */
            u8 *rram = ram;
            if(!getenv("RENDER_INPLACE")){ memcpy(scratch, ram, sizeof ram); rram = scratch; }
            if(getenv("PROJCARRY")){ memcpy(rram+0x002D6AD8u, projsave, 64); }   /* carry seed proj -> finite coords */
            else if(!getenv("NOPROJ")){ Sh4Ctx pc; memset(&pc,0,sizeof pc); pc.ram=rram; pc.r[15]=0x8CFF0000u;
              call_addr(&pc, 0x8c1216c0u); }
            /* Clamp the 16 slot-table layer counts to 0x60: the walker (loc_8c0308c2) reads them
             * BLIND, and a corrupted high count overruns g_scene = the live segfault. */
            if(!getenv("NOCLAMP")){ for(int L=0;L<16;L++) if(rram[0x2895E0+L]>0x60) rram[0x2895E0+L]=0x60; }
            render_frame_reset();
            { Sh4Ctx rc; memset(&rc,0,sizeof rc); rc.ram=rram; rc.r[15]=0x8CFF0000u;
              render_frame(&rc); }
            /* SCDIR=dir: dump each frame's scene quads (nscene * 88B) so an offline rasterizer
             * (make_demo.py) can turn the PURE-LOCAL drive into a visible PNG/GIF sequence —
             * a demo of the executor-as-engine that sidesteps the live-client GPU driver exit. */
            { const char *scd = getenv("SCDIR");
              if(scd){ char p[512]; snprintf(p,sizeof p,"%s/scene_%03d.bin", scd, f);
                FILE *so=fopen(p,"wb"); if(so){ int n=render_frame_nscene(); unsigned long qb=render_frame_quad_bytes();
                  fwrite(render_frame_scene(), qb, (size_t)n, so); fclose(so); } } }
            /* CHDIR=dir: dump the BYTE-EXACT game-tick state per frame — the 6 char structs
             * (page 616 @0x268340) + the global page (@0x289000). This is the proven-correct
             * data (render coords need the unsolved render-pass proj; game state does not), so
             * make_demo.py visualizes the real match the executor drives: positions, health,
             * meters, animation — pure local, no server, no flycast. Dump `ram` AFTER the tick
             * (the persistent game state; render ran on the scratch copy, ram is untouched). */
            { const char *chd = getenv("CHDIR");
              if(chd){ char p[512]; snprintf(p,sizeof p,"%s/state_%03d.bin", chd, f);
                FILE *co=fopen(p,"wb"); if(co){ /* 0x268000..0x28A000 covers chars(+0x340) + globals(+0x21000) */
                  fwrite(ram+0x268000u, 1, 0x22000u, co); fclose(co); } } }
            printf("  f%d: %d quads (tickcalls=%ld)\n", f, render_frame_nscene(), g_calls);
            fflush(stdout);
        }
        printf("driveN: SURVIVED %d frames\n", N); fflush(stdout);
        return 0;
    }
    if(!strcmp(mode,"tickN")){
        /* Tick N times, then render ONCE fresh. If the N-tick STATE renders full (104) but the
         * per-frame driveN renders 0 at frame N, the decay is render_frame's cross-call GLOBAL
         * state (arena/cursor/g_scene not fully reset), NOT the game/render state. */
        int N = argc>3 ? atoi(argv[3]) : 5;
        for(int f=0; f<N; f++){ g_calls=0;
            if(!getenv("NOARENA")){ *(u32*)(ram+0x1F9D98)=0; *(u32*)(ram+0x1F9D94)=16; }
            tick_entry(&c);
            ram[0x289F80]=ram[0x289F81]=ram[0x289F82]=ram[0x289F83]=0; }
        memcpy(scratch, ram, sizeof ram);
        { Sh4Ctx pc; memset(&pc,0,sizeof pc); pc.ram=scratch; pc.r[15]=0x8CFF0000u; call_addr(&pc,0x8c1216c0u); }
        for(int L=0;L<16;L++) if(scratch[0x2895E0+L]>0x60) scratch[0x2895E0+L]=0x60;
        render_frame_reset();
        { Sh4Ctx rc; memset(&rc,0,sizeof rc); rc.ram=scratch; rc.r[15]=0x8CFF0000u; render_frame(&rc); }
        printf("tickN(%d): %d quads (single FRESH render of the %d-tick state)\n", N, render_frame_nscene(), N);
        return 0;
    }
    if(!strcmp(mode,"projonly")){
        /* diagnostic: no tick — just run the proj composer on the (valid) input RAM.
         * If it reproduces 0x2D6AD8, the composer works standalone; then the FP entry
         * state (FR/SZ) or a tick-corrupted input is the tickproj culprit. */
        u32 fp = getenv("FPSCR") ? (u32)strtoul(getenv("FPSCR"),0,16) : 0;
        c.fpscr = fp;
        call_addr(&c, 0x8c1216c0u);
        printf("projonly (fpscr=0x%08X): 0x2D6AD8 ->", fp);
        for(int i=0;i<16;i++){ u32 w=r32(&c,0x8C2D6AD8u+i*4); printf(" %.3g", *(float*)&w); } printf("\n");
    }
    if(!strcmp(mode,"tick") || !strcmp(mode,"tickcam") || !strcmp(mode,"tickproj")){
        /* The game-logic tick transiently zeroes the render-walk's projection matrix
         * @0x2D6AD8 (the render subtree normally re-deposits it). Three ways to handle it:
         *   tick     : leave it (render coords go NaN — shows the raw gap)
         *   tickcam  : carry the 64B camera proj across the tick (interim hack)
         *   tickproj : LEGIT — run the render-walk proj composer loc_8c1216c0 post-tick,
         *              which rebuilds 0x2D6AD8 from matrix-stack descriptors 2/3. */
        u8 projsave[64];
        if(!strcmp(mode,"tickcam")) memcpy(projsave, ram+0x002D6AD8u, 64);
        tick_entry(&c);                 /* input -> next-frame game state (in-place) */
        if(!strcmp(mode,"tickcam")) memcpy(ram+0x002D6AD8u, projsave, 64);
        if(!strcmp(mode,"tickproj")){
            Sh4Ctx pc; memset(&pc,0,sizeof pc); pc.ram=ram; pc.r[15]=0x8CFF0000u;
            call_addr(&pc, 0x8c1216c0u);   /* compose proj -> 0x2D6AD8 */
        }
        if(getenv("DBG")){
            for(u32 base=0x2D6900u; base<=0x2D690Cu; base+=0xC){
                u32 idxmax=r32(&c,0x8C000000u|base), pbase=r32(&c,0x8C000000u|(base+4)), ptop=r32(&c,0x8C000000u|(base+8));
                printf("  desc@%06X: idxmax=%08X base=%08X top=%08X  topmat[0..3]:", base, idxmax, pbase, ptop);
                for(int i=0;i<4;i++){ u32 w=r32(&c, ptop+i*4); printf(" %.3g", *(float*)&w); } printf("\n");
            }
            printf("  0x2D6AD8 now:"); for(int i=0;i<16;i++){ u32 w=r32(&c,0x8C2D6AD8u+i*4); printf(" %.3g", *(float*)&w); } printf("\n");
        }
        printf("executor tick: %ld dispatches%s\n", g_calls,
               !strcmp(mode,"tickcam") ? " (+carried camera proj @0x2D6AD8)" :
               !strcmp(mode,"tickproj")? " (+ran loc_8c1216c0 proj composer)" : "");
    }

    /* render the (post-tick) resident state */
    render_frame_reset();
    Sh4Ctx rc; memset(&rc,0,sizeof rc); rc.ram=ram; rc.r[15]=0x8CFF0000u;
    render_frame(&rc);
    int n = render_frame_nscene();
    unsigned long qb = render_frame_quad_bytes();
    FILE *o=fopen(out,"wb"); if(o){ fwrite(render_frame_scene(),qb,(size_t)n,o); fclose(o); }
    printf("scene: %d quads (%lu B/quad) -> %s\n", n, qb, out);
    return 0;
}
