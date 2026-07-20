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
    u32 sr_q, sr_m;    /* div0s/div1 quotient/mode bits (game-tick divide idiom) */
    u32 _pool;         /* lifter helper: last mova'd pool word (see codegen) */
    u8 *ram;           /* flat area-3 RAM, RAM_SIZE bytes */
} Sh4Ctx;

/* translate guest virtual -> ram[] index (area-3 only for this PoC) */
static inline u32 mc_idx(u32 a){ return a & 0x00FFFFFFu; }

#ifdef MC_READLOG
/* READ-SET CAPTURE (2026-07-02, wire-read-set completeness diagnostic). When compiled with
 * -DMC_READLOG, every guest RAM read through the r32/r16/r8 accessors records the touched byte
 * range into a coverage bitmap (one bit per byte of the 16MB area-3 RAM). The harness dumps the
 * merged address RANGES = render_frame's TRUE read-set, to diff against the wire's D() regions. */
extern unsigned char mc_readbmp[0x1000000/8];   /* 2MB bitmap, one bit/RAM byte */
static inline void mc_readlog(u32 a, u32 n){
    u32 i0 = a & 0x00FFFFFFu;
    for (u32 k = 0; k < n; k++){ u32 i = (i0 + k) & 0x00FFFFFFu; mc_readbmp[i>>3] |= (unsigned char)(1u << (i & 7)); }
}
#define MC_RLOG(a,n) mc_readlog((a),(n))
#else
#define MC_RLOG(a,n) ((void)0)
#endif

#ifdef MC_WRITELOG
/* WRITE-SET CAPTURE (2026-07-20, game-tick executor verification). Symmetric to
 * MC_READLOG: when compiled with -DMC_WRITELOG, every guest RAM store through the
 * w32/w16/w8 accessors records the touched byte range into a coverage bitmap. A
 * verification harness dumps the merged written RANGES = a transpiled function's
 * exact write-set (its output I/O footprint), to diff byte-exact vs flycast /
 * vs the frame snapshot. This is how each self-contained leaf is verified. */
extern unsigned char mc_writebmp[0x1000000/8];   /* 2MB bitmap, one bit/RAM byte */
static inline void mc_writelog(u32 a, u32 n){
    u32 i0 = a & 0x00FFFFFFu;
    for (u32 k = 0; k < n; k++){ u32 i = (i0 + k) & 0x00FFFFFFu; mc_writebmp[i>>3] |= (unsigned char)(1u << (i & 7)); }
}
#define MC_WLOG(a,n) mc_writelog((a),(n))
#else
#define MC_WLOG(a,n) ((void)0)
#endif

/* LITTLE-ENDIAN guest loads. MVC2 runs the SH4 in LE mode (flycast stores guest RAM
 * in host LE order); the prod RAM dump is verbatim LE. Earlier the PoC used BE
 * accessors + a byteswapped image — a double-inversion that cancelled for word/half
 * reads but CORRUPTED sub-word BYTE reads (node+0x12c guard, node+0x5d, descriptor
 * bytes). LE everywhere + a verbatim dump copy is the correct, consistent model. */
static inline u32 r32(Sh4Ctx*c, u32 a){
    MC_RLOG(a,4);
    u32 i=mc_idx(a); u8*p=c->ram+i;
    return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);
}
static inline u32 r16s(Sh4Ctx*c, u32 a){
    MC_RLOG(a,2);
    u32 i=mc_idx(a); u8*p=c->ram+i;
    u16 v=(u16)p[0]|((u16)p[1]<<8);
    return (u32)(s32)(s16)v;       /* mov.w sign-extends */
}
static inline u32 r16u(Sh4Ctx*c, u32 a){
    MC_RLOG(a,2);
    u32 i=mc_idx(a); u8*p=c->ram+i; return (u16)p[0]|((u16)p[1]<<8);
}
static inline u32 r8s(Sh4Ctx*c, u32 a){
    MC_RLOG(a,1);
    u32 i=mc_idx(a); return (u32)(s32)(s8)c->ram[i];  /* mov.b sign-extends */
}
static inline u32 r8u(Sh4Ctx*c, u32 a){ MC_RLOG(a,1); return c->ram[mc_idx(a)]; }

#ifdef MC_WTRAP
extern void mc_wtrap(u32 a);
#define MC_WT(a) mc_wtrap(a)
#else
#define MC_WT(a) ((void)0)
#endif
static inline void w32(Sh4Ctx*c, u32 a, u32 v){
    MC_WT(a); MC_WLOG(a,4);
    u32 i=mc_idx(a); u8*p=c->ram+i;
    p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;
}
static inline void w16(Sh4Ctx*c, u32 a, u32 v){
    MC_WLOG(a,2);
    u32 i=mc_idx(a); u8*p=c->ram+i; p[0]=v; p[1]=v>>8;
}
static inline void w8(Sh4Ctx*c, u32 a, u32 v){ MC_WLOG(a,1); c->ram[mc_idx(a)]=(u8)v; }

#endif
