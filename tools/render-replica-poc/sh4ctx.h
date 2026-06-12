/* Sh4Ctx + flat-RAM memory model for the Option-C lift-to-C PoC.
 *
 * Memory model: a flat 16MB ram[] representing SH4 area-3 system RAM
 * (guest 0x0C000000..0x0CFFFFFF == phys 0x8C000000..0x8CFFFFFF; the render code
 * runs MMU-off, translate(a)=a&0x1FFFFFFF, area-3 -> ram[a & 0x00FFFFFF]).
 * Guest is BIG-ENDIAN; loads/stores byteswap.
 *
 * This PoC only ever touches area-3 RAM (the node struct + GFX2 + descriptor
 * tables we synthesize there) — no MMIO/VRAM routing needed, matching the
 * Option-C scope note ("tree touches only area-3 RAM+VRAM").
 */
#ifndef SH4CTX_H
#define SH4CTX_H
#include <stdint.h>
#include <math.h>
#include <string.h>

typedef uint8_t  u8;  typedef int8_t  s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;

#define RAM_SIZE (16u*1024u*1024u)

typedef struct {
    u32 r[16];
    float fr[16];
    float xf[16];      /* xmtrx bank (unused by these 2 fns, kept for generality) */
    u32 fpscr, fpul, pr, macl, mach, sr_t, gbr;
    u32 _pool;         /* lifter helper: last mova'd pool word (see codegen) */
    u8 *ram;           /* flat area-3 RAM, RAM_SIZE bytes */
} Sh4Ctx;

/* translate guest virtual -> ram[] index (area-3 only for this PoC) */
static inline u32 mc_idx(u32 a){ return a & 0x00FFFFFFu; }

/* big-endian guest loads */
static inline u32 r32(Sh4Ctx*c, u32 a){
    u32 i=mc_idx(a); u8*p=c->ram+i;
    return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3];
}
static inline u32 r16s(Sh4Ctx*c, u32 a){
    u32 i=mc_idx(a); u8*p=c->ram+i;
    u16 v=((u16)p[0]<<8)|p[1];
    return (u32)(s32)(s16)v;       /* mov.w sign-extends */
}
static inline u32 r16u(Sh4Ctx*c, u32 a){
    u32 i=mc_idx(a); u8*p=c->ram+i; return ((u16)p[0]<<8)|p[1];
}
static inline u32 r8s(Sh4Ctx*c, u32 a){
    u32 i=mc_idx(a); return (u32)(s32)(s8)c->ram[i];  /* mov.b sign-extends */
}
static inline u32 r8u(Sh4Ctx*c, u32 a){ return c->ram[mc_idx(a)]; }

static inline void w32(Sh4Ctx*c, u32 a, u32 v){
    u32 i=mc_idx(a); u8*p=c->ram+i;
    p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v;
}
static inline void w16(Sh4Ctx*c, u32 a, u32 v){
    u32 i=mc_idx(a); u8*p=c->ram+i; p[0]=v>>8; p[1]=v;
}
static inline void w8(Sh4Ctx*c, u32 a, u32 v){ c->ram[mc_idx(a)]=(u8)v; }

#endif
