// _zz_rf_engine_diff.mjs — RENDER_FRAME vs ENGINE (ASMTRACE = legacy ground truth) per-part diff.
// The render-side companion to _zz_catalog_carve_gate: for each body part in a capture, compare
// what render_frame EMITS (position + texU mirror) against the engine's authoritative per-part
// record (ASMTRACE: screenX/Y, flip col, flags col), and REPORT every render_frame divergence
// grouped by the engine flag bits — so an unhandled transform bit (0x8000, 0x10, 0x20, ...) shows
// up as "engine has flag X, render_frame emitted no matching transform." Drives to 0 when
// render_frame accounts for every flag. Samples frames (default every 15th) to stay fast.
//   node _zz_rf_engine_diff.mjs <file.mcrr> <asm.log> [stepFrames=15] [posTol=6]
import { readFileSync } from 'node:fs';
import createRenderFrame from './render_frame_node.mjs';

const path=process.argv[2], asmPath=process.argv[3], STEP=+(process.argv[4]||15), TOL=+(process.argv[5]||6);

// ---- parse .mcrr ----
const buf=new Uint8Array(readFileSync(path));
const dv=new DataView(buf.buffer,buf.byteOffset,buf.byteLength);
let p=0; const u32=()=>{const v=dv.getUint32(p,true);p+=4;return v>>>0;};
u32();u32(); const nS=u32(),nD=u32(),nF=u32(),vb=u32(),pb=u32();u32();
const region=()=>{const a=u32(),l=u32();let t='';for(let i=0;i<8;i++){const c=buf[p+i];if(c)t+=String.fromCharCode(c);}p+=8;return{addr:a>>>0,len:l,tag:t};};
const sR=Array.from({length:nS},region), dR=Array.from({length:nD},region);
const ram=new Uint8Array(16*1024*1024);
let q=p+vb+pb;
for(const r of sR){const b=buf.subarray(q,q+r.len);q+=r.len;if(r.tag==='ram16')ram.set(b,0);else ram.set(b,r.addr&0xFFFFFF);}
p=q; const frames=[];
for(let f=0;f<nF;f++){u32();const vf=u32();const tail=u32();const dof=p;for(const r of dR)p+=r.len;p+=tail;frames.push({vf,dof});}
const vfMin=Math.min(...frames.map(f=>f.vf)), vfMax=Math.max(...frames.map(f=>f.vf));

// ---- ASMTRACE index: vf -> node -> [{sel,sx,sy,flip,flags}] in fire order ----
const asmByVf=new Map();
for(const line of readFileSync(asmPath,'utf8').split('\n')){
  if(!line||line[0]==='#')continue; const t=line.split(/\s+/); const vf=+t[0];
  if(vf<vfMin||vf>vfMax)continue;
  let m=asmByVf.get(vf); if(!m){m=new Map();asmByVf.set(vf,m);}
  const node=t[17]; let a=m.get(node); if(!a){a=[];m.set(node,a);}
  a.push({sel:+t[4], sx:+t[9], sy:+t[10], flip:+t[13], flags:parseInt(t[14],16)||0});
}

const M=await createRenderFrame({locateFile:x=>x});
const ramPtr=M._malloc(ram.length), outPtr=M._malloc(256*1024);
const mCap=4096, mPtr=M._malloc(mCap);

// engine flag bits render_frame accounts for (2026-07-10): 0x8000 = texU H-mirror (facing-composed)
// + 0x4000 = texV V-mirror. NOTE: the per-quad `mirror` gate below compares against ap.flip =
// the ASMTRACE `flip` col = (flags & 0x10), which is the HITBOX bit, NOT the render mirror — that
// comparison is INVALID for the texU/texV axes (don't trust `texU-mirror mismatch`). The FLAG-GAP
// report (unaccounted render bits) is the valid signal; with HANDLED=0xC000 it should show no
// 0x8000/0x4000 rows (only the non-render 0x10/0x20/0x40/0x80 hitbox bits).
const HANDLED=0xC000;
let framesChecked=0, partsSeen=0, posBad=0, mirrorBad=0;
const flagGap=new Map();   // engine flag bit present + render_frame emitted no transform -> count
const flagGapSel=new Map();

for(let fi=0; fi<nF; fi+=STEP){
  const fr=frames[fi]; const asm=asmByVf.get(fr.vf); if(!asm) continue;
  let o=fr.dof; for(const r of dR){ram.set(buf.subarray(o,o+r.len),r.addr&0xFFFFFF);o+=r.len;}
  M.HEAPU8.set(ram,ramPtr);
  M._render_frame_ta(ramPtr,outPtr,256*1024);
  const quads=M._render_frame_quad_count();
  const ta=M.HEAPU8.subarray(outPtr,outPtr+quads*96);
  const tdv=new DataView(ta.buffer,ta.byteOffset,ta.byteLength);
  M._render_frame_quad_mirror(mPtr, quads);
  const mir=M.HEAPU8.subarray(mPtr, mPtr+quads);
  const objN=M._render_frame_obj_count();
  let qi=0;
  for(let b=0;b<objN;b++){
    const node=((M._render_frame_obj_node(b)>>>0)&0x0FFFFFFF).toString(16).padStart(8,'0');
    const nt=M._render_frame_obj_ntiles(b);
    const a=asm.get(node);
    for(let k=0;k<nt;k++){
      const off=(qi+k)*96;
      const ax=tdv.getFloat32(off+36,true), bx=tdv.getFloat32(off+48,true), cy=tdv.getFloat32(off+64,true);
      const rfMir=mir[qi+k]&1;
      const ap=a&&a[k]; if(!ap){ continue; }
      partsSeen++;
      const dPos=Math.min(Math.abs(ax-ap.sx),Math.abs(bx-ap.sx));
      if(dPos>TOL && Math.abs(cy-ap.sy)>TOL) posBad++;
      // mirror: engine 'flip' col (applied X flip) vs render_frame mirror
      if((ap.flip&1)!==rfMir) mirrorBad++;
      // FLAG GAP: any engine flags bit render_frame does NOT account for
      const unh = ap.flags & ~HANDLED & 0xFFFF;
      if(unh){ for(let bit=1;bit<0x10000;bit<<=1) if(unh&bit){
        flagGap.set(bit,(flagGap.get(bit)||0)+1);
        const key=bit+':'+ap.sel; flagGapSel.set(key,(flagGapSel.get(key)||0)+1);
      }}
    }
    qi+=nt;
  }
  framesChecked++;
}

console.log(`\n=== render_frame vs ENGINE (ASMTRACE) — ${framesChecked} frames sampled (vf ${vfMin}..${vfMax}, every ${STEP}), ${partsSeen} parts ===`);
console.log(`position mismatch(>${TOL}px): ${posBad}   texU-mirror mismatch: ${mirrorBad}`);
console.log(`\nUNACCOUNTED engine flag bits render_frame emitted NO transform for (per part-instance):`);
console.log(' bit      part-instances');
for(const [bit,cnt] of [...flagGap].sort((a,b)=>b[1]-a[1]))
  console.log(`  0x${bit.toString(16).padStart(4,'0')}  ${String(cnt).padStart(8)}`);
console.log(`\n(0 in every row = render_frame accounts for every engine transform on the sampled parts.)`);
