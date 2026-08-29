/* readset_probe.c — WIRE READ-SET COMPLETENESS DIAGNOSTIC (2026-07-02).
 * Runs render_frame() on a full RAM dump with MC_READLOG on, so every guest RAM byte the
 * transpiled render (walker + transform + effect/scale walk + submit) TOUCHES is recorded.
 * Coalesces the touched bytes into address RANGES = render_frame's TRUE read-set. Then diffs
 * against the WIRE read-set (the D()/S() regions maplecast_replica_live.cpp buildTables ships).
 * ANY range render reads that the wire does NOT ship-per-frame = a GAP = the live garble.
 *
 * NOTE: the client also splats the WHOLE 16MB RAM ONCE in the prefix, so a gap only MATTERS
 * if that region MUTATES per frame (else the stale prefix value is correct). This probe reports
 * the read-set; the frame-diff (changed addresses) intersect is the second tool. */
#include "sh4ctx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

unsigned char mc_readbmp[0x1000000/8];   /* the coverage bitmap (defined here) */

void render_frame(Sh4Ctx *c);
void render_frame_reset(void);
int  render_frame_nscene(void);

static u8 ram[RAM_SIZE];

/* The WIRE per-frame DYNAMIC read-set (D() regions in buildTables), transcribed. Static (GFX1/
 * GFX2 + the 16MB ram16 prefix) is shipped ONCE — flagged separately. addr,len,tag,perFrame. */
typedef struct { u32 addr, len; const char* tag; int perFrame; } WireReg;
static WireReg WIRE[] = {
    {0x8C2895E0,0x10,      "slot_cnt",1},
    {0x8C287DE0,16*0x180,  "slot_ptr",1},
    {0x8C289620,0x60,      "gstate",1},
    {0x8C2895C0,0x60,      "battle",1},
    {0x8C268340,6*0x5A4,   "char_str",1},
    {0x8C26AA54,0x1D000,   "objpool",1},
    {0x8C1F9D80,0x20,      "arena",1},
    {0x8C1F9F9C,0x1800,    "tiledesc",1},
    {0x8C2D6AD8,0xC0,      "cam_mat",1},
    {0x8C26A510,0x40,      "camZ",1},
    {0x8C26823C,0x04,      "ggp_ptr",1},
    {0x8C268240,0x60,      "ggp_acc",1},
    {0x8C26A974,0x100,     "rparam",1},
    {0x8C2DAD30,0x40,      "tab_ptr",1},
    {0x8C2AA4C0,0x10,      "rmode",1},
    /* idxtab/rectab: pointer-resolved; typical live bases (0x8C2DAxxx family). Filled at runtime
     * from the dump's 0x8C2DAD3C/0x8C2DAD4C pointers below. */
    {0,0x2000,             "idxtab",1},
    {0,0x10000,            "rectab",1},
    /* effect templates (7 * 0x3000) */
    {0x8C565000,0x3000,"efxtmpl",1},{0x8C955000,0x3000,"efxtmpl",1},{0x8C6B5000,0x3000,"efxtmpl",1},
    {0x8CAA5000,0x3000,"efxtmpl",1},{0x8C805000,0x3000,"efxtmpl",1},{0x8CBF5000,0x3000,"efxtmpl",1},
    {0x8CD45000,0x3000,"efxtmpl",1},
};
#define NWIRE (int)(sizeof(WIRE)/sizeof(WIRE[0]))

static int inWire(u32 a){
    for (int i=0;i<NWIRE;i++){ if (WIRE[i].addr==0) continue;
        if (a >= (WIRE[i].addr&0x00FFFFFF) && a < ((WIRE[i].addr&0x00FFFFFF)+WIRE[i].len)) return 1; }
    return 0;
}
static const char* wireTag(u32 a){
    for (int i=0;i<NWIRE;i++){ if (WIRE[i].addr==0) continue;
        if (a >= (WIRE[i].addr&0x00FFFFFF) && a < ((WIRE[i].addr&0x00FFFFFF)+WIRE[i].len)) return WIRE[i].tag; }
    return "(NOT-SHIPPED-per-frame)";
}

static int bit(u32 i){ return (mc_readbmp[i>>3]>>(i&7))&1; }

int main(int argc,char**argv){
    const char* path=(argc>1)?argv[1]:"../../../scratchpad/mc_ram_dump.bin";
    FILE*f=fopen(path,"rb"); if(!f){fprintf(stderr,"open %s failed\n",path);return 2;}
    fread(ram,1,RAM_SIZE,f); fclose(f);

    /* resolve idxtab/rectab pointers from the dump */
    u32 idxtab = ram[0x2DAD3C]|(ram[0x2DAD3D]<<8)|(ram[0x2DAD3E]<<16)|(ram[0x2DAD3F]<<24);
    u32 rectab = ram[0x2DAD4C]|(ram[0x2DAD4D]<<8)|(ram[0x2DAD4E]<<16)|(ram[0x2DAD4F]<<24);
    for(int i=0;i<NWIRE;i++){ if(!strcmp(WIRE[i].tag,"idxtab")) WIRE[i].addr=idxtab;
                              if(!strcmp(WIRE[i].tag,"rectab")) WIRE[i].addr=rectab; }

    Sh4Ctx c; memset(&c,0,sizeof c); c.ram=ram;
    memset(mc_readbmp,0,sizeof mc_readbmp);
    render_frame_reset();
    render_frame(&c);
    fprintf(stderr,"render_frame produced %d scene quads; read-set captured.\n", render_frame_nscene());

    /* coalesce touched bytes into ranges (merge gaps <= 64 bytes for readability) */
    printf("=== render_frame TRUE read-set (ranges) vs WIRE per-frame ship ===\n");
    printf("%-10s %-10s %-8s %s\n","start(P1)","end","bytes","wire-region");
    u32 i=0; unsigned long long touchedTotal=0, gapTotal=0;
    while(i<0x1000000){
        if(!bit(i)){i++;continue;}
        u32 s=i; while(i<0x1000000 && (bit(i)|| (i>s && i<s+1 ))) { /* extend, merge <=64B holes */
            if(!bit(i)){ u32 h=i; while(h<0x1000000 && !bit(h) && h-i<64) h++; if(h<0x1000000 && bit(h)){ i=h; continue;} else break; }
            i++;
        }
        u32 e=i; u32 len=e-s; touchedTotal+=len;
        int shipped=inWire(s); const char* tag=wireTag(s);
        /* sample the whole range's ship-coverage: is ANY byte not shipped? */
        int anyGap=0; for(u32 x=s;x<e;x++) if(!inWire(x)){anyGap=1;break;}
        printf("0x%08X 0x%08X %-8u %s%s\n", 0x8C000000u|s, 0x8C000000u|e, len,
               shipped?tag:"** NOT SHIPPED per-frame **", (shipped&&anyGap)?" (PARTIAL)":"");
        if(!shipped) gapTotal+=len;
    }
    printf("\nTOTAL read %llu bytes; NOT-shipped-per-frame %llu bytes.\n", touchedTotal, gapTotal);
    printf("(Static-once regions GFX1/GFX2 + the 16MB ram16 prefix cover the rest — a NOT-SHIPPED\n");
    printf(" range only garbles if it MUTATES per frame; run the frame-diff tool to confirm which.)\n");
    return 0;
}
