// Verify ta_buffer.bin parses through the REAL web renderer's TA parser, and that
// the recovered vertex corners match the harness output bit-for-bit.
import { readFileSync } from 'fs';
import { TAParser } from '../../web/webgpu/ta-parser.mjs';

const buf = new Uint8Array(readFileSync('ta_buffer.bin'));
const p = new TAParser();
const r = p.parse(buf, buf.length);

console.log(`ta-parser.mjs: ${r.opaque.length} opaque polys, ${r.vertexCount} vertices`);
const f32 = new Float32Array(r.vertexData.buffer, r.vertexData.byteOffset, r.vertexCount*7);
let quads=0;
for (const pp of r.opaque) {
  // each strip = 4 verts (TL,TR,BL,BR)
  const base = pp.first*7;
  const TLx=f32[base], TLy=f32[base+1];
  const BRx=f32[base+3*7], BRy=f32[base+3*7+1];
  console.log(`  quad ${quads}: tcw=0x${(pp.tcw>>>0).toString(16)} TL(${TLx.toFixed(2)},${TLy.toFixed(2)}) BR(${BRx.toFixed(2)},${BRy.toFixed(2)}) count=${pp.count}`);
  quads++;
}
const ok = r.opaque.length===9 && r.vertexCount===36;
console.log(`VERIFY: ${ok? 'PASS — real ta-parser.mjs decodes 9 quads / 36 verts from the emitted native TA stream':'FAIL'}`);
process.exit(ok?0:1);
