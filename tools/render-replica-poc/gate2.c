/* Instrumented variant of gate.c: keeps a REAL call stack (mc_push/mc_pop) so we can
 * print the looping function F and its full call chain at hang, and log every (caller->target)
 * pair for the first M dispatches after the jump input arrives. */
#include "sh4ctx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
void tick_entry(Sh4Ctx *c);

static volatile int g_frame = -1;
extern u32 mc_curfn;

/* real call stack */
static u32 g_stk[512]; static volatile int g_sp = 0;
static long g_calls = 0;
int  mc_call_guard(void){ return (++g_calls > 200000L) ? 1 : 0; }
void mc_unknown_call(u32 a){ (void)a; }
void mc_unk_regs(u32 *r){ (void)r; }
u32  mc_curfn = 0;
void mc_push(u32 a){ if(g_sp < 512) g_stk[g_sp]=a; g_sp++; }
void mc_pop(void){ if(g_sp>0) g_sp--; }

static DWORD WINAPI watchdog(LPVOID p){ (void)p;
    Sleep(4000);
    fprintf(stderr,"\n[WD] HUNG frame %d calls=%ld sp=%d\n", g_frame, g_calls, g_sp);
    fprintf(stderr,"[WD] call stack (top last = the looping F):\n");
    int lo = g_sp-24; if(lo<0) lo=0;
    for(int i=lo;i<g_sp && i<512;i++) fprintf(stderr,"   [%d] 0x%08X\n", i, g_stk[i]);
    fflush(stderr); ExitProcess(3); return 0;
}

/* dispatch hook: log caller(=stack top before this push already happened) -> target for a window */
static long g_logged=0;
void mc_dispatch_hook(u32 a, Sh4Ctx*c){
    /* at hook time mc_push(a) already ran, so g_stk[g_sp-1]==a, caller = g_stk[g_sp-2] */
    static int phase=0, done=0, nlog=0;   /* phase=1 once we enter the node=268340 handler */
    static u32 sp15_at[600];              /* r15 at entry, keyed by g_sp */
    if(g_frame==20 && !done){
        u32 caller = (g_sp>=2)? g_stk[g_sp-2] : 0;
        if(caller==0x8C043CDC && a==0x8C04761Cu && c->r[12]==0x8C268340u){ phase=1; }
        if(phase){
            if(g_sp<600) sp15_at[g_sp]=c->r[15];
            if(nlog<260){
                fprintf(stderr,"T sp=%d %08X->%08X r13=%08X r15=%08X\n", g_sp, caller, a, c->r[13], c->r[15]);
                nlog++;
            }
            if(caller==0x8C043CDC && a==0x8C268340u){ fprintf(stderr,"### handler RETURNED with r13=268340 (loop begins)\n"); done=1; }
        }
    }
}

/* r15 balance report (patched call_addr in gen_tick_bal.c calls this on imbalance) */
static int g_baln=0;
void mc_bal(u32 a, u32 sp0, u32 sp1, int flag){ (void)flag;
    if(g_frame==20 && g_baln<40){
        u32 caller = (g_sp>=2)? g_stk[g_sp-2] : 0;
        fprintf(stderr,"IMBAL sp=%d caller=0x%08X fn=0x%08X  r15 %08X->%08X (delta %+d)\n",
            g_sp, caller, a, sp0, sp1, (int)(sp1-sp0));
        g_baln++;
    }
}

static u8 ram[32u*1024u*1024u];

static u16 demo_input(int f){
    const char*mode=getenv("MC_INPUT");
    if(mode && !strcmp(mode,"jump")) return (f%50>=20 && f%50<28) ? 0x2000 : 0;
    return 0;
}

int main(int argc,char**argv){
    const char*rp = argc>1?argv[1]:"_ram_f90.bin"; int N = argc>2?atoi(argv[2]):300;
    FILE*f=fopen(rp,"rb"); if(!f){ printf("no %s\n",rp); return 2; }
    fread(ram,1,RAM_SIZE,f); fclose(f);
    Sh4Ctx c; memset(&c,0,sizeof c); c.ram=ram; c.r[15]=0x8CFF0000u;
    if(getenv("MC_WATCHDOG")) CreateThread(NULL,0,watchdog,NULL,0,NULL);
    u16 prev=0;
    for(int fr=0; fr<N; fr++){
        g_frame=fr;
        *(u32*)(ram+0x1F9D98)=0; *(u32*)(ram+0x1F9D94)=16;
        u16 cur=demo_input(fr);
        u8*id=ram+0x2681DC;
        *(u16*)(id+0)=cur; *(u16*)(id+2)=prev; *(u16*)(id+4)=(u16)(cur&~prev); *(u16*)(id+6)=(u16)(prev&~cur);
        prev=cur;
        g_calls=0; g_sp=0;
        tick_entry(&c);
        if(fr%50==0){ printf("[gate2] f%d calls=%ld\n", fr, g_calls); fflush(stdout); }
    }
    printf("[DONE] %d frames clean\n", N);
    return 0;
}
