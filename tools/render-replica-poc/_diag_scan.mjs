// _diag_scan.mjs — per-body diagnostic over a .mcrr: sid, effect/scale-walker
// routing, tile count, and per-cell X(0x4000)/Y(0x8000) mirror flags.
//   node _diag_scan.mjs <file.mcrr> [frameStep=20]
import { readFileSync } from 'node:fs';
const path = process.argv[2] || 'idle.mcrr';
const step = +(process.argv[3] || 20);
const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p=0; const u32=()=>{const v=dv.getUint32(p,true);p+=4;return v>>>0;};
u32(); const ver=u32(),nS=u32(),nD=u32(),nF=u32(),vb=u32(),pb=u32(); u32();
const reg=()=>{const a=u32(),l=u32();let t='';for(let i=0;i<8;i++){const c=buf[p+i];if(c)t+=String.fromCharCode(c);}p+=8;return{addr:a,len:l,tag:t};};
const sR=Array.from({length:nS},reg), dR=Array.from({length:nD},reg);
const ram=new Uint8Array(16*1024*1024);
let q=p+vb+pb; for(const r of sR){const b=buf.subarray(q,q+r.len);q+=r.len; if(r.tag==='ram16')ram.set(b,0);else ram.set(b,r.addr&0xFFFFFF);}
p=q; const frames=[]; for(let f=0;f<nF;f++){const fm=u32(),vf=u32(),ts=u32();const dof=p;for(const r of dR)p+=r.len;p+=ts;frames.push({vf,dof});}
const G=a=>a&0xFFFFFF, r8s=a=>(ram[G(a)]<<24)>>24, r16u=a=>ram[G(a)]|(ram[G(a)+1]<<8), r32=a=>(ram[G(a)]|(ram[G(a)+1]<<8)|(ram[G(a)+2]<<16)|(ram[G(a)+3]<<24))>>>0;
const isRam=g=>(((g>>>24)&0x7F)===0x0C)&&g!==0;
const COUNT=0x8C2895E0, PTR=0x8C287DE0, STR=0x180;
console.log(`${path}: ${nF} frames, ${nS} static / ${nD} dyn\n`);
let TX=0,TY=0,Tboth=0,Tplain=0,Teff=0,Tbody=0; const sidHist=new Map();
for(let fi=0;fi<nF;fi+=step){ let off=frames[fi].dof; for(const r of dR){ram.set(buf.subarray(off,off+r.len),r.addr&0xFFFFFF);off+=r.len;}
  const rows=[];
  for(let L=0;L<16;L++){const cnt=r8s(COUNT+L);for(let i=0;i<cnt;i++){const node=r32(PTR+L*STR+i*4);if(!isRam(node))continue;
    const cat=r8s(node+3); const sid=r16u(node+0x144); const gfx2=r32(node+0x160); if(!isRam(gfx2))continue;
    const isEff=(sid&0x8000)!==0;                       // sel bit15 -> SCALE walker (effects)
    const cellOff=r32(gfx2+(sid&0x7FFF)*4); const cb=gfx2+cellOff; const nc=r16u(cb); if(nc===0||nc>128)continue;
    let x=0,y=0,b=0,pl=0;
    for(let c=0;c<nc;c++){const fl=r16u(cb+2+c*8+4);const X=(fl&0x4000)!==0,Y=(fl&0x8000)!==0;if(X&&Y)b++;else if(Y)y++;else if(X)x++;else pl++;}
    if(cat===0){Tbody++;}else{Teff++;}
    TX+=x;TY+=y;Tboth+=b;Tplain+=pl;
    sidHist.set(sid, (sidHist.get(sid)||0)+1);
    if((y||b) || (fi===0)) rows.push(`  node=${node.toString(16)} cat=${cat} sid=0x${sid.toString(16)}${isEff?' [EFFECT/scale-walker]':''} cells=${nc}  X=${x} Y=${y} XY=${b} plain=${pl}`);
  }}
  if(rows.length) { console.log(`frame ${fi} vframe=${frames[fi].vf}:`); rows.forEach(r=>console.log(r)); }
}
console.log(`\n=== TOTALS (sampled every ${step} frames) ===`);
console.log(`bodies=${Tbody} effects(scale-walker)=${Teff}`);
console.log(`cell flags: plain=${Tplain}  X(0x4000)=${TX} [render_frame HANDLES]  Y(0x8000)=${TY} [IGNORED]  XY=${Tboth} [Y dropped]`);
const top=[...sidHist.entries()].sort((a,b)=>b[1]-a[1]).slice(0,10).map(([s,n])=>`0x${s.toString(16)}(${n})`).join(' ');
console.log(`top sids: ${top}`);
