/* engine_loop — persistent executor+render engine for the LIVE PLAYABLE client. Reads a u16 CPS2
 * input from stdin per frame, advances the byte-exact tick, renders (render_frame + PROJCARRY),
 * writes the scene to stdout: u32 n | quads(n*88) | srcdesc(n*4) | colrow(n*8) | effect(n*1).
 * live_play.py drives it (keyboard->input, scene->decode_body->window). No flycast.
 * Build: zig cc -O2 -w -o engine_loop.exe engine_loop.c gen_tick_all.c render_frame.c gen_walker.c \
 *   gen_walker_root.c gen_walker_scale.c gen_render_object.c gen_render_satellite.c gen_transform.c \
 *   gen_transform_obj.c gen_submit.c gen_submit_params.c gen_leaf.c -lm */
#include "sh4ctx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
  #include <fcntl.h>
  #include <io.h>
#endif
void tick_entry(Sh4Ctx *c);
void render_frame_reset(void);
void render_frame(Sh4Ctx *c);
int  render_frame_nscene(void);
const void* render_frame_scene(void);
unsigned long render_frame_quad_bytes(void);
unsigned int render_frame_quad_srcdesc_impl(unsigned char*, unsigned int);
unsigned int render_frame_quad_colrow_impl(int*, unsigned int);
unsigned int render_frame_quad_is_effect_impl(unsigned char*, unsigned int);

static long g_calls=0;
int  mc_call_guard(void){ return (++g_calls>5000000L)?1:0; }
void mc_unknown_call(u32 a){ (void)a; }
void mc_unk_regs(u32*r){ (void)r; }
u32  mc_curfn=0; void mc_push(u32 a){ (void)a; } void mc_pop(void){}

static unsigned char ram[32u*1024u*1024u], scratch[32u*1024u*1024u], projsave[64];

int main(int argc,char**argv){
    const char* rp = argc>1?argv[1]:"_ram_f90.bin";
    FILE* f=fopen(rp,"rb"); if(!f){ return 2; } fread(ram,1,RAM_SIZE,f); fclose(f);
    memcpy(projsave, ram+0x002D6AD8u, 64);       /* the seed's valid render projection (PROJCARRY) */
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY);
#endif
    Sh4Ctx c; memset(&c,0,sizeof c); c.ram=ram; c.r[15]=0x8CFF0000u;
    unsigned short prev=0, cur;
    static unsigned char sd[1024*4]; static int cr[1024*2]; static unsigned char ie[1024];
    while(fread(&cur,2,1,stdin)==1){
        unsigned char* id=ram+0x2681DC;           /* inject P1 Input_DEC (edges) */
        *(unsigned short*)(id+0)=cur; *(unsigned short*)(id+2)=prev;
        *(unsigned short*)(id+4)=(unsigned short)(cur&~prev); *(unsigned short*)(id+6)=(unsigned short)(prev&~cur);
        prev=cur;
        *(u32*)(ram+0x1F9D98)=0; *(u32*)(ram+0x1F9D94)=16;   /* arena frame-setup */
        g_calls=0; tick_entry(&c);                            /* byte-exact authoritative tick */
        ram[0x289F80]=ram[0x289F81]=ram[0x289F82]=ram[0x289F83]=0;
        memcpy(scratch, ram, sizeof ram);
        memcpy(scratch+0x002D6AD8u, projsave, 64);
        for(int L=0;L<16;L++) if(scratch[0x2895E0+L]>0x60) scratch[0x2895E0+L]=0x60;
        render_frame_reset();
        { Sh4Ctx rc; memset(&rc,0,sizeof rc); rc.ram=scratch; rc.r[15]=0x8CFF0000u; render_frame(&rc); }
        int n=render_frame_nscene(); if(n<0)n=0; if(n>1024)n=1024;
        render_frame_quad_srcdesc_impl(sd,1024); render_frame_quad_colrow_impl(cr,1024); render_frame_quad_is_effect_impl(ie,1024);
        unsigned un=(unsigned)n;
        fwrite(&un,4,1,stdout);
        fwrite(render_frame_scene(), render_frame_quad_bytes(), (size_t)n, stdout);
        fwrite(sd,4,(size_t)n,stdout); fwrite(cr,8,(size_t)n,stdout); fwrite(ie,1,(size_t)n,stdout);
        fflush(stdout);
    }
    return 0;
}
