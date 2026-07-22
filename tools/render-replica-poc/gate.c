/* COMPLETENESS / DETERMINISM GATE — in the BYTE-EXACT reference build (zig/clang -O2, the same
 * build proven byte-exact vs flycast). Drives the transpiled game-tick over an input-rich
 * sequence (walk/jump/crouch/all attacks/QCF/QCB/DP + double-motion supers) and reports, per
 * frame: dispatch count, ROM/hardware reads (MC_RTRAP -> mc_note_read for any non-area-3 read),
 * and NaN positions. A clean run = strong evidence the tick can be the AUTHORITATIVE server.
 * Build: zig cc -O2 -DMC_RTRAP gate.c gen_tick_all.c gen_leaf.c -lm
 * Run:   ./gate.exe _ram_f90.bin 1000                                                          */
#include "sh4ctx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
void tick_entry(Sh4Ctx *c);

/* Watchdog: an intra-function infinite loop never hits the dispatch guard, so it hangs. This
 * thread fires after 6s and prints the function the executor is stuck in (mc_curfn = the codegen's
 * current-function tag; g_last_dispatched = last call_addr target) — the culprit to fix. */
static volatile u32 g_last_dispatched = 0;
static volatile int g_frame = -1;
static volatile u32 g_ring[16]; static volatile int g_ring_n = 0;  /* last distinct dispatch targets */
extern u32 mc_curfn;
static long g_calls_fwd(void);
static DWORD WINAPI watchdog(LPVOID p){ (void)p;
    Sleep(6000);
    fprintf(stderr, "\n[WATCHDOG] HUNG at frame %d — stuck dispatching 0x%08X, calls=%ld\n",
        g_frame, g_last_dispatched, g_calls_fwd());
    fprintf(stderr, "[WATCHDOG] dispatch trail (last distinct targets, the caller precedes the bad one):\n");
    int n = g_ring_n < 16 ? g_ring_n : 16;
    for(int i=0;i<n;i++){ int idx=(g_ring_n-n+i)%16; fprintf(stderr,"    0x%08X\n", g_ring[idx]); }
    fflush(stderr); ExitProcess(3); return 0;
}

/* runtime stubs. Cap dispatch at 200k (a real in-match tick is ~2000) so a genuine runaway
 * aborts in a few ms and is flagged, instead of hanging on the 50M-style guard. */
static long g_calls = 0;
int  mc_call_guard(void){ return (++g_calls > 200000L) ? 1 : 0; }
void mc_unknown_call(u32 a){ (void)a; }
void mc_unk_regs(u32 *r){ (void)r; }
u32  mc_curfn = 0;
void mc_push(u32 a){
    g_last_dispatched = a;
    if(g_ring_n==0 || g_ring[(g_ring_n-1)%16] != a){ g_ring[g_ring_n%16]=a; g_ring_n++; }
}
void mc_pop(void){}
static long g_calls_fwd(void){ return g_calls; }

/* non-RAM (ROM/hardware) census — the completeness signal. g_nr_set is the WHOLE-run footprint. */
static long g_nonram = 0;
static u32  g_nr_set[64]; static int g_nr_n = 0; static u32 g_nr_fn = 0, g_nr_first_f = 0;
void mc_note_read(u32 a, u32 n){ (void)n;
    if(!g_nonram) g_nr_fn = mc_curfn;
    g_nonram++;
    for(int i=0;i<g_nr_n;i++) if(g_nr_set[i]==a) return;
    if(g_nr_n<64) g_nr_set[g_nr_n++] = a;
}

static u8 ram[32u*1024u*1024u];

/* CPS2 active-high input bits: U=0x2000 D=0x1000 L=0x0800 R=0x0400 | LP=0x0200 HP=0x0100
 * LK=0x0040 HK=0x0020 | A1=0x0080 A2=0x0010. 18-frame phases; specials/supers scripted. */
static u16 demo_input(int f){
    const char*mode=getenv("MC_INPUT");
    if(mode){
        if(!strcmp(mode,"coast")) return 0;                                  /* no input at all */
        if(!strcmp(mode,"jump"))  return (f%50>=20 && f%50<28) ? 0x2000 : 0; /* realistic idle->jump->idle */
        if(!strcmp(mode,"walk"))  return (f%40>=10 && f%40<30) ? 0x0400 : 0; /* idle->walk->idle */
    }
    int p=(f/18)%18, m=f%18;
    switch(p){
        case 0: return 0; case 1: return 0x0400; case 2: return 0x0800; case 3: return 0x2000;
        case 4: return 0x1000; case 5: return 0x0200; case 6: return 0x0100; case 7: return 0x0040;
        case 8: return 0x0020; case 9: return 0x0080; case 10: return 0x0010;
        case 11: return (m<2)?0x1000:(m<4)?0x1400:(m<6)?0x0400:0x0100;                 /* QCF+HP */
        case 12: return (m<2)?0x1000:(m<4)?0x1800:(m<6)?0x0800:0x0100;                 /* QCB+HP */
        case 13: return (m<2)?0x0400:(m<4)?0x1000:(m<6)?0x1400:0x0100;                 /* DP+HP */
        case 14: { u16 s[6]={0x1000,0x1400,0x0400,0x1000,0x1400,0x0400}; return (m<6)?s[m]:0x0100; } /* QCFx2 super */
        case 15: { u16 s[6]={0x1000,0x1800,0x0800,0x1000,0x1800,0x0800}; return (m<6)?s[m]:0x0100; } /* QCBx2 super */
        case 16: return 0x0400|0x0100; default: return 0x0080|0x0010;
    }
}

int main(int argc,char**argv){
    const char*rp = argc>1?argv[1]:"_ram_f90.bin"; int N = argc>2?atoi(argv[2]):1000;
    FILE*f=fopen(rp,"rb"); if(!f){ printf("no %s\n",rp); return 2; }
    fread(ram,1,RAM_SIZE,f); fclose(f);
    Sh4Ctx c; memset(&c,0,sizeof c); c.ram=ram; c.r[15]=0x8CFF0000u;
    if(getenv("MC_WATCHDOG")) CreateThread(NULL,0,watchdog,NULL,0,NULL);
    u16 prev=0; int gaps=0; long mind=1L<<30, maxd=0;
    for(int fr=0; fr<N; fr++){
        g_frame=fr;
        *(u32*)(ram+0x1F9D98)=0; *(u32*)(ram+0x1F9D94)=16;
        u16 cur=demo_input(fr);
        u8*id=ram+0x2681DC;
        *(u16*)(id+0)=cur; *(u16*)(id+2)=prev; *(u16*)(id+4)=(u16)(cur&~prev); *(u16*)(id+6)=(u16)(prev&~cur);
        prev=cur;
        g_calls=0; g_nonram=0;
        tick_entry(&c);
        long disp=g_calls;
        if(disp<mind)mind=disp; if(disp>maxd)maxd=disp;
        float x1=*(float*)(ram+0x268340+0x34), y1=*(float*)(ram+0x268340+0x38);
        float x2=*(float*)(ram+0x2688E4+0x34), y2=*(float*)(ram+0x2688E4+0x38);
        int nan = (x1!=x1)||(y1!=y1)||(x2!=x2)||(y2!=y2);
        if(disp>=200000 || disp<300 || nan){
            printf("[GAP] f%d phase%d in=0x%04X disp=%ld nan=%d\n", fr,(fr/18)%18,cur,disp,nan);
            fflush(stdout); gaps++;
        }
        if(fr%100==0){ printf("[gate] f%d disp=%ld\n", fr, disp); fflush(stdout); }
    }
    printf("\n[DONE] %d input-rich frames | dispatch[%ld..%ld] | %d gaps\n", N, mind, maxd, gaps);
    printf("[DONE] non-RAM (ROM/hw) footprint = %d distinct address(es), first reader fn 0x%08X:\n", g_nr_n, g_nr_fn);
    for(int i=0;i<g_nr_n;i++) printf("    0x%08X\n", g_nr_set[i]);
    if(!gaps && g_nr_n<=4){ printf("[DONE] CLEAN — no collapse/runaway/NaN; fixed small hw footprint (masked, byte-exact)\n"); return 0; }
    return 1;
}
