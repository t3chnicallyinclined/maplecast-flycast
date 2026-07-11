// _zz_anim_flag_audit.mjs — MASTER per-character / per-ANIMATION render-flag audit.
// Joins THREE catalogs to produce the definitive "which named animation of which character
// uses which render transform, and does render_frame handle it" map:
//   (1) skin-studio anim catalog  web/anim/PL{NN}.json  (group name -> subanims -> cells -> sprite_id)
//   (2) GFX2 dispatch  web/render-replica/gfx/PL{NN}_gfx2.bin  (sprite_id -> parts -> flags u16)
//   (3) render_frame's handled-flag set (2026-07-10: BOTH 0x8000 texU H-flip AND 0x4000 texV
//       V-flip — submit_1244b0 reads both; q->mirror=facing^0x8000, q->mirror_v=0x4000)
// Output: every animation GROUP that references a part carrying an UNHANDLED flag bit, labeled by
// its human name — so "Storm / Lightning attack uses 0x8000 (unhandled)" instead of a bare sel.
// This is the render-side twin of the carve catalog gate, driven by the real animation catalog.
//   node _zz_anim_flag_audit.mjs [--animdir=<path>] [--gaps-only] [PL2A ...]
import { readFileSync, existsSync } from 'node:fs';

const argv = k => { const p=process.argv.find(a=>a.startsWith(k+'=')); return p?p.split('=')[1]:null; };
const ANIMDIR = argv('--animdir') || 'C:/Users/trist/projects/mvc2-skin-studio/web/anim';
const GFXDIR  = new URL('../../web/render-replica/gfx/', import.meta.url);
const onlyChars = process.argv.filter(a=>/^PL[0-9A-Fa-f]{2}$/.test(a)).map(s=>s.toUpperCase());
const gapsOnly = process.argv.includes('--gaps-only');

const HANDLED = 0xC000;                    // render_frame submit_1244b0 reads 0x8000 (texU H) + 0x4000 (texV V)
const r16=(b,a)=>b[a]|(b[a+1]<<8), r32=(b,a)=>(b[a]|(b[a+1]<<8)|(b[a+2]<<16)|(b[a+3]<<24))>>>0;

// GFX2: sprite_id -> Set(flags) over its cell records
function gfx2FlagsBySid(g2){
  const out=new Map(); const nsid=r32(g2,0)>>>2; if(!nsid||nsid>0x40000) return out;
  for(let sid=0; sid<nsid; sid++){
    const disp=r32(g2,sid*4); if(!disp||disp+2>g2.length) continue;
    const nrec=r16(g2,disp); if(!nrec||nrec>256) continue;
    const set=new Set(); let rec=disp+2;
    for(let r=0;r<nrec&&rec+8<=g2.length;r++,rec+=8) set.add(r16(g2,rec+4));
    out.set(sid,set);
  }
  return out;
}

const perCharGaps=[];   // {char, group, sids, flagBits:Set}
let charsDone=0, groupsTotal=0, groupsGap=0;
for(let cid=0; cid<=0x3A; cid++){
  const nn=cid.toString(16).toUpperCase().padStart(2,'0'); const PL='PL'+nn;
  if(onlyChars.length && !onlyChars.includes(PL)) continue;
  const animF=`${ANIMDIR}/${PL}.json`, g2F=new URL(`${PL}_gfx2.bin`,GFXDIR);
  if(!existsSync(animF)||!existsSync(g2F)) continue;
  charsDone++;
  const anim=JSON.parse(readFileSync(animF,'utf8'));
  const flagsBySid=gfx2FlagsBySid(new Uint8Array(readFileSync(g2F)));
  const groups=anim.groups||{};
  for(const gk of Object.keys(groups)){
    const g=groups[gk]; groupsTotal++;
    const sids=new Set(); const flagBits=new Set(); let allFlags=new Set();
    for(const sa of (g.subanims||[])) for(const c of (sa.cells||[])){
      const sid=c.sprite_id; sids.add(sid);
      const fs=flagsBySid.get(sid&0x7FFF); if(!fs) continue;
      for(const f of fs){ allFlags.add(f); const unh=f&~HANDLED&0xFFFF; if(unh) for(let b=1;b<0x10000;b<<=1) if(unh&b) flagBits.add(b); }
    }
    if(flagBits.size){ groupsGap++; perCharGaps.push({char:PL,name:anim.name,group:g.name||gk,kind:g.kind,
      nsids:sids.size, flagBits:[...flagBits].sort((a,b)=>a-b)}); }
  }
}

console.log(`\n=== PER-ANIMATION RENDER-FLAG AUDIT — ${charsDone} chars, ${groupsTotal} animation groups (${groupsGap} touch an UNHANDLED flag) ===`);
console.log(`render_frame currently handles ONLY 0x4000 (texU mirror). Groups below reference a part with a flag render_frame ignores:\n`);
const bybit=new Map();
for(const r of perCharGaps){ for(const b of r.flagBits) bybit.set(b,(bybit.get(b)||0)+1); }
console.log('unhandled-flag -> # of animation groups that use it:');
for(const [b,c] of [...bybit].sort((a,b)=>b[1]-a[1])) console.log(`  0x${b.toString(16).padStart(4,'0')}  ${String(c).padStart(4)} groups`);
console.log(`\nsample gap groups (char / animation / unhandled flags):`);
for(const r of perCharGaps.slice(0, gapsOnly?9999:60))
  console.log(`  ${r.char} ${r.char==='PL2A'?'*':' '} ${(r.char+' '+r.name).padEnd(16)} | ${String(r.group).slice(0,34).padEnd(34)} | ${r.flagBits.map(b=>'0x'+b.toString(16)).join(' ')}`);
console.log(`\n(When render_frame accounts for every flag bit, this list is empty. Labels come from the skin-studio anim catalog.)`);
