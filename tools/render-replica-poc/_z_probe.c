/* _z_probe.c — run the REAL render_frame on a captured 16MB RAM image and dump
 * per-part z per body. Verifies re_kb/38: is intra-body z constant (bug) or
 * distinctly decreasing per part (fixed)?  Build: see _build_zprobe.sh */
#include "sh4ctx.h"
#include <stdio.h>
#include <stdlib.h>

/* SceneQuad layout MUST match render_frame.c / wasm_entry_frame.c exactly. */
typedef struct {
    u32 pcw, isp, tsp, tcw, recidx;
    float Ax,Ay,Bx,By,Cx,Cy,Dx,Dy, u1, v1;
    u32 sel, gfx1, mirror, facing;
    float z;
} SceneQuad;

extern uint32_t render_frame_ta(uint8_t*, uint8_t*, uint32_t);
extern int render_frame_nscene(void);
extern const SceneQuad* render_frame_scene(void);
extern int g_body_count;
extern int g_obj_ntiles[64];
extern u32 g_obj_node[64];

int main(int argc, char** argv){
    const char* path = argc>1 ? argv[1] : "ram.bin";
    FILE* f = fopen(path,"rb"); if(!f){ perror(path); return 1; }
    uint8_t* ram = malloc(16*1024*1024);
    size_t n = fread(ram,1,16*1024*1024,f); fclose(f);
    if(n < 16*1024*1024){ fprintf(stderr,"short RAM read %zu\n",n); return 1; }
    uint8_t* out = malloc(4*1024*1024);
    uint32_t talen = render_frame_ta(ram, out, 4*1024*1024);
    int ns = render_frame_nscene();
    const SceneQuad* S = render_frame_scene();
    printf("bodies=%d  total_tiles=%d  ta=%u bytes\n\n", g_body_count, ns, talen);

    int ti=0, anyMulti=0;
    for(int b=0; b<g_body_count; b++){
        int nt = g_obj_ntiles[b];
        if(nt<=0){ continue; }
        float zmin=1e30f, zmax=-1e30f; int distinct=1;
        float z0 = S[ti].z;
        for(int k=0;k<nt && ti+k<ns;k++){
            float z=S[ti+k].z;
            if(z<zmin)zmin=z; if(z>zmax)zmax=z;
        }
        /* count distinct z values */
        int nd=0;
        for(int k=0;k<nt && ti+k<ns;k++){
            int seen=0; for(int j=0;j<k;j++) if(S[ti+j].z==S[ti+k].z){seen=1;break;}
            if(!seen) nd++;
        }
        printf("body%d node=%08X ntiles=%d  distinct_z=%d  zmin=%.9g zmax=%.9g  %s\n",
               b, g_obj_node[b], nt, nd, zmin, zmax,
               nt>1 ? (nd==1 ? "<<< ALL PARTS SAME Z (intra-body order = paint/submission)"
                             : (nd==nt ? "distinct per part" : "partly distinct")) : "(single tile)");
        if(nt>1){ anyMulti=1;
            for(int k=0;k<nt && ti+k<ns;k++) printf("    part%2d z=%.9g\n", k, S[ti+k].z);
        }
        ti += nt;
    }
    if(!anyMulti) printf("\n(no multi-part body in this frame — try another frameIndex)\n");
    return 0;
}
