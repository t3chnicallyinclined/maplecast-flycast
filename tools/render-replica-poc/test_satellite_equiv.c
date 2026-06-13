/* test_satellite_equiv.c — FAITHFULNESS PROOF for the loc_8c030af8 satellite setup.
 *
 * The slot-walk renders a cat 1..4 satellite through loc_8c030af8, which deposits the SAME
 * per-frame walker fields (node+0xE0/E4 anchor, +0xEC/F0 scale, +0x104/130/134/136) that
 * the cat==0 body path loc_8c03093c deposits — the field-by-field disasm comparison
 * (gen_render_satellite.py) shows the math is byte-identical for the non-zoom path a
 * body-sprite satellite takes (node+0x14D==0). The body path is ALREADY validated to 0.00px
 * vs the ASMTRACE/CHARQ ground truth. So if, on the SAME node, the satellite setup deposits
 * BIT-IDENTICAL +0xE0..+0x136 to the body setup, the satellite renders the SAME geometry the
 * body would — proven against ground truth transitively, with NO need for a live satellite
 * frame. (A live satellite, when one spawns, is also checked by _diff_sat.mjs.)
 *
 * This loads a real engine RAM frame (mc_ram_dump.bin OR a frame extracted from an .mcrr),
 * picks the real cat==0 body node, runs both setups on a fresh copy of that node, and diffs
 * the deposited fields. Build: see run note at bottom.
 */
#include "sh4ctx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void render_object_setup_03093c(Sh4Ctx *c);   /* loc_8c03093c body     */
void render_object_setup_030af8(Sh4Ctx *c);   /* loc_8c030af8 satellite */

static u32 RD32(u8* ram, u32 a){ a&=0xFFFFFF; return ram[a]|(ram[a+1]<<8)|(ram[a+2]<<16)|((u32)ram[a+3]<<24); }
static u16 RD16(u8* ram, u32 a){ a&=0xFFFFFF; return ram[a]|(ram[a+1]<<8); }

int main(int argc, char** argv){
    if(argc<3){ fprintf(stderr,"usage: %s <ram16.bin> <nodeHex>\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1],"rb"); if(!f){ perror("open"); return 2; }
    static u8 ram[16*1024*1024];
    size_t n = fread(ram,1,sizeof ram,f); fclose(f);
    fprintf(stderr,"loaded %zu bytes\n", n);
    u32 node = (u32)strtoul(argv[2],0,16);

    /* the walker fields both setups deposit */
    const u32 OFFS[] = {0xE0,0xE4,0xE8,0xEC,0xF0,0x104,0x130,0x134,0x136,0x100,0x110};
    const int NO = (int)(sizeof OFFS/sizeof OFFS[0]);

    fprintf(stderr,"node 0x%08x: cat=%u +0x12c=%u +0x14D(zoom gate)=%u +0x144 sid=%u\n",
        node, ram[(node&0xFFFFFF)+3], ram[(node&0xFFFFFF)+0x12c], ram[(node&0xFFFFFF)+0x14d], RD16(ram,node+0x144));

    /* --- run BODY setup on a copy --- */
    static u8 ramA[16*1024*1024]; memcpy(ramA, ram, sizeof ram);
    Sh4Ctx ca; memset(&ca,0,sizeof ca); ca.ram=ramA;
    ca.r[4]=node; ca.r[14]=node; ca.r[15]=0x0C480000u; ca.pr=0xDEADBEEFu;
    render_object_setup_03093c(&ca);

    /* --- run SATELLITE setup on a fresh copy of the SAME node --- */
    static u8 ramB[16*1024*1024]; memcpy(ramB, ram, sizeof ram);
    Sh4Ctx cb; memset(&cb,0,sizeof cb); cb.ram=ramB;
    cb.r[4]=node; cb.r[14]=node; cb.r[15]=0x0C480000u; cb.pr=0xDEADBEEFu;
    render_object_setup_030af8(&cb);

    int diffs=0;
    printf("off    body(93c)        satellite(030af8)   match\n");
    for(int i=0;i<NO;i++){
        u32 a = RD32(ramA, node+OFFS[i]);
        u32 b = RD32(ramB, node+OFFS[i]);
        float fa=*(float*)&a, fb=*(float*)&b;
        int ok = (a==b);
        if(!ok) diffs++;
        printf("+0x%-4x 0x%08x(%g)  0x%08x(%g)  %s\n", OFFS[i], a, fa, b, fb, ok?"OK":"<<DIFF");
    }
    printf("\n%s: %d/%d deposited walker fields BIT-IDENTICAL between body and satellite setup\n",
        diffs==0?"PASS":"REVIEW", NO-diffs, NO);
    return diffs?1:0;
}
