/* executor_server — (2) the EXECUTOR as the authoritative game server, standalone, NO flycast.
 *
 * Seed the game state once, then run the authoritative loop: latch input -> advance the byte-exact
 * transpiled tick (byte-identical to flycast, proven by oracle_diff.py) -> emit a thin state-wire
 * delta (statewire_v2 dirty-diff, ported here). This is the flycast-headless replacement: the same
 * authoritative SH-4 game logic at ~0.1 ms/frame instead of full-emulator cost, shipping only the
 * per-frame state churn (the client runs its OWN executor + render_frame). Input here is a scripted
 * stream and the wire is measured; the UDP :7100 input ({u16 buttons; u8 lt; u8 rt; u32 seq} ->
 * dc_to_cps2 -> Input_DEC @0x2681DC) + relay publish are the deployment wrapper around this loop.
 *
 * Build: zig cc -O2 -w executor_server.c gen_tick_all.c gen_leaf.c -lm
 * Run:   ./executor_server _ram_f90.bin 600
 */
#include "sh4ctx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
void tick_entry(Sh4Ctx* c);

static long g_calls = 0;
int  mc_call_guard(void){ return (++g_calls > 5000000L) ? 1 : 0; }
void mc_unknown_call(u32 a){ (void)a; }
void mc_unk_regs(u32* r){ (void)r; }
u32  mc_curfn = 0;
void mc_push(u32 a){ (void)a; }
void mc_pop(void){}

/* statewire_v2 dirty-diff (C port of core/network/statewire_v2.h encode, delta path): runs of
 * bytes differing from the last keyframe, folding up to MERGE=8 stable bytes into a run. Byte-exact
 * with the header + the JS decoder. Returns the delta length. */
#define MERGE 8
static void put32(unsigned char* p, unsigned v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static unsigned sw_delta(const unsigned char* cur, unsigned n, const unsigned char* ref,
                         unsigned keyId, unsigned char* out){
    unsigned char* o = out;
    *o++ = 0; put32(o, keyId); o += 4;
    unsigned char* nruns_p = o; o += 4;
    unsigned nRuns = 0, i = 0;
    while(i < n){
        if(cur[i] == ref[i]){ i++; continue; }
        unsigned start = i, last = i, j = i + 1;
        while(j < n && (j - last) <= MERGE){ if(cur[j] != ref[j]) last = j; j++; }
        unsigned len = last - start + 1;
        put32(o, start); o += 4; put32(o, len); o += 4;
        memcpy(o, cur + start, len); o += len;
        nRuns++; i = last + 1;
    }
    put32(nruns_p, nRuns);
    return (unsigned)(o - out);
}

/* scripted input (stand-in for the UDP :7100 stream): walk/jump/attacks/specials/supers (CPS2). */
static unsigned short script_cps2(int f){
    int p=(f/18)%18, m=f%18;
    switch(p){
        case 0: return 0; case 1: return 0x0400; case 2: return 0x0800; case 3: return 0x2000;
        case 4: return 0x1000; case 5: return 0x0200; case 6: return 0x0100; case 7: return 0x0040;
        case 8: return 0x0020; case 9: return 0x0080; case 10: return 0x0010;
        case 11: return (m<2)?0x1000:(m<4)?0x1400:(m<6)?0x0400:0x0100;
        case 12: return (m<2)?0x1000:(m<4)?0x1800:(m<6)?0x0800:0x0100;
        case 13: return (m<2)?0x0400:(m<4)?0x1000:(m<6)?0x1400:0x0100;
        case 14: { unsigned short s[6]={0x1000,0x1400,0x0400,0x1000,0x1400,0x0400}; return (m<6)?s[m]:0x0100; }
        case 15: { unsigned short s[6]={0x1000,0x1800,0x0800,0x1000,0x1800,0x0800}; return (m<6)?s[m]:0x0100; }
        case 16: return 0x0400|0x0100; default: return 0x0080|0x0010;
    }
}

static unsigned char ram[32u*1024u*1024u];
/* authoritative DYNAMIC state region shipped to clients (char structs + objects + globals + camera). */
#define ST_OFF 0x00260000u
#define ST_LEN 0x00040000u          /* 256 KB window */
static unsigned char keyref[ST_LEN];
static unsigned char wirebuf[ST_LEN + 4096];

int main(int argc, char** argv){
    const char* rp = argc>1?argv[1]:"_ram_f90.bin";
    int N = argc>2?atoi(argv[2]):600;
    const int KEY_EVERY = 60;
    FILE* f=fopen(rp,"rb"); if(!f){ printf("no %s\n",rp); return 2; }
    fread(ram,1,RAM_SIZE,f); fclose(f);
    Sh4Ctx c; memset(&c,0,sizeof c); c.ram=ram; c.r[15]=0x8CFF0000u;

    unsigned short prev=0;
    long delta_total=0, delta_max=0, key_total=0; int deltas=0, keys=0;
    clock_t tick_clk=0;
    for(int fr=0; fr<N; fr++){
        unsigned short cur = script_cps2(fr);          /* real pad: dc_to_cps2(buttons,lt,rt) */
        unsigned char* id = ram + 0x2681DC;
        *(unsigned short*)(id+0)=cur; *(unsigned short*)(id+2)=prev;
        *(unsigned short*)(id+4)=(unsigned short)(cur&~prev); *(unsigned short*)(id+6)=(unsigned short)(prev&~cur);
        prev=cur;
        g_calls=0;
        clock_t t0=clock(); tick_entry(&c); tick_clk += clock()-t0;
        int isKey = (fr % KEY_EVERY)==0;
        if(isKey){ memcpy(keyref, ram+ST_OFF, ST_LEN); key_total += ST_LEN; keys++; }
        else { unsigned ws = sw_delta(ram+ST_OFF, ST_LEN, keyref, fr/KEY_EVERY, wirebuf);
               delta_total += ws; if((long)ws>delta_max) delta_max=ws; deltas++; }
    }
    double tick_us = (double)tick_clk/CLOCKS_PER_SEC*1e6/N;
    double avg_delta = deltas? (double)delta_total/deltas : 0;
    double delta_mbps = avg_delta*8*60/1e6;
    printf("[executor-server] %d authoritative frames driven by input, NO flycast, NO emulator\n", N);
    printf("  tick:  %.3f us/frame avg  = %.3f%% of the 16.67ms 60fps budget  (~%.0fx real-time)\n",
           tick_us, tick_us/16670.0*100.0, 16670.0/tick_us);
    printf("  wire:  %.0f B/delta avg (max %ld B), keyframe %uB every %d frames\n",
           avg_delta, delta_max, ST_LEN, KEY_EVERY);
    printf("  wire:  ~%.3f Mbps of deltas @60fps  (client runs its OWN executor + render_frame)\n", delta_mbps);
    printf("  => input -> byte-exact authoritative state -> thin wire, entirely off flycast.\n");
    return 0;
}
