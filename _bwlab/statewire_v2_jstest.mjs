// Validate the JS v2 decoder (web/render-replica/statewire_v2.mjs) against a
// cross-language test vector produced by the Python encoder:
//   python statewire_v2_gate.py <cap.mcrr> --dump vec.bin --dumpn 180
// Vector: [u32 dynbytes][u32 nframes] then per frame [u32 enclen][enc][u32 crc32(raw)].
// PASS => the JS client decodes byte-identically to the Python reference (and the
// C++ server, which the Python matches). Usage: node statewire_v2_jstest.mjs vec.bin
import fs from 'fs';
import zlib from 'zlib';
import { decodeV2, decodeV2Len } from '../web/render-replica/statewire_v2.mjs';

const buf = fs.readFileSync(process.argv[2]);
let p = 0;
const dynbytes = buf.readUInt32LE(p); p += 4;
const nframes  = buf.readUInt32LE(p); p += 4;

let key = null, ok = 0, bad = 0, firstBad = -1, lenBad = 0;
for (let i = 0; i < nframes; i++) {
  const enclen = buf.readUInt32LE(p); p += 4;
  const enc = buf.subarray(p, p + enclen); p += enclen;
  const wantCrc = buf.readUInt32LE(p); p += 4;

  // self-describing length must match the actual encoded length (locates the tails)
  if (decodeV2Len(enc) !== enclen) lenBad++;

  const blob = decodeV2(enc, key, dynbytes);
  if (enc[0] === 1) key = blob;                 // keyframe becomes the reference
  const gotCrc = zlib.crc32(blob) >>> 0;
  if (gotCrc === wantCrc && blob.length === dynbytes) ok++;
  else { bad++; if (firstBad < 0) firstBad = i; }
}

console.log(`== JS v2 decoder test ==`);
console.log(`vector : ${process.argv[2]}  (dynbytes ${dynbytes}, ${nframes} frames)`);
console.log(`decodeV2Len mismatches : ${lenBad}`);
console.log(`CORRECTNESS : ${bad === 0 && lenBad === 0
  ? 'PASS - JS decode == Python/C++ reference, byte-exact (crc32) all frames'
  : `FAIL - ${bad} crc mismatches (first frame ${firstBad}), ${lenBad} len mismatches`}`);
process.exit(bad === 0 && lenBad === 0 ? 0 : 1);
