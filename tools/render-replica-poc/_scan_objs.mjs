// _scan_objs.mjs — scan OBJF messages in a 7200 capture for effect objects.
// Tries OBJF_REC_SIZE 16 and 18; reports per-frame any record with is_effect set,
// or any sprite_id change burst (super/projectile signature).
import { readFileSync } from 'node:fs';
const path = process.argv[2] || '_fxwin_long.zcst';
const file = readFileSync(path);
const dv = new DataView(file.buffer, file.byteOffset, file.byteLength);
let off=0; const objfMsgs=[], objsMsgs=[];
while(off+4<=file.length){const len=dv.getUint32(off,true);off+=4;if(off+len>file.length)break;
  if(len>0){const m=file.subarray(off,off+len);off+=len;
    if(m[0]===0x4F&&m[1]===0x42&&m[2]===0x4A&&m[3]===0x46) objfMsgs.push(m); // OBJF
    if(m[0]===0x4F&&m[1]===0x42&&m[2]===0x4A&&m[3]===0x53) objsMsgs.push(m); // OBJS
  } else {}
}
console.log(`OBJF msgs=${objfMsgs.length}  OBJS msgs=${objsMsgs.length}`);
// OBJF: magic(4)? or count(1)+recs? serialize writes count(1)+N*REC with NO magic per the cpp.
// But the wire framing put OBJF magic — inspect raw header of first OBJF.
if(objfMsgs.length){ const m=objfMsgs[0]; console.log('OBJF[0] len',m.length,'bytes[0..8]',[...m.slice(0,12)]); }
if(objsMsgs.length){ const m=objsMsgs[0]; console.log('OBJS[0] len',m.length,'bytes[0..16]',[...m.slice(0,16)]); }

// Heuristic parse: after a 4-byte magic, byte = count; then N records of REC bytes.
function scanEffects(msgs, label){
  let framesWithEfx=0, maxObj=0, anyEfxFrames=[];
  for(let fi=0; fi<msgs.length; fi++){
    const m=msgs[fi];
    // try magic(4)+count(1) then 16 or 18 stride
    for(const stride of [16,18,14,13,11]){
      const cnt=m[4];
      if(cnt>0 && cnt<=255 && 5+cnt*stride<=m.length+stride){
        // is_effect offset within record: per serialize it's at rec offset 11 (16B layout) — but try scanning
        let efx=0;
        for(let i=0;i<cnt;i++){
          const base=5+i*stride; if(base+stride>m.length)break;
          // search any byte==1 at the "is_effect" slot candidates
          // 16B layout: is_effect at +11
          const ie = (base+11<m.length)?m[base+11]:0;
          if(ie===1) efx++;
        }
        maxObj=Math.max(maxObj,cnt);
        if(efx>0){ framesWithEfx++; if(anyEfxFrames.length<20) anyEfxFrames.push(`f${fi}:cnt${cnt}efx${efx}(s${stride})`); }
        break; // only try the first plausible stride
      }
    }
  }
  console.log(`${label}: maxObjCount=${maxObj} framesWithEfx=${framesWithEfx}`);
  if(anyEfxFrames.length) console.log('  ', anyEfxFrames.join(' '));
}
scanEffects(objfMsgs,'OBJF');
scanEffects(objsMsgs,'OBJS');
