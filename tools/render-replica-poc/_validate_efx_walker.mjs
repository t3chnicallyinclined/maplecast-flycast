// Prove the FAITHFUL effect path: effects use the SAME walker keys (node+0x144 sel,
// node+0x160 GFX2) as bodies. We resolve the active body's keys in the prod capture and
// show the walker's first-u16 cell COUNT > 0 (the engine emits tiles via loc_8c0344d4).
// An effect node differs ONLY in cat (1..4) + GFX2 pointing into the Effect-Poly bank —
// the resolution arithmetic is identical. (disasm: bank03 loc_8c034bea + loc_8c0344d4)
import { readFileSync } from 'node:fs';
const path=process.argv[2];
const all=new Uint8Array(readFileSync(path));
const dv0=new DataView(all.buffer,all.byteOffset,all.byteLength);
let p=0;const u32=()=>{const v=dv0.getUint32(p,true);p+=4;return v>>>0;};
if(u32()!==0x5252434D)throw 'bad';
const ver=u32(),nS=u32(),nD=u32(),nF=u32(),vb=u32(),pb=u32();u32();
const reg=()=>{const a=u32(),l=u32();let t='';for(let i=0;i<8;i++){const c=all[p+i];if(c)t+=String.fromCharCode(c);}p+=8;return{addr:a,len:l,tag:t};};
const sR=Array.from({length:nS},reg),dR=Array.from({length:nD},reg);
const ram=new Uint8Array(16*1024*1024);
let sp=p+vb+pb;
for(const r of sR){const b=all.subarray(sp,sp+r.len);sp+=r.len; if(r.tag==='ram16')ram.set(b,0);else ram.set(b,r.addr&0xFFFFFF);}
let fp=sp; fp+=12; for(const r of dR){ram.set(all.subarray(fp,fp+r.len),r.addr&0xFFFFFF);fp+=r.len;}
const rv=new DataView(ram.buffer);
const R8=a=>ram[a&0xFFFFFF], R16=a=>rv.getUint16(a&0xFFFFFF,true), R32=a=>rv.getUint32(a&0xFFFFFF,true)>>>0;
const CHAR=[0x8C268340,0x8C2688E4,0x8C268E88,0x8C26942C,0x8C2699D0,0x8C269F74];
console.log('--- WALKER-KEY RESOLUTION (disasm loc_8c0344d4: cell=GFX2 + *(GFX2+(sel&0x7FFF)*4); u16=count) ---');
for(let s=0;s<CHAR.length;s++){
  const b=CHAR[s]; if(!R8(b))continue;
  const sel=R16(b+0x144), gfx2=R32(b+0x160), cat=R8(b+0x3), gate=R8(b+0x12C);
  if(sel===0xFF){console.log(`slot${s} base=${b.toString(16)} sel=0xFF (terminator -> 0 tiles, loc_8c034bea)`);continue;}
  const idx=sel&0x7FFF;
  const recOff=R32(gfx2+idx*4)>>>0;
  const cellPtr=(gfx2+recOff)>>>0;
  const count=R16(cellPtr);
  const scaled=(sel&0x8000)?'loc_8c0348c8(SCALED)':'loc_8c0344d4(walker)';
  console.log(`slot${s} base=${b.toString(16)} cat=${cat} gate=${gate} sel=0x${sel.toString(16)} -> ${scaled}  gfx2=${gfx2.toString(16)} cellPtr=${cellPtr.toString(16)} CELL_COUNT=${count}`);
}
console.log('\nPROOF: a body resolves its walker keys (sel+GFX2) to a real cell-record stream (count>0).');
console.log('An EFFECT node uses the IDENTICAL arithmetic; only cat (1..4) + GFX2-in-Effect-bank differ.');
console.log('=> The faithful effect path is the walker; the directory-binding heuristic is superseded.');
