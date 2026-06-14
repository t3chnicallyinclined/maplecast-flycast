// diag_stage_cameras.mjs — drive the StageClient through the SAME PVR2Renderer for MANY
// camera frames out of _satlive.mcrr, counting rendered pixels + reprojected on-screen verts
// per frame. Detects whether a LIVE camera frame collapses the stage to black (the MARGIN
// cull dropping everything), which the single-frame shot harness would miss.
import './webgpu-headless.mjs';
import { initDevice } from './webgpu-headless.mjs';
import { readFileSync } from 'node:fs';
import { PNG } from 'pngjs';
import { pathToFileURL } from 'node:url';
import { join, dirname } from 'node:path';

const HERE = dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const RECF = join(HERE, '..', '..', '_satlive.mcrr');
const STAGE_DIR = join(HERE, '..', '..', 'web', 'test-atlas', 'stages');
const W = 640, H = 480;

globalThis.fetch = async (url) => {
  let p = String(url); if (p.startsWith('file://')) p = new URL(p).pathname.replace(/^\/([A-Za-z]:)/, '$1');
  const buf = readFileSync(p);
  return { ok: true, json: async () => JSON.parse(buf.toString('utf8')), blob: async () => ({ _png: buf }) };
};
globalThis.createImageBitmap = async (blob) => { const png = PNG.sync.read(blob._png); return { width: png.width, height: png.height, _rgba: new Uint8Array(png.data) }; };

// ---- load ALL frames' RAM (dynamic regions) ----
function loadAllFrames(path) {
  const buf = new Uint8Array(readFileSync(path));
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
    p += taSize; frames.push({ dynOff });
  }
  return { buf, baseRam, dynamicRegs, frames, nFrames, G };
}
const { buf, baseRam, dynamicRegs, frames, nFrames, G } = loadAllFrames(RECF);
function ramAt(fi) { const ram = baseRam.slice(); let o = frames[fi].dynOff; for (const r of dynamicRegs) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } return ram; }
function mat16(ram, addr) { const rd = new DataView(ram.buffer, ram.byteOffset, ram.byteLength); const m = new Float32Array(16); for (let i = 0; i < 16; i++) m[i] = rd.getFloat32(G(addr + i * 4), true); return m; }

const { device } = await initDevice();
const W_DIR = new URL('../../web/webgpu/', import.meta.url);
const { PVR2Renderer } = await import(new URL('pvr2-renderer.mjs', W_DIR));
const { StageClient, STAGE_PVRSNAP } = await import(new URL('stage-client.mjs', W_DIR));
const R = new PVR2Renderer(); R.dev = device; R.fmt = 'rgba8unorm'; R._init(W, H);
device.queue.copyExternalImageToTexture = (src, dst, size) => { const bm = src.source; const [w, h] = size; device.queue.writeTexture({ texture: dst.texture }, bm._rgba, { bytesPerRow: w * 4, rowsPerImage: h }, [w, h, 1]); };

const ram0 = ramAt(0);
const stageId = ram0[G(0x8C289638)];
const stage = new StageClient(pathToFileURL(STAGE_DIR).href);
stage.attachDevice(device);
stage.setState(stageId, 0);
for (let i = 0; i < 200 && stage.stageId !== stageId; i++) await new Promise(r => setTimeout(r, 25));

function makeRT() { const color = device.createTexture({ size: [W, H], format: R.fmt, usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC | GPUTextureUsage.TEXTURE_BINDING }); const depth = device.createTexture({ size: [W, H], format: 'depth32float', usage: GPUTextureUsage.RENDER_ATTACHMENT }); return { color, depth, colorView: color.createView(), depthView: depth.createView(), width: W, height: H }; }

async function countPx(rt) {
  const bytesPerRow = Math.ceil(W * 4 / 256) * 256;
  const readBuf = device.createBuffer({ size: bytesPerRow * H, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
  const enc = device.createCommandEncoder();
  enc.copyTextureToBuffer({ texture: rt.color }, { buffer: readBuf, bytesPerRow, rowsPerImage: H }, [W, H, 1]);
  device.queue.submit([enc.finish()]);
  await readBuf.mapAsync(GPUMapMode.READ);
  const mapped = new Uint8Array(readBuf.getMappedRange()); let nz = 0;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) { const s = y * bytesPerRow + x * 4; if (mapped[s] | mapped[s + 1] | mapped[s + 2]) nz++; }
  readBuf.unmap(); readBuf.destroy(); return nz;
}

const probeFrames = [];
for (let i = 0; i < 12; i++) probeFrames.push(Math.floor(nFrames * i / 12));
console.log('stage_id=' + stageId + ' nFrames=' + nFrames);
for (const fi of probeFrames) {
  const ram = ramAt(fi);
  const M1 = mat16(ram, 0x8C2D6B18), M2 = mat16(ram, 0x8C2D6AD8);
  stage.setCamera(M1, M2);
  const p = stage.parsed;
  const rt = makeRT();
  R.renderFrame(p, stage.texMgr, STAGE_PVRSNAP, null, { singlePass: true, noSort: true, drawOpaque: true, drawPunch: false, drawTrans: true }, rt);
  device.queue.submit([R._lastEncoder.finish()]);
  const nz = await countPx(rt);
  console.log(`frame ${String(fi).padStart(3)}  M2[15]=${M2[15].toFixed(1)} M2[0]=${M2[0].toFixed(3)}  verts=${p.vertexCount} op=${p.opaque.length}  nz=${nz} (${(100*nz/(W*H)).toFixed(1)}%)`);
  rt.color.destroy(); rt.depth.destroy();
}
process.exit(0);
