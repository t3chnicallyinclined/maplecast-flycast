/* sh4ctx_trace.h — READ-SET-INSTRUMENTED drop-in for sh4ctx.h.
 *
 * Identical memory model to sh4ctx.h (LITTLE-ENDIAN, flat 16MB area-3 RAM,
 * translate(a)=a&0x00FFFFFF), but every guest-RAM READ is recorded into a global
 * trace buffer so we can derive the EXACT read-set of one render_frame() call.
 *
 * Build the render tree against THIS header (instead of sh4ctx.h) by -include'ing
 * it / -DSH4CTX_H so the real sh4ctx.h is skipped. The trace records (addr,size)
 * per read; the harness coalesces into [start,end) regions and classifies them.
 *
 * SCOPE NOTE — the walker/setup use an SH4 STACK at r15 = 0x0C480000 (scratch the
 * PoC owns, NOT engine state). Those stack reads/writes hit ram[0x480000..]. We DO
 * record them (so the trace is honest), and the harness tags the [0x0C480000-128 ..
 * 0x0C480100) band as PoC-SCRATCH (neither static nor dynamic engine state — it is
 * re-initialized by the caller every frame). Everything ELSE that is read is real
 * engine RAM and must be partitioned static|dynamic.
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
    float xf[16];
    u32 fpscr, fpul, pr, macl, mach, sr_t, gbr;
    u32 _pool;
    u8 *ram;
} Sh4Ctx;

/* ---- read trace ---- */
#define MC_TRACE_MAX (1u<<22)   /* 4M events plenty for one frame */
typedef struct { u32 addr; u32 size; } McRead;
extern McRead  mc_trace[MC_TRACE_MAX];
extern u32     mc_trace_n;
extern int     mc_trace_on;     /* gate: only record while a render_frame is active */

static inline void mc_rec(u32 a, u32 sz){
    if(mc_trace_on && mc_trace_n < MC_TRACE_MAX){
        mc_trace[mc_trace_n].addr = a & 0x00FFFFFFu;   /* normalize to ram[] index space */
        mc_trace[mc_trace_n].size = sz;
        mc_trace_n++;
    }
}

static inline u32 mc_idx(u32 a){ return a & 0x00FFFFFFu; }

static inline u32 r32(Sh4Ctx*c, u32 a){
    mc_rec(a,4); u32 i=mc_idx(a); u8*p=c->ram+i;
    return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);
}
static inline u32 r16s(Sh4Ctx*c, u32 a){
    mc_rec(a,2); u32 i=mc_idx(a); u8*p=c->ram+i;
    u16 v=(u16)p[0]|((u16)p[1]<<8); return (u32)(s32)(s16)v;
}
static inline u32 r16u(Sh4Ctx*c, u32 a){
    mc_rec(a,2); u32 i=mc_idx(a); u8*p=c->ram+i; return (u16)p[0]|((u16)p[1]<<8);
}
static inline u32 r8s(Sh4Ctx*c, u32 a){
    mc_rec(a,1); u32 i=mc_idx(a); return (u32)(s32)(s8)c->ram[i];
}
static inline u32 r8u(Sh4Ctx*c, u32 a){ mc_rec(a,1); return c->ram[mc_idx(a)]; }

/* writes are NOT recorded (read-set only); they go to ram[] (PoC scratch + node deposits) */
static inline void w32(Sh4Ctx*c, u32 a, u32 v){
    u32 i=mc_idx(a); u8*p=c->ram+i; p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;
}
static inline void w16(Sh4Ctx*c, u32 a, u32 v){
    u32 i=mc_idx(a); u8*p=c->ram+i; p[0]=v; p[1]=v>>8;
}
static inline void w8(Sh4Ctx*c, u32 a, u32 v){ c->ram[mc_idx(a)]=(u8)v; }

#endif
