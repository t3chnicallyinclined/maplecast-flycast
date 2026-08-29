/* fx_live.c — DETERMINISTIC OFFLINE EMISSION MEASUREMENT for the LIVE-match wire (_live2).
 * Loads a reconstructed 16MB RAM image + the frame's shipped BTCW words (fed via
 * render_frame_set_body_tcws exactly like the live client), runs the REAL render_frame,
 * and dumps every emitted scene quad with per-node attribution.
 *
 * Attribution method (cull-proof): pass 1 = render_frame(c) records the slot-walk node list
 * (g_obj_node/g_obj_ntiles, walk order). Pass 2 = render_frame_reset() + per-node direct
 * render_object_full/_satellite calls in the SAME order, reading g_nscene before/after each
 * -> exact per-node emitted quad ranges. Pass 2 scene is byte-compared against pass 1
 * (minus the 85xxx collapse, which we re-apply) to PROVE the replay is faithful.
 *
 * Usage: fx_live.exe <ram16.bin> [btcw_words.bin]
 * Build: cl /nologo /O2 /fp:precise /D_CRT_SECURE_NO_WARNINGS /Fe:fx_live.exe fx_live.c
 *        render_frame.c gen_render_object.c gen_render_satellite.c gen_transform_obj.c
 *        gen_submit_params.c gen_walker.c gen_walker_root.c gen_walker_scale.c gen_leaf.c
 */
#include "sh4ctx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* mirror of render_frame.c SceneQuad — MUST match (pcw..recidx, 8 corners + u1 + v1, sel,
 * gfx1, mirror, facing, z). */
typedef struct {
    u32 pcw, isp, tsp, tcw, recidx;
    float Ax,Ay,Bx,By,Cx,Cy,Dx,Dy, u1, v1;
    u32 sel; u32 gfx1; u32 mirror; u32 facing; float z;
} SceneQuad;

void render_frame(Sh4Ctx *c);
void render_frame_reset(void);
int  render_frame_nscene(void);
const SceneQuad* render_frame_scene(void);
u32  render_frame_quad_is_effect_impl(unsigned char* out_e, u32 cap);
void render_frame_set_body_tcws(const u32* buf, int nWords);
int  render_object_full(Sh4Ctx *c, u32 node);
int  render_object_full_satellite(Sh4Ctx *c, u32 node);
extern int g_body_count;
extern u32 g_obj_node[64];
extern int g_obj_ntiles[64];

static float rff(Sh4Ctx*c, u32 a){ u32 w=r32(c,a); float f; memcpy(&f,&w,4); return f; }

int main(int argc, char** argv){
    if(argc<2){ fprintf(stderr,"usage: %s <ram16.bin> [btcw_words.bin]\n",argv[0]); return 2; }
    FILE* f=fopen(argv[1],"rb"); if(!f){ perror("open ram"); return 2; }
    static u8 RAM[RAM_SIZE];
    size_t got=fread(RAM,1,RAM_SIZE,f); fclose(f);
    if(got!=RAM_SIZE){ fprintf(stderr,"short ram read %zu\n",got); return 2; }

    static u32 BT[65536]; int btWords=0;
    if(argc>=3){
        FILE* fb=fopen(argv[2],"rb"); if(!fb){ perror("open btcw"); return 2; }
        btWords=(int)(fread(BT,4,65536,fb)); fclose(fb);
        render_frame_set_body_tcws(BT, btWords);
    }

    static Sh4Ctx c; memset(&c,0,sizeof c); c.ram=RAM;

    /* ---- pass 1: the REAL render_frame (exactly what the live client runs) ---- */
    render_frame(&c);
    int nq1=render_frame_nscene();
    static SceneQuad S1[1024]; memcpy(S1, render_frame_scene(), sizeof(SceneQuad)*(nq1>1024?1024:nq1));
    static unsigned char E1[1024]; render_frame_quad_is_effect_impl(E1,1024);
    int nobj=g_body_count;
    static u32 nodes[64]; static int nt1[64];
    for(int i=0;i<nobj && i<64;i++){ nodes[i]=g_obj_node[i]; nt1[i]=g_obj_ntiles[i]; }

    printf("=== %s: btcwWords=%d nquads=%d nobjs=%d ===\n", argv[1], btWords, nq1, nobj);

    /* ---- pass 2: replay per node for exact attribution ---- */
    static int emitted[64]; static int qbase[64];
    render_frame_reset();
    for(int i=0;i<nobj && i<64;i++){
        u32 node=nodes[i];
        u32 cat = r8u(&c, node+0x3);
        int before=render_frame_nscene();
        int nt = (cat==0) ? render_object_full(&c,node) : render_object_full_satellite(&c,node);
        int after=render_frame_nscene();
        qbase[i]=before; emitted[i]=after-before;
        if(nt!=nt1[i]) printf("!! REPLAY DIVERGENCE node=0x%08X pass1 nt=%d pass2 nt=%d\n",node,nt1[i],nt);
    }
    int nq2=render_frame_nscene();
    /* re-apply the 85xxx collapse (render_frame() does it; per-node replay doesn't) */
    {   SceneQuad* S2=(SceneQuad*)render_frame_scene();
        for(int i=0;i<nq2;i++){
            u32 band=S2[i].tcw & 0x1FFFFFu;
            if(band>=0x85000u && band<0x86000u){
                S2[i].Bx=S2[i].Cx=S2[i].Dx=S2[i].Ax; S2[i].By=S2[i].Cy=S2[i].Dy=S2[i].Ay;
            }
        }
        int same = (nq1==nq2) && (memcmp(S1,S2,sizeof(SceneQuad)*nq1)==0);
        printf("replay-fidelity: nq1=%d nq2=%d bytes %s\n", nq1, nq2, same?"IDENTICAL":"DIFFER");
        if(!same && nq1==nq2){
            int shown=0;
            for(int i=0;i<nq1 && shown<8;i++){
                if(memcmp(&S1[i],&S2[i],sizeof(SceneQuad))!=0){
                    const u32* a=(const u32*)&S1[i]; const u32* b=(const u32*)&S2[i];
                    printf("  qdiff[%d]:", i);
                    for(int w=0;w<(int)(sizeof(SceneQuad)/4);w++)
                        if(a[w]!=b[w]) printf(" word%d 0x%08X!=0x%08X", w, a[w], b[w]);
                    printf("\n");
                    shown++;
                }
            }
        }
    }

    /* ---- per-node table ---- */
    for(int i=0;i<nobj && i<64;i++){
        u32 node=nodes[i];
        u16 sid=r16u(&c,node+0x144);
        u32 cat=r8u(&c,node+0x3);
        printf("NODE %d 0x%08X cat=%u sid=0x%04X bit15=%d walker_tiles=%d emitted=%d culled=%d anchor=(%.1f,%.1f) sxs=%.3f sys=%.3f gfx1=0x%08X f104=0x%X facing=%u\n",
            i, node, cat, sid, (sid&0x8000)?1:0, nt1[i], emitted[i], nt1[i]-emitted[i],
            rff(&c,node+0xE0), rff(&c,node+0xE4), rff(&c,node+0xEC), rff(&c,node+0xF0),
            r32(&c,node+0x15C), r32(&c,node+0x104), r32(&c,node+0x110)?1u:0u);
    }

    /* ---- per-quad table (pass-2 order == pass-1 order, proven above) ---- */
    for(int i=0;i<nobj && i<64;i++){
        for(int k=0;k<emitted[i];k++){
            const SceneQuad* q=&S1[qbase[i]+k];
            float xs[4]={q->Ax,q->Bx,q->Cx,q->Dx}, ys[4]={q->Ay,q->By,q->Cy,q->Dy};
            float mnx=xs[0],mxx=xs[0],mny=ys[0],mxy=ys[0];
            for(int j=1;j<4;j++){ if(xs[j]<mnx)mnx=xs[j]; if(xs[j]>mxx)mxx=xs[j];
                                  if(ys[j]<mny)mny=ys[j]; if(ys[j]>mxy)mxy=ys[j]; }
            u32 addr=((q->tcw&0x1FFFFFu)<<3);
            u32 pal=(q->tcw>>21)&0x3F;
            printf("Q n%d k%02d eff=%d sel=0x%04X tcw=0x%08X addr=0x%06X pal=%u xy=(%.1f,%.1f) wh=(%.1fx%.1f) z=%.5f mir=%u\n",
                i, k, E1[qbase[i]+k]?1:0, q->sel, q->tcw, addr, pal,
                mnx, mny, mxx-mnx, mxy-mny, q->z, q->mirror);
        }
    }
    return 0;
}
