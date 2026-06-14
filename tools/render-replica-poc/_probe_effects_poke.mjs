// _probe_effects_poke.mjs — reproduce __pokeEffect + EffectsClient.readEffectNodes in Node
// (NO GPU needed — it's a pure RAM read) to find why the poked effect draws 0 quads.
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { pathToFileURL } from 'node:url';

const HERE = dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const RECF = join(HERE, '..', '..', '_satlive.mcrr');

// ---- load a live RAM image (mid frame) ----
const buf = new Uint8Array(readFileSync(RECF));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
if (u32() !== 0x5252434D) throw new Error('bad MCRR');
u32(); const nStatic = u32(), nDynamic = u32(), nFrames = u32(), vramBytes = u32(), pvrBytes = u32(); u32();
const region = () => { const addr = u32(), len = u32(); let tag = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) tag += String.fromCharCode(c); } p += 8; return { addr: addr >>> 0, len, tag }; };
const staticRegs = Array.from({ length: nStatic }, region);
const dynamicRegs = Array.from({ length: nDynamic }, region);
p += vramBytes + pvrBytes;
const staticData = staticRegs.map(r => { const b = buf.subarray(p, p + r.len); p += r.len; return b; });
const frameStart = p;
const G = a => (a >>> 0) & 0xFFFFFF;
const baseRam = new Uint8Array(16 * 1024 * 1024);
staticRegs.forEach((r, i) => { if (r.tag === 'ram16') baseRam.set(staticData[i], 0); else baseRam.set(staticData[i], G(r.addr)); });
p = frameStart; const frames = [];
for (let f = 0; f < nFrames; f++) {
  if (u32() !== 0x784D5246) throw new Error(`frame ${f}: bad FRMx`);
  u32(); const taSize = u32();
  const dynOff = p; for (const r of dynamicRegs) p += r.len;
  const nGfx = (p + 4 <= buf.length) ? dv.getUint32(p, true) : 0;
  if (nGfx <= 64) { p += 4; for (let g = 0; g < nGfx && p + 8 <= buf.length; g++) { const len = dv.getUint32(p + 4, true); p += 8 + len; } }
  p += taSize;
  frames.push({ dynOff });
}
const fr = frames[Math.floor(frames.length * 0.6)];
const ram = baseRam.slice(); { let o = fr.dynOff; for (const r of dynamicRegs) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } }

const RAM_LO = 0x00FFFFFF;
const rd = {
  u8: a => ram[G(a)],
  u16: a => ram[G(a)] | (ram[G(a)+1] << 8),
  u32: a => (ram[G(a)] | (ram[G(a)+1]<<8) | (ram[G(a)+2]<<16) | (ram[G(a)+3]<<24)) >>> 0,
  f32: a => { const b = ram.subarray(G(a), G(a)+4); return new DataView(b.buffer, b.byteOffset, 4).getFloat32(0, true); },
};

// directory base
const dirBase = rd.u32(0x0CED0008) >>> 0;
console.log('dirBase = 0x' + dirBase.toString(16), ' (low24 = 0x' + (dirBase & RAM_LO).toString(16) + ')');

// ---- replicate __pokeEffect(0, 320, 240) ----
const dirIdx = 0, sx = 320, sy = 240;
const entry = (dirBase + dirIdx * 0x10) >>> 0;
const NODE = 0x8C26AA54 + 200 * 0x1D0;
const w32 = (a, v) => { a &= RAM_LO; ram[a]=v&0xff; ram[a+1]=(v>>>8)&0xff; ram[a+2]=(v>>>16)&0xff; ram[a+3]=(v>>>24)&0xff; };
const wf  = (a, f) => { const b=new DataView(new ArrayBuffer(4)); b.setFloat32(0,f,true); w32(a, b.getUint32(0,true)); };
w32(NODE+0x15C, entry); ram[(NODE+0x144)&RAM_LO]=7; ram[(NODE+0x3)&RAM_LO]=1;
wf(NODE+0xE0, sx); wf(NODE+0xE4, sy); ram[(NODE+0x130)&RAM_LO]=0; ram[(NODE+0x131)&RAM_LO]=0;
const L=5, cnt=rd.u8(0x8C2895E0+L), slot=Math.min(cnt,0x5F);
w32(0x8C287DE0 + L*0x180 + slot*4, NODE); ram[(0x8C2895E0+L)&RAM_LO]=slot+1;
console.log(`poke: entry=0x${entry.toString(16)} NODE=0x${NODE.toString(16)} layer=${L} cnt(before)=${cnt} slot=${slot}`);
console.log('  NODE+0x15C =', rd.u32(NODE+0x15C).toString(16), 'gfxLo=', (rd.u32(NODE+0x15C)&0x0FFFFFFF).toString(16));

// ---- run EffectsClient.readEffectNodes verbatim ----
const { EffectsClient } = await import(pathToFileURL(join(HERE, '..', '..', 'web', 'render-replica', 'effects-client.mjs')).href);
const efx = new EffectsClient('x');
const nodes = efx.readEffectNodes(rd);
console.log('readEffectNodes ->', nodes.length, 'nodes');
for (const n of nodes) console.log('  ', JSON.stringify(n));

// ---- step through the gate manually for the poked node ----
const EFX_BANK_LO=0x0CED0000, EFX_BANK_HI=0x0CEE0000;
const gfx = rd.u32(NODE+0x15C)>>>0, gfxLo = gfx & 0x0FFFFFFF;
const dirLo = dirBase & RAM_LO;
const off = ((gfx & RAM_LO) - dirLo);
console.log(`\nmanual gate for poked NODE:`);
console.log(`  gfx=0x${gfx.toString(16)} gfxLo=0x${gfxLo.toString(16)} inBank=${gfxLo>=EFX_BANK_LO&&gfxLo<EFX_BANK_HI}`);
console.log(`  off=(gfx&RAM_LO)-dirLo = 0x${(gfx&RAM_LO).toString(16)} - 0x${dirLo.toString(16)} = ${off} (alignedTo16=${(off&0xF)===0})`);
const idx = off>>4;
const e = (dirBase + idx*0x10)>>>0; const e0 = rd.u32(e)>>>0;
console.log(`  idx=${idx} e0=0x${e0.toString(16)} w=${e0&0xffff} h=${(e0>>>16)&0xffff}`);
console.log(`  slot-table sees NODE? walking layer ${L}: count=${rd.u8(0x8C2895E0+L)}`);
