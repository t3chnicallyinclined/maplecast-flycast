import { readFileSync } from 'node:fs';
const buf = new Uint8Array(readFileSync(process.argv[2]));
const wantV = +process.argv[3];
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
u32(); u32(); const nS = u32(), nD = u32(), nF = u32(), vB = u32(), pB = u32(); u32();
const reg = () => { const a = u32(), l = u32(); let t = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) t += String.fromCharCode(c); } p += 8; return { a, l, t }; };
const S = Array.from({ length: nS }, reg), D = Array.from({ length: nD }, reg);
p += vB + pB; const sd = S.map(r => { const b = buf.subarray(p, p + r.l); p += r.l; return b; });
const G = a => (a >>> 0) & 0xFFFFFF; const ram = new Uint8Array(16 * 1024 * 1024);
S.forEach((r, i) => { if (r.t === 'ram16') ram.set(sd[i], 0); else ram.set(sd[i], G(r.a)); });
for (let f = 0; f < nF; f++) {
  if (u32() !== 0x784D5246) throw 'bad'; const vf = u32(); const ts = u32(); const dO = p; for (const r of D) p += r.l;
  const nGfx = dv.getUint32(p, true); if (nGfx <= 64) { p += 4; for (let g = 0; g < nGfx; g++) { const len = dv.getUint32(p + 4, true); p += 8 + len; } } p += ts;
  if (vf === wantV) {
    let o = dO; for (const r of D) { ram.set(buf.subarray(o, o + r.l), G(r.a)); o += r.l; }
    const rd8 = a => ram[G(a)], rd32 = a => (ram[G(a)] | ram[G(a) + 1] << 8 | ram[G(a) + 2] << 16 | ram[G(a) + 3] << 24) >>> 0;
    for (let L = 0; L < 16; L++) { const cnt = rd8(0x8C2895E0 + L); if (!cnt || cnt > 0x60) continue; const base = 0x8C287DE0 + L * 0x180;
      for (let i = 0; i < cnt; i++) { const node = rd32(base + i * 4); const cat = rd8(node + 0x3);
        const f32 = a => { const w = rd32(a); return new DataView(new Uint32Array([w]).buffer).getFloat32(0, true); };
        console.log(`L${L} i${i} node=0x${(node >>> 0).toString(16)} cat=${cat} cull0x12C=${rd8(node + 0x12c)} ax=${f32(node + 0xE0).toFixed(1)} gfx2=0x${rd32(node + 0x160).toString(16)}`); } }
    break;
  }
}
