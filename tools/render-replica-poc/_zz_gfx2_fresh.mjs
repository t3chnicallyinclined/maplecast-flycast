// _zz_gfx2_fresh.mjs — re_kb/32 diagnostic: is the shipped GFX2 dispatch fresh?
// For Storm's body node each frame: compute the client cell pointer
//   r11_client = GFX2base + read_u32(GFX2base + (sid&0x7FFF)*4)
// from the .mcrr RAM, and compare to the ENGINE r11 (ASMTRACE col 16).
// Divergence => the server shipped a STALE (frozen) GFX2 self-modify entry = the scramble.
//   node _zz_gfx2_fresh.mjs <file.mcrr> <asm.log> <nodeHex> <cid>
import { readFileSync } from 'node:fs';
const path=process.argv[2], asmPath=process.argv[3], nodeHex=process.argv[4], wantCid=+process.argv[5];
const nodeGuest = (parseInt(nodeHex,16) & 0xFFFFFF);

// ---- parse .mcrr, build per-vframe RAM seek ----
const buf=new Uint8Array(readFileSync(path));
const dv=new DataView(buf.buffer,buf.byteOffset,buf.byteLength);
let p=0; const u32=()=>{const v=dv.getUint32(p,true);p+=4;return v>>>0;};
u32();u32(); const nS=u32(),nD=u32(),nF=u32(),vb=u32(),pb=u32();u32();
const region=()=>{const a=u32(),l=u32();let t='';for(let i=0;i<8;i++){const c=buf[p+i];if(c)t+=String.fromCharCode(c);}p+=8;return{addr:a>>>0,len:l,tag:t};};
const sR=Array.from({length:nS},region), dR=Array.from({length:nD},region);
const ram=new Uint8Array(16*1024*1024);
let q=p+vb+pb;
for(const r of sR){const b=buf.subarray(q,q+r.len);q+=r.len;if(r.tag==='ram16')ram.set(b,0);else ram.set(b,r.addr&0xFFFFFF);}
p=q; const frames=new Map();
for(let f=0;f<nF;f++){u32();const vf=u32();const tail=u32();const dof=p;for(const r of dR)p+=r.len;p+=tail;frames.set(vf,dof);}

const r32=a=>(ram[a&0xFFFFFF]|(ram[(a+1)&0xFFFFFF]<<8)|(ram[(a+2)&0xFFFFFF]<<16)|(ram[(a+3)&0xFFFFFF]<<24))>>>0;
const r16=a=>ram[a&0xFFFFFF]|(ram[(a+1)&0xFFFFFF]<<8);
function seedFrame(vf){ const dof=frames.get(vf); if(dof===undefined) return false; let o=dof; for(const r of dR){ram.set(buf.subarray(o,o+r.len),r.addr&0xFFFFFF);o+=r.len;} return true; }

// engine r11 per (vf) for our node+cid from ASMTRACE
const engR11=new Map();  // vf -> engine r11 (first tile of the body)
for(const line of readFileSync(asmPath,'utf8').split('\n')){
  if(!line||line[0]==='#')continue; const t=line.split(/\s+/);
  if(+t[3]!==wantCid) continue; const node=t[17]; if((parseInt(node,16)&0xFFFFFF)!==nodeGuest) continue;
  const vf=+t[0]; if(!engR11.has(vf)) engR11.set(vf, parseInt(t[15],16));  // col16 (1-idx) = r11
}

let n=0, stale=0, fresh=0; const staleVfs=[];
for(const [vf, r11eng] of engR11){
  if(!seedFrame(vf)) continue;
  const gfx2 = r32(nodeGuest + 0x160);
  const sid  = r16(nodeGuest + 0x144) & 0x7FFF;
  if(!gfx2) continue;
  const disp = r32((gfx2 & 0xFFFFFF) + sid*4);         // GFX2[sid*4]
  const r11cli = (gfx2 + disp + 2) >>> 0;  /* +2 = nrec u16 header (engine r11 = cell+2) */
  n++;
  const eq = (r11cli & 0xFFFFFF) === (r11eng & 0xFFFFFF);
  if(eq) fresh++; else { stale++; if(staleVfs.length<12) staleVfs.push({vf,sid,r11cli:(r11cli>>>0).toString(16),r11eng:(r11eng>>>0).toString(16)}); }
}
console.log(`node=${nodeHex} cid=${wantCid}: ${n} frames checked, FRESH(client==engine)=${fresh}, STALE(scramble)=${stale}`);
if(stale) { console.log('STALE frames (client GFX2 dispatch != engine) — the re_kb/32 scramble:');
  for(const s of staleVfs) console.log(`  vf=${s.vf} sid=0x${s.sid.toString(16)} r11_client=0x${s.r11cli} r11_engine=0x${s.r11eng}`); }
else console.log('GFX2 is FRESH every frame — the server ships the self-modify correctly (scramble is elsewhere).');
