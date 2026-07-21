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

static u8 ram[RAM_SIZE];
static int load(const char *p){ FILE *f=fopen(p,"rb"); if(!f) return 0;
    size_t n=fread(ram,1,RAM_SIZE,f); fclose(f); return n==RAM_SIZE; }

int main(int argc, char **argv){
    const char *rp   = argc>1 ? argv[1] : "_ram_f90.bin";
    const char *mode = argc>2 ? argv[2] : "tick";
    const char *out  = argc>3 ? argv[3] : "scene_out.bin";
    if(!load(rp)){ printf("cannot load %s\n", rp); return 2; }

    Sh4Ctx c; memset(&c,0,sizeof c); c.ram=ram; c.r[15]=0x8CFF0000u;
    if(!strcmp(mode,"tick") || !strcmp(mode,"tickcam")){
        /* tickcam: the game-logic tick transiently zeroes the render-walk's projection
         * matrix @0x2D6AD8 (the render subtree normally re-deposits it). Until we chain
         * the render-walk proj setup (loc_8c1216c0), carry the camera projection across
         * the tick so render_frame's world->screen transform has a valid matrix.
         * The viewport @0x2D6B18 is already game-tick-stable, so only 0x2D6AD8 is carried. */
        u8 projsave[64];
        if(!strcmp(mode,"tickcam")) memcpy(projsave, ram+0x002D6AD8u, 64);
        tick_entry(&c);                 /* input -> next-frame game state (in-place) */
        if(!strcmp(mode,"tickcam")) memcpy(ram+0x002D6AD8u, projsave, 64);
        printf("executor tick: %ld dispatches%s\n", g_calls,
               !strcmp(mode,"tickcam") ? " (+carried camera proj @0x2D6AD8)" : "");
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
