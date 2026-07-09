#!/usr/bin/env node
// zcs2_seq_gate.mjs — ZCS2 header/seq parser gate (synthetic, no ROM, no server)
//
// Extracts the ACTUAL worker-decode source from web/webgpu-test.html (the code
// that ships) and drives it with synthetic ZCS2 messages laid out exactly like
// maplecast_mirror.cpp emits them: 10B header + cam(bit3) + vf(bit5) + ord(bit6)
// + seq(bit7) + chunk. The zstd stage is stubbed with an identity Decompress so
// the gate isolates the layer the seq change touched: header offsets, epoch
// handling, bit7 seq-gap detection, soaInverse, and the frameSize belt.
//
// Scenarios:
//   1. clean epoch: stream-start + N frames, seq 0..N     -> N+1 frames, 0 desync
//   2. mid-epoch msg loss (seq gap)                        -> deterministic desync, no garbage frame
//   3. resync at next stream-start after a gap             -> decoding resumes
//   4. old-server compat: no bit7                          -> decodes fine
//   5. full header: cam+vf+ord+seq                         -> offsets right, cam/vf/ord surfaced
//   6. SoA inner + seq                                     -> soaInverse output byte-exact
//   7. epoch gap (relay dropped a whole epoch)             -> desync (pre-existing check intact)
//
// Run: node zcs2_seq_gate.mjs   (from tools/render-replica-poc/, exits 0 on PASS)

import {readFileSync, writeFileSync, mkdtempSync} from 'fs';
import {tmpdir} from 'os';
import {join, dirname} from 'path';
import {pathToFileURL, fileURLToPath} from 'url';

const HERE = dirname(fileURLToPath(import.meta.url));
const HTML = readFileSync(join(HERE, '../../web/webgpu-test.html'), 'utf8');

// --- extract the worker source (the shipped code, not a copy) ---------------
const m = HTML.match(/const src=`\n([\s\S]*?)`;\n  try\{\n    const w=new Worker/);
if (!m) { console.error('FAIL: could not extract worker src from webgpu-test.html'); process.exit(1); }
const workerSrc = m[1];
if (!/seq gap/.test(workerSrc)) { console.error('FAIL: extracted worker src has no seq-gap check'); process.exit(1); }

// --- identity "Decompress" stub (fzstd API shape: ctor(cb), push(chunk)) ----
const stubDir = mkdtempSync(join(tmpdir(), 'zcs2gate-'));
const stubPath = join(stubDir, 'fzstd_stub.mjs');
writeFileSync(stubPath, 'export class Decompress{constructor(cb){this.cb=cb}push(c){this.cb(c)}}\n');

// --- run the worker source with a mock `self` --------------------------------
function makeWorker() {
  const posts = [];
  const self = {postMessage: (msg) => posts.push(msg)};
  // The worker src assigns self.onmessage and uses performance.now() (node global).
  new Function('self', workerSrc)(self);
  const send = async (data) => { await self.onmessage({data}); };
  return {posts, send};
}

// --- synthetic wire builders --------------------------------------------------
// Inner legacy frame: frameSize(4)+frameNum(4)+pvr(64)+taSize(4)+dPay(4)+[delta]+checksum(4)+count(4)
function legacyInner(frameNum) {
  const buf = new Uint8Array(88);
  const dv = new DataView(buf.buffer);
  dv.setUint32(0, 84, true);          // frameSize = total - 4
  dv.setUint32(4, frameNum, true);
  dv.setUint32(72, 0, true);          // taSize
  dv.setUint32(76, 0, true);          // deltaPayloadSize
  return buf;
}

// SoA (flags bit1) inner + its expected post-inverse legacy bytes.
function soaInnerPair(frameNum) {
  const runs = [{off: 0x100, data: [1, 2, 3]}, {off: 0x180, data: [4, 5]}];
  const nRuns = runs.length, dataB = runs.reduce((a, r) => a + r.data.length, 0);
  const v2Sec = 4 + nRuns * 6 + dataB;
  const legacySec = nRuns * 6 + dataB + 4;
  const tail = new Uint8Array(8);      // checksum(4) + dirtyPageCount(4) = 0
  // v2 form
  const v2 = new Uint8Array(80 + v2Sec + tail.length);
  const dv = new DataView(v2.buffer);
  dv.setUint32(0, 80 + legacySec + tail.length - 4, true);  // stale pre-transform frameSize == legacy value
  dv.setUint32(4, frameNum, true);
  dv.setUint32(72, 99, true);                               // taSize != dPay (delta frame)
  dv.setUint32(76, v2Sec, true);
  dv.setUint32(80, nRuns, true);
  let prev = 0, off = 84;
  for (const r of runs) { dv.setUint32(off, r.off - prev, true); prev = r.off; off += 4; }
  for (const r of runs) { dv.setUint16(off, r.data.length, true); off += 2; }
  for (const r of runs) { v2.set(r.data, off); off += r.data.length; }
  v2.set(tail, 80 + v2Sec);
  // expected legacy form
  const leg = new Uint8Array(80 + legacySec + tail.length);
  const lv = new DataView(leg.buffer);
  leg.set(v2.subarray(0, 80), 0);
  lv.setUint32(76, legacySec, true);
  off = 80;
  for (const r of runs) {
    lv.setUint32(off, r.off, true); lv.setUint16(off + 4, r.data.length, true); off += 6;
    leg.set(r.data, off); off += r.data.length;
  }
  lv.setUint32(off, 0xFFFFFFFF, true); off += 4;
  leg.set(tail, off);
  return {v2, leg};
}

// ZCS2 message around a "compressed" (identity-stub) chunk.
function zcs2Msg({epoch, start = false, soa = false, seq = null, cam = null, vf = null, ord = null, inner}) {
  const camLen = cam ? 132 : 0, vfLen = vf != null ? 4 : 0;
  const ordLen = ord ? 1 + 3 * ord.length : 0, seqLen = seq != null ? 2 : 0;
  const buf = new Uint8Array(10 + camLen + vfLen + ordLen + seqLen + inner.length);
  const dv = new DataView(buf.buffer);
  buf.set([0x5A, 0x43, 0x53, 0x32], 0);           // 'ZCS2'
  buf[4] = epoch;
  buf[5] = (start ? 1 : 0) | (soa ? 2 : 0) | (camLen ? 8 : 0) | (vfLen ? 32 : 0)
         | (ordLen ? 64 : 0) | (seqLen ? 128 : 0);
  dv.setUint32(6, inner.length, true);            // innerSize (identity stub)
  let o = 10;
  if (camLen) { dv.setUint32(o, cam.sid, true); o += 132; }
  if (vfLen) { dv.setUint32(o, vf, true); o += 4; }
  if (ordLen) { buf[o++] = ord.length; for (const r of ord) { buf[o++] = r.cls; dv.setUint16(o, r.cnt, true); o += 2; } }
  if (seqLen) { dv.setUint16(o, seq, true); o += 2; }
  buf.set(inner, o);
  return {b: buf.buffer, off: 0, len: buf.length};
}

// --- assertions ---------------------------------------------------------------
let failures = 0;
function check(name, cond, detail = '') {
  if (cond) console.log(`  PASS ${name}`);
  else { console.log(`  FAIL ${name} ${detail}`); failures++; }
}
const frames = (p) => p.filter(x => x.frame);
const desyncs = (p) => p.filter(x => x.desync);

async function boot() {
  const w = makeWorker();
  await w.send({init: pathToFileURL(stubPath).href});
  if (!w.posts.some(p => p.ready)) { console.error('FAIL: worker never posted ready'); process.exit(1); }
  w.posts.length = 0;
  return w;
}

// 1. clean epoch
{
  console.log('scenario 1: clean epoch, seq 0..3');
  const w = await boot();
  for (let i = 0; i <= 3; i++)
    await w.send(zcs2Msg({epoch: 1, start: i === 0, seq: i, inner: legacyInner(100 + i)}));
  check('4 frames decoded', frames(w.posts).length === 4, `got ${frames(w.posts).length}`);
  check('0 desyncs', desyncs(w.posts).length === 0, JSON.stringify(desyncs(w.posts)));
}

// 2. mid-epoch loss
{
  console.log('scenario 2: msg seq=2 lost mid-epoch');
  const w = await boot();
  await w.send(zcs2Msg({epoch: 1, start: true, seq: 0, inner: legacyInner(100)}));
  await w.send(zcs2Msg({epoch: 1, seq: 1, inner: legacyInner(101)}));
  await w.send(zcs2Msg({epoch: 1, seq: 3, inner: legacyInner(103)}));   // 2 was dropped
  check('exactly 2 frames decoded', frames(w.posts).length === 2, `got ${frames(w.posts).length}`);
  const d = desyncs(w.posts);
  check('deterministic seq-gap desync', d.length === 1 && /seq gap 1->3/.test(d[0].desync), JSON.stringify(d));

  // 3. resync on next stream-start
  console.log('scenario 3: resync at next stream-start');
  w.posts.length = 0;
  await w.send(zcs2Msg({epoch: 2, start: true, seq: 0, inner: legacyInner(104)}));
  await w.send(zcs2Msg({epoch: 2, seq: 1, inner: legacyInner(105)}));
  check('decoding resumed', frames(w.posts).length === 2, `got ${frames(w.posts).length}`);
  check('resumed flag posted', w.posts.some(p => p.resumed));
}

// 4. old-server compat (no bit7)
{
  console.log('scenario 4: no seq field (old server)');
  const w = await boot();
  for (let i = 0; i <= 2; i++)
    await w.send(zcs2Msg({epoch: 1, start: i === 0, inner: legacyInner(200 + i)}));
  check('3 frames, no desync', frames(w.posts).length === 3 && desyncs(w.posts).length === 0);
}

// 5. full header: cam + vf + ord + seq
{
  console.log('scenario 5: cam+vf+ord+seq offsets');
  const w = await boot();
  const cam = {sid: 0x0B};
  const ord = [{cls: 0, cnt: 43}, {cls: 1, cnt: 5}, {cls: 2, cnt: 12}];
  await w.send(zcs2Msg({epoch: 1, start: true, seq: 0, cam, vf: 777, ord, inner: legacyInner(300)}));
  await w.send(zcs2Msg({epoch: 1, seq: 1, cam, vf: 778, ord, inner: legacyInner(301)}));
  const f = frames(w.posts);
  check('2 frames with full header', f.length === 2 && desyncs(w.posts).length === 0);
  check('vframe surfaced', f[0].vf === 777 && f[1].vf === 778, JSON.stringify(f.map(x => x.vf)));
  check('cam block 132B', f[0].cam && f[0].cam.byteLength === 132);
  check('ord descriptor 1+3n', f[0].ord && f[0].ord.byteLength === 1 + 3 * ord.length);
  check('frame bytes intact', new Uint8Array(f[0].frame, 0, f[0].len).length === 88 &&
    new DataView(f[0].frame).getUint32(4, true) === 300);
}

// 6. SoA inner + seq: soaInverse byte-exact
{
  console.log('scenario 6: SoA inverse under seq');
  const w = await boot();
  const {v2, leg} = soaInnerPair(400);
  await w.send(zcs2Msg({epoch: 1, start: true, soa: true, seq: 0, inner: v2}));
  const f = frames(w.posts);
  check('SoA frame decoded', f.length === 1, JSON.stringify(desyncs(w.posts)));
  if (f.length === 1) {
    const got = new Uint8Array(f[0].frame, 0, f[0].len);
    check('soaInverse byte-exact', got.length === leg.length && got.every((b, i) => b === leg[i]));
  }
}

// 7. epoch gap still detected
{
  console.log('scenario 7: epoch gap (whole epoch lost)');
  const w = await boot();
  await w.send(zcs2Msg({epoch: 1, start: true, seq: 0, inner: legacyInner(500)}));
  await w.send(zcs2Msg({epoch: 3, seq: 1, inner: legacyInner(501)}));  // epoch 2 never seen
  const d = desyncs(w.posts);
  check('epoch-gap desync', d.length === 1 && /epoch gap/.test(d[0].desync), JSON.stringify(d));
}

console.log(failures === 0 ? '\nALL PASS' : `\n${failures} FAILURE(S)`);
process.exit(failures === 0 ? 0 : 1);
