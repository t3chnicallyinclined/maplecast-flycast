// Byte-exact gate for the browser TDW decoder.
//
// Decodes a captured TDW stream through the wasm's browser ABI and prints one
// line per decoded frame: "<frameNum> <taLen> <fnv64(ta) hex>". Diff that against
// the native reference (native-client tdw.rs compiled natively over the same
// capture) — they MUST be identical. This is what proved 481/481 byte-exact.
//
//   # 1. capture a live TDW stream (writes [len u32 LE][msg] framing):
//   node ../_bwlab/cap_tdw.mjs wss://play.nobd.net/ws cap.bin 60
//   # 2. dump per-frame hashes from the wasm:
//   node gate.mjs ../web/webgpu/tdw.wasm cap.bin > wasm.txt
//   # 3. compare to the native reference (see tdw-wasm/README.md).
import fs from 'fs';

const [, , wasmPath, capPath] = process.argv;
if (!wasmPath || !capPath) { console.error('usage: node gate.mjs <tdw.wasm> <capture.bin>'); process.exit(2); }

const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasmPath), {});
const ex = instance.exports;
const U8 = () => new Uint8Array(ex.memory.buffer);
const DV = () => new DataView(ex.memory.buffer);

const MASK = (1n << 64n) - 1n;
function fnv64(bytes) {
  let h = 1469598103934665603n;
  for (let i = 0; i < bytes.length; i++) { h = (h ^ BigInt(bytes[i])) & MASK; h = (h * 1099511628211n) & MASK; }
  return h.toString(16).padStart(16, '0');
}

const cap = fs.readFileSync(capPath);
const dv = new DataView(cap.buffer, cap.byteOffset, cap.byteLength);
const dec = ex.tdw_new();
let off = 0, frames = 0;
const out = [];
while (off + 4 <= cap.length) {
  const len = dv.getUint32(off, true); off += 4;
  if (off + len > cap.length) break;
  const msg = cap.subarray(off, off + len); off += len;
  if (msg.length < 4) continue;
  const m = String.fromCharCode(msg[0], msg[1], msg[2], msg[3]);
  if (m === 'TDWS') { const p = ex.tdw_inbuf(dec, msg.length); U8().set(msg, p); ex.tdw_snapshot(dec, msg.length); continue; }
  if (m !== 'TDW1' && m !== 'TDW2') continue;
  const p = ex.tdw_inbuf(dec, msg.length); U8().set(msg, p);
  const h = ex.tdw_feed(dec, msg.length);
  if (h === 0) continue;
  const d = DV();
  if (!d.getUint32(h, true)) continue;
  const frameNum = d.getUint32(h + 4, true);
  const taPtr = d.getUint32(h + 8, true), taLen = d.getUint32(h + 12, true);
  out.push(`${frameNum} ${taLen} ${fnv64(new Uint8Array(ex.memory.buffer, taPtr, taLen))}`);
  frames++;
}
process.stdout.write(out.join('\n') + (out.length ? '\n' : ''));
process.stderr.write(`decoded ${frames} frames\n`);
