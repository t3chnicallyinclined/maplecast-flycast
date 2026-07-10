// _tx_rec_dump.mjs — hex-dump the GFX2 cell records around a given RAM address for one
// captured frame (seed statics + that frame's dyn regions, then read RAM bytes).
//   node _tx_rec_dump.mjs <file.mcrr> <vframe> <ramAddrHex> [nRecords=24] [recBytes=8]
import { readFileSync } from 'node:fs';
const path = process.argv[2], wantVf = +process.argv[3], base = parseInt(process.argv[4],16);
const nRec = +(process.argv[5]||24), rb = +(process.argv[6]||8);

const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
u32(); u32(); const nS=u32(), nD=u32(), nF=u32(), vb=u32(), pb=u32(); u32();
const region = () => { const a=u32(), l=u32(); let t=''; for(let i=0;i<8;i++){const c=buf[p+i];if(c)t+=String.fromCharCode(c);} p+=8; return {addr:a>>>0, len:l, tag:t}; };
const sR = Array.from({length:nS}, region), dR = Array.from({length:nD}, region);
const ram = new Uint8Array(16*1024*1024);
let q = p + vb + pb;
for (const r of sR) { const b = buf.subarray(q, q+r.len); q+=r.len; if(r.tag==='ram16') ram.set(b,0); else ram.set(b, r.addr&0xFFFFFF); }
p = q;
for (let f=0; f<nF; f++){ u32(); const vf=u32(); const tail=u32(); const dof=p; for(const r of dR) p+=r.len; p+=tail;
  if (vf===wantVf){ let o=dof; for (const r of dR){ ram.set(buf.subarray(o,o+r.len), r.addr&0xFFFFFF); o+=r.len; } break; } }

const off = base & 0xFFFFFF;
for (let i=0;i<nRec;i++){
  const a = off + i*rb;
  const bytes = Array.from(ram.subarray(a, a+rb)).map(b=>b.toString(16).padStart(2,'0')).join(' ');
  const u16 = [];
  for (let k=0;k+1<rb;k+=2) u16.push((ram[a+k]|(ram[a+k+1]<<8)).toString(16).padStart(4,'0'));
  console.log(`0c${a.toString(16).padStart(6,'0')}: ${bytes}   u16: ${u16.join(' ')}`);
}
