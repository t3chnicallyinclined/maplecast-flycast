// _tx_desc_order.mjs — for one node+frame: walk the GFX2 record list and print the
// SHIPPED tiledesc entries (m, cnt-1, col, rowsMinusRow) per record — the engine
// builder's ground-truth tile order — alongside rebuild_tile_grid's synthetic order.
//   node _tx_desc_order.mjs <file.mcrr> <vframe> <node8hex> [selFilter]
import { readFileSync } from 'node:fs';
const path=process.argv[2], wantVf=+process.argv[3], nodeHex=process.argv[4], selF=process.argv[5]?+process.argv[5]:-1;

const buf=new Uint8Array(readFileSync(path));
const dv=new DataView(buf.buffer,buf.byteOffset,buf.byteLength);
let p=0; const u32=()=>{const v=dv.getUint32(p,true);p+=4;return v>>>0;};
u32();u32(); const nS=u32(),nD=u32(),nF=u32(),vb=u32(),pb=u32();u32();
const region=()=>{const a=u32(),l=u32();let t='';for(let i=0;i<8;i++){const c=buf[p+i];if(c)t+=String.fromCharCode(c);}p+=8;return{addr:a>>>0,len:l,tag:t};};
const sR=Array.from({length:nS},region), dR=Array.from({length:nD},region);
const ram=new Uint8Array(16*1024*1024);
let q=p+vb+pb;
for(const r of sR){const b=buf.subarray(q,q+r.len);q+=r.len;if(r.tag==='ram16')ram.set(b,0);else ram.set(b,r.addr&0xFFFFFF);}
p=q;
for(let f=0;f<nF;f++){u32();const vf=u32();const tail=u32();const dof=p;for(const r of dR)p+=r.len;p+=tail;
  if(vf===wantVf){let o=dof;for(const r of dR){ram.set(buf.subarray(o,o+r.len),r.addr&0xFFFFFF);o+=r.len;}break;}}

const r8=a=>ram[a&0xFFFFFF], r16=a=>ram[a&0xFFFFFF]|(ram[(a+1)&0xFFFFFF]<<8);
const r32r=a=>(ram[a&0xFFFFFF]|(ram[(a+1)&0xFFFFFF]<<8)|(ram[(a+2)&0xFFFFFF]<<16)|(ram[(a+3)&0xFFFFFF]<<24))>>>0;
const node=parseInt(nodeHex,16)|0x8C000000;
const gfx1=r32r(node+0x15C), gfx2=r32r(node+0x160);
const sid=r16(node+0x144)&0x7FFF, dc=r16(node+0xDC);
const DESC=0x8C1F9F9C;
console.log(`node=${nodeHex} sid=${sid} dc=${dc} gfx1=0x${gfx1.toString(16)} gfx2=0x${gfx2.toString(16)}`);
const cell=r32r(gfx2+(sid<<2))+gfx2;
const nrec=r16(cell);
let rec=cell+2, idx=dc;
console.log(`cell=0x${cell.toString(16)} nrec=${nrec}`);
for(let r=0;r<nrec;r++,rec+=8){
  const dx=(r16(rec)<<16)>>16, dy=(r16(rec+2)<<16)>>16, fl=r16(rec+4), sel=r16(rec+6);
  // shipped desc: read entries until the count-1 byte says done (count from first slot)
  const first=DESC+idx*4;
  const cnt=r8(first+1)+1;
  const ent=[];
  for(let k=0;k<cnt;k++){const a=DESC+(idx+k)*4;ent.push(`[m=${r8(a)} c${r8(a+2)} r${r8(a+3)}]`);}
  // rebuild synthetic order for comparison (2-row-band scan, as rebuild_tile_grid does)
  const off=r32r(gfx1+(sel<<2)), hdr=gfx1+off;
  const sw=r8(hdr+2), sh=r8(hdr+3), W=sw*8, H=sh*8;
  const d=Math.min(W,H)||8, m=(d===8)?8:(d===16)?16:32;
  const cols=W/m||1, rows=H/m||1;
  const syn=[];   // column-pair-major (the FIXED rebuild_tile_grid order, re_kb/68)
  for(let cp=0;cp<cols;cp+=2){const cw=Math.min(2,cols-cp);
    for(let by=0;by<rows;by+=2){const bh=Math.min(2,rows-by);
      for(let cx2=0;cx2<cw;cx2++)for(let ry=0;ry<bh;ry++){const cx=cp+cx2,row=by+ry;syn.push(`[m=${m} c${cx} r${rows-row}]`);}}}
  if(selF>=0&&sel!==selF){idx+=cnt;continue;}
  console.log(`rec${r} sel=${sel} dx=${dx} dy=${dy} fl=${fl.toString(16)} W=${W} H=${H} cols=${cols} rows=${rows} cnt=${cnt}`);
  console.log(`  shipped: ${ent.join(' ')}`);
  console.log(`  rebuild: ${syn.join(' ')}${syn.length!==cnt?'  (COUNT MISMATCH '+syn.length+')':''}`);
  console.log(`  MATCH: ${ent.join('')===syn.join('')?'YES':'NO <<<<'}`);
  idx+=cnt;
}
