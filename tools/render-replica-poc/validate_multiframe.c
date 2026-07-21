/* SHADOW-EXECUTOR CONCEPT PROOF (offline, multi-frame).
 * The live shim will feed each render-boundary RAM snapshot through the executor and diff
 * the GAME-STATE regions vs the next frame's snapshot. Here we do exactly that across the 4
 * consecutive captures _ram_f90..f93 (3 transitions), to prove the concept before building
 * the live shim. Game-state regions = the same ones gameStateRegionHash() uses on the server
 * (char structs 0x268340, global page 0x289000) + camera 0x26A520, minus the known
 * hw-timer / vsync / render-anim bytes the game-logic tick legitimately doesn't own. */
#include "sh4ctx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void tick_entry(Sh4Ctx *c);
static long g_calls = 0;
int  mc_call_guard(void){ return (++g_calls > 5000000L) ? 1 : 0; }
void mc_unknown_call(u32 a){ (void)a; }
void mc_unk_regs(u32 *r){ (void)r; }
u32  mc_curfn = 0; void mc_push(u32 a){ (void)a; } void mc_pop(void){}

static u8 work[RAM_SIZE], truth[RAM_SIZE];
static int load(const char *p, u8 *d){ FILE *f=fopen(p,"rb"); if(!f) return 0;
    size_t n=fread(d,1,RAM_SIZE,f); fclose(f); return n==RAM_SIZE; }

/* per-char render-anim counter char+0x502 (6 chars) is written by the EXCLUDED render walker;
 * frame_counter 0x3496B0 by vsync; the 3 hw-timer mirrors are masked project-wide. */
static int masked(u32 off){
    if (off==0x2D5748||off==0x2D5749||off==0x2D574A||off==0x2D574B) return 1;
    if (off==0x32DBAC||off==0x32DBAD||off==0x32DBAE||off==0x32DBAF) return 1;
    if (off==0x268250) return 1;                             /* fight-tick */
    if (off>=0x3496B0 && off<=0x3496B3) return 1;             /* frame_counter (vsync) */
    if (off>=0x2895E0 && off<=0x2895EF) return 1;            /* slot-table count[16] = render draw list (loc_8c0308c2 rebuilds each frame) */
    for (int k=0;k<6;k++){ u32 cb=0x268340u + (u32)k*0x5A4u;
        if (off==cb+0x502u || off==cb+0x503u) return 1;      /* +0x502 render anim */
        if (off>=cb+0x0E0u && off<=cb+0x0EBu) return 1;      /* +0xE0/E4/E8 screen x/y/z (render-deposited) */
    }
    return 0;
}

/* diff one game-state region, return #differing bytes (masked), print first few */
static long diffregion(const char *name, u32 base, u32 len){
    long d=0; u32 first=0;
    for (u32 o=base; o<base+len; o++){ if (masked(o)) continue;
        if (work[o]!=truth[o]) { if(!d) first=o; d++; } }
    if (d) printf("    %-14s DIFF %ld bytes (first @0x8C%06X)\n", name, d, first);
    return d;
}

int main(int argc, char **argv){
    const char *frames[] = {"_ram_f90.bin","_ram_f91.bin","_ram_f92.bin","_ram_f93.bin"};
    int nf = 4;
    long grand = 0;
    for (int i=0; i+1<nf; i++){
        if(!load(frames[i], work) || !load(frames[i+1], truth)){
            printf("cannot load %s / %s\n", frames[i], frames[i+1]); return 2; }
        g_calls = 0;
        Sh4Ctx c; memset(&c,0,sizeof c); c.ram=work; c.r[15]=0x8CFF0000u;
        tick_entry(&c);
        printf("transition %s -> %s  (%ld dispatches):\n", frames[i], frames[i+1], g_calls);
        long d = 0;
        d += diffregion("char structs", 0x268340u, 6u*0x5A4u);
        d += diffregion("global page",  0x289000u, 0x1000u);
        d += diffregion("camera",       0x26A520u, 0x60u);
        if (!d) printf("    -> GAME-STATE BYTE-EXACT (0 differing bytes)\n");
        grand += d;
    }
    printf("\nTOTAL game-state divergence across %d transitions: %ld bytes  %s\n",
           nf-1, grand, grand? "" : "-> SHADOW CONCEPT VALIDATED");
    return 0;
}
