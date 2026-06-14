// render_mirror_left.mjs — render the LEFT (TA-truth) pane from a captured /ws stream.
// The /ws stream MULTIPLEXES: ZCST (SYNC + TA deltas) AND raw GSTA/PALF/OBJS/OBJF/MCSV.
// FrameDecoder only eats ZCST; feed it ONLY ZCST messages, in order, preserving syncPending.
//   node render_mirror_left.mjs <cap.zcst> <out.png> [frameIdx=-1]
import './webgpu-headless.mjs';
import { initDevice } from './webgpu-headless.mjs';
import { readFileSync, writeFileSync } from 'node:fs';
import { PNG } from 'pngjs';

const W_DIR = new URL('../../web/webgpu/', import.meta.url);
const { PVR2Renderer }   = await import(new URL('pvr2-renderer.mjs', W_DIR));
const { TAParser }       = await import(new URL('ta-parser.mjs', W_DIR));
const { TextureManager } = await import(new URL('texture-manager.mjs', W_DIR));
const { FrameDecoder }   = await import(new URL('frame-decoder.mjs', W_DIR));

const VRAM_SIZE = 8 * 1024 * 1024;
const MAGIC_ZCST = 0x5453435A;

const capPath = process.argv[2];
const outPath = process.argv[3] || 'left.png';
const frameIdx = process.argv[4] !== undefined ? +process.argv[4] : -1;
const W = 640, H = 480;

function synthSnap(w, h) {
    const s = new Uint32Array(16);
    const tx = Math.max(0, Math.round(w / 32) - 1) & 0x3F, ty = Math.max(0, Math.round(h / 32) - 1) & 0x3F;
    s[0] = (tx & 0x3F) | ((ty & 0x3F) << 16);
    return s;
}
function makeRT(dev, fmt, w, h) {
    const color = dev.createTexture({ size: [w, h], format: fmt, usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC | GPUTextureUsage.TEXTURE_BINDING });
    const depth = dev.createTexture({ size: [w, h], format: 'depth32float', usage: GPUTextureUsage.RENDER_ATTACHMENT });
    return { color, depth, colorView: color.createView(), depthView: depth.createView(), width: w, height: h };
}
async function readback(dev, tex, w, h, fmt) {
    const bpr = Math.ceil(w * 4 / 256) * 256;
    const buf = dev.createBuffer({ size: bpr * h, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const enc = dev.createCommandEncoder();
    enc.copyTextureToBuffer({ texture: tex }, { buffer: buf, bytesPerRow: bpr }, [w, h]);
    dev.queue.submit([enc.finish()]);
    await buf.mapAsync(GPUMapMode.READ);
    const src = new Uint8Array(buf.getMappedRange());
    const out = new Uint8Array(w * h * 4);
    for (let y = 0; y < h; y++) out.set(src.subarray(y * bpr, y * bpr + w * 4), y * w * 4);
    buf.unmap();
    return out;
}
function writePNG(path, rgba, w, h) {
    const png = new PNG({ width: w, height: h });
    png.data = Buffer.from(rgba);
    writeFileSync(path, PNG.sync.write(png));
}

// split length-prefixed messages, keep ONLY ZCST ones
function zcstMessages(file) {
    const dv = new DataView(file.buffer, file.byteOffset, file.byteLength);
    let off = 0; const out = [];
    while (off + 4 <= file.length) {
        const len = dv.getUint32(off, true); off += 4;
        if (!len || off + len > file.length) break;
        const m = file.subarray(off, off + len); off += len;
        const md = new DataView(m.buffer, m.byteOffset, m.byteLength);
        if (md.getUint32(0, true) === MAGIC_ZCST) out.push(m);
    }
    return out;
}

async function main() {
    const { device, info } = await initDevice();
    console.log('[gpu]', info.vendor || info.description, info.device || '');
    const R = new PVR2Renderer(); R.dev = device; R.fmt = 'rgba8unorm';
    R._init(W, H);
    const rt = makeRT(device, R.fmt, W, H);
    const D = new FrameDecoder();
    const T = new TextureManager(device);
    const P = new TAParser();

    const file = readFileSync(capPath);
    const msgs = zcstMessages(file);
    console.log(`[mirror] ${msgs.length} ZCST messages`);
    const frames = [];
    for (const m of msgs) {
        let fr = null;
        try { fr = D.applyFrame(m); } catch (e) { console.warn('  applyFrame skip:', e.message); continue; }
        if (fr) frames.push(fr);
    }
    if (!frames.length) throw new Error('no renderable TA frame');
    const fr = frameIdx < 0 ? frames[frames.length - 1] : frames[Math.min(frameIdx, frames.length - 1)];
    console.log(`[mirror] ${frames.length} TA frame(s); rendering #${fr.frameNum} (idx ${frameIdx})`);

    T.setDirtyPages(fr.dirtyPageList, fr.pvrDirty);
    T.updatePalette(D.pvrRegs);
    const parsed = P.parse(fr.taBuffer, fr.taSize);
    try { P.fillBGP(parsed, D.pvrRegs, D.vram); } catch (e) { console.warn('[fillBGP]', e.message); }
    console.log(`[parse] ${parsed.vertexCount} verts | op=${parsed.opaque.length} pt=${parsed.punchThrough.length} tr=${parsed.translucent.length}`);

    R.renderFrame(parsed, T, fr.pvrSnapshot, D.vram, {}, rt);
    device.queue.submit([R._lastEncoder.finish()]);
    const rgba = await readback(device, rt.color, W, H, R.fmt);
    writePNG(outPath, rgba, W, H);
    let nz = 0; for (let i = 0; i < rgba.length; i += 4) if (rgba[i] | rgba[i+1] | rgba[i+2]) nz++;
    console.log(`[done] wrote ${outPath}; ${nz}/${W*H} non-black`);
}
main().catch(e => { console.error(e); process.exit(1); });
