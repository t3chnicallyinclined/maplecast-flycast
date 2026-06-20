// _find_efx_nodes.mjs — load an MCRR RAM capture, walk the slot table EXACTLY like
// gen_walker_root.c across all frames, enumerate every node, classify by category +3,
// and report nodes whose GFX2(node+0x160) OR GFX1(node+0x15C) lands in the Effect-Poly
// bank [0x0CED0000,0x0CEE0000). These are the super/projectile EFFECT nodes.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
const CAP = process.argv[2] || '../../_satwalk2.mcrr';
const STEP = parseInt(process.argv[3] || '4', 10);
const buf = new Uint8Array(readFileSync(fileURLToPath(new URL(CAP, import.meta.url))));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
if (u32() !== 0x5252434D) throw new Error('bad MCRR');
const ver=u32(),nS=u32(),nD=u32(),nF=u32(),vB=u32(),pB=u32();u32();
const reg=()=>{const a=u32(),l=u32();let t='';for(let i=0;i<8;i++){const c=buf[p+i];if(c)t+=String.fromCharCode(c);}p+=8;return{addr:a>>>0,len:l,tag:t};};
const sR=Array.from({length:nS},reg),dR=Array.from({length:nD},reg);
const headerEnd=p; const G=a=>(a>>>0)&0xFFFFFF;
const baseRam=new Uint8Array(16*1024*1024);
{let sp=headerEnd+vB+pB;for(const r of sR){const b=buf.subarray(sp,sp+r.len);sp+=r.len;if(r.tag==='ram16')baseRam.set(b,0);else baseRam.set(b,G(r.addr));}}
const dynTotal=dR.reduce((s,r)=>s+r.len,0);
const frames=[];
{let fp=headerEnd+vB+pB;for(const r of sR)fp+=r.len;for(let f=0;f<nF;f++){const recStart=fp;const vframe=dv.getUint32(fp+4,true);fp+=12+dynTotal;frames.push({recStart,vframe});}}
const u8=(ram,a)=>ram[G(a)];
const s8=(ram,a)=>{const v=ram[G(a)];return v>127?v-256:v;};
const u32r=(ram,a)=>(ram[G(a)]|(ram[G(a)+1]<<8)|(ram[G(a)+2]<<16)|(ram[G(a)+3]<<24))>>>0;
const u16r=(ram,a)=>(ram[G(a)]|(ram[G(a)+1]<<8))>>>0;
const COUNT_BASE=0x8C2895E0,PTR_BASE=0x8C287DE0,LAYER_STRIDE=0x180;
const inEfx=g=>g>=0x0CED0000&&g<0x0CEE0000;
const isArea3=g=>(((g>>>24)&0x7F)===0x0C)&&g!==0;
const efxHits=new Map(); // key node -> sample
let framesWithEfx=0, totalNodes=0, catHist={};
for(let fi=0;fi<nF;fi+=STEP){
  const fr=frames[fi]; const ram=baseRam.slice();
  {let dp=fr.recStart+12;for(const r of dR){ram.set(buf.subarray(dp,dp+r.len),G(r.addr));dp+=r.len;}}
  let frameHasEfx=false;
  let r13=COUNT_BASE, r12=PTR_BASE; const r8=COUNT_BASE+0x10;
  while(r13<r8){
    const count=s8(ram,r13);
    for(let r14=0;r14<count && count<=0x60;r14++){
      const node=u32r(ram,r12+(r14<<2));
      if(!isArea3(node))continue;
      totalNodes++;
      const cat=s8(ram,node+0x3); catHist[cat]=(catHist[cat]||0)+1;
      const gfx1=u32r(ram,node+0x15C);
      const gfx2=u32r(ram,node+0x160);
      const sel=u16r(ram,node+0x144);
      if(inEfx(gfx1)||inEfx(gfx2)){
        frameHasEfx=true;
        if(!efxHits.has(node)) efxHits.set(node,{fi,vframe:fr.vframe,cat,gfx1,gfx2,sel,gate:u16r(ram,node+0x12C),
          ex0:u32r(ram,node+0xE0),ey0:u32r(ram,node+0xE4)});
      }
    }
    r13+=1; r12+=LAYER_STRIDE;
  }
  if(frameHasEfx)framesWithEfx++;
}
console.log(`CAP=${CAP} frames=${nF} step=${STEP} totalNodesWalked=${totalNodes} framesWithEfx=${framesWithEfx}`);
console.log('category histogram (all walked nodes):',JSON.stringify(catHist));
console.log(`distinct effect-poly nodes: ${efxHits.size}`);
let i=0;
for(const [node,s] of efxHits){ if(i++>=30)break;
  const exf=(x)=>{const f=new Float32Array(new Uint32Array([x]).buffer)[0];return f.toFixed(1);};
  console.log(`  node=${node.toString(16)} fi=${s.fi} vf=${s.vframe} cat=${s.cat} gfx1=${s.gfx1.toString(16)} gfx2=${s.gfx2.toString(16)} sel=${s.sel.toString(16)} gate=${s.gate} pos=(${exf(s.ex0)},${exf(s.ey0)})`);
}
