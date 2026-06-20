// _scan_fx.mjs — decode a .zcst mirror capture and for EVERY TA frame report:
//   - total sprite count
//   - sprites grouped by (ListType, SrcInstr, DstInstr) blend signature
//   - additive sprites (DstInstr==ONE i.e. ==1) = EFFECT candidates, with screen bbox
// Finds which frames carry super/projectile effect-poly quads.
import { readFileSync } from 'node:fs';
import './webgpu-headless.mjs';
const W_DIR = new URL('../../web/webgpu/', import.meta.url);
const { FrameDecoder } = await import(new URL('frame-decoder.mjs', W_DIR));

const path = process.argv[2] || '_fxwin1.zcst';
const file = readFileSync(path);
const dv = new DataView(file.buffer, file.byteOffset, file.byteLength);
const D = new FrameDecoder();
let off = 0; const msgs = [];
while (off + 4 <= file.length) {
  const len = dv.getUint32(off, true); off += 4;
  if (off + len > file.length) break;
  if (len > 0) { const m = file.subarray(off, off + len); off += len;
    // only feed ZCST (the engine TA mirror) to the FrameDecoder
    if (m.length>=4 && m[0]===0x5A && m[1]===0x43 && m[2]===0x53 && m[3]===0x54) msgs.push(m);
  } else { /* zero-length keepalive */ }
}

const SRC = ['ZERO','ONE','OTHER','INVOTHER','SRCA','INVSRCA','DSTA','INVDSTA'];
function parseTA(buf){
  const out=[]; let o=0; const n=buf.length;
  while(o+32<=n){
    const pcw=buf.readUInt32LE(o); const pt=(pcw>>29)&7;
    if(pt===5){ if(o+96>n)break;
      const tsp=buf.readUInt32LE(o+8); const tcw=buf.readUInt32LE(o+12);
      const v=o+32;
      const Ax=buf.readFloatLE(v+4),Ay=buf.readFloatLE(v+8);
      const Cx=buf.readFloatLE(v+28),Cy=buf.readFloatLE(v+32);
      const lt=(pcw>>24)&7;
      const srcI=(tsp>>29)&7, dstI=(tsp>>26)&7;
      out.push({lt,srcI,dstI,tcw:tcw&0x1FFFFF,Ax,Ay,Cx,Cy});
      o+=96; continue;
    }
    if(pt===0) break; // end of list-ish; but TA may have multiple lists. just step
    o+=32;
  }
  return out;
}

let fi=0;
for(const m of msgs){
  let fr=null; try{ fr=D.applyFrame(m);}catch(e){continue;}
  if(!fr) continue;
  fi++;
  const buf=Buffer.from(fr.taBuffer.buffer||fr.taBuffer, fr.taBuffer.byteOffset||0, fr.taBuffer.byteLength||fr.taBuffer.length);
  const sp=parseTA(buf);
  const add = sp.filter(s=>s.dstI===1); // DstInstr=ONE => additive accumulate => EFFECT
  if(add.length){
    // bbox of additive sprites
    let x0=1e9,y0=1e9,x1=-1e9,y1=-1e9;
    for(const s of add){ x0=Math.min(x0,s.Ax,s.Cx);x1=Math.max(x1,s.Ax,s.Cx);y0=Math.min(y0,s.Ay,s.Cy);y1=Math.max(y1,s.Ay,s.Cy);}
    const sig=[...new Set(add.map(s=>`lt${s.lt}/${SRC[s.srcI]}->${SRC[s.dstI]}`))].join(',');
    console.log(`frame#${fi} fnum=${fr.frameNum??'?'} sprites=${sp.length} ADDITIVE=${add.length} bbox=[${x0|0},${y0|0}..${x1|0},${y1|0}] sig=${sig}`);
  }
}
console.log(`-- ${fi} renderable frames scanned --`);
