// verify_rf_bodies.mjs — PROVE the DRAWN bodies are render_frame (the transpile) + verbatim decode.
//
// After the 2026-06-14 "transpile-draws" restore: replay.html draws character bodies via
// render_frame.wasm's TA (transpiledTA -> cockpitProcess -> ensureBodyTextures into pane.vram ->
// pane.render == pvr2-renderer). The SpriteClient emitter no longer draws. This harness drives the
// LIVE page headlessly, runs its real draw path for a mid-match frame (so ensureBodyTextures has
// populated pane.vram with the FAITHFUL VERBATIM decode), then RENDERS the page's actual post-
// cockpit drawTA (window.__lastDrawTA) into an OFFSCREEN texture and reads it back with
// copyTextureToBuffer (the only readback that works under software/ANGLE WebGPU). It writes a PNG
// and reports textured-pixel coverage + a coarse color histogram, so a clean, textured, multi-color
// body (NOT flat magenta / single-color garble) is verifiable.
//
//   node tools/render-replica-poc/verify_rf_bodies.mjs <base_url> <rec_url> [frame]
//   e.g. node tools/render-replica-poc/verify_rf_bodies.mjs http://localhost:8099 \
//          http://localhost:8099/tools/render-replica-poc/_fidcap.mcrr 80
import puppeteer from 'puppeteer';
import fs from 'fs';
import zlib from 'zlib';

const BASE  = process.argv[2] || 'http://localhost:8099';
const REC   = process.argv[3] || `${BASE}/tools/render-replica-poc/_fidcap.mcrr`;
const FRAME = parseInt(process.argv[4] || '80', 10);
const OUT   = process.argv[5] || 'PNG_rf_bodies.png';

const args = ['--enable-unsafe-webgpu','--enable-webgpu-developer-features','--ignore-gpu-blocklist',
              '--no-sandbox','--use-webgpu-adapter=swiftshader','--enable-features=Vulkan'];
const browser = await puppeteer.launch({ headless:'new', args });
const page = await browser.newPage();
const logs = [];
page.on('console', m => logs.push('[page] ' + m.text()));
page.on('pageerror', e => logs.push('[ERR] ' + e.message));

const PAGE = `${BASE}/web/render-replica/replay.html?rec=${encodeURIComponent(REC)}`;
console.log('loading', PAGE);
try {
  await page.goto(PAGE, { waitUntil:'domcontentloaded', timeout:120000 });
  // wait for the GPU device AND a parsed recording
  await page.waitForFunction(() => window.__pane && window.__pane.R && window.__pane.R.dev, { timeout:60000, polling:500 });
  console.log('GPU device up');
  await page.waitForFunction(() => window.__state && window.__state().nFrames > 0, { timeout:120000, polling:1000 });
  console.log('recording parsed:', await page.evaluate(()=>window.__state()));
} catch (e) {
  console.log('SETUP TIMEOUT:', e.message);
  console.log(logs.slice(-30).join('\n'));
  await browser.close(); process.exit(2);
}

const res = await page.evaluate(async (frame) => {
    const { TAParser } = await import(new URL('../webgpu/ta-parser.mjs', location.href));
    const pane = window.__pane, R = pane.R, dev = R.dev, W = 640, H = 480;

    // Run the page's REAL draw path for this frame: transpiledTA -> ensureBodyTextures (verbatim
    // decode into pane.vram) -> cockpitProcess -> sets window.__lastDrawTA. (pane.render to the
    // swap-chain also runs, but its texture is consumed on present, so we re-draw offscreen.)
    window.__showFrame(frame);
    const drawTA = window.__lastDrawTA;
    if (!drawTA) return { err: 'no __lastDrawTA' };

    // Offscreen RT + copyTextureToBuffer readback (works under software WebGPU; canvas drawImage
    // does not). Same pane.vram (now holds the verbatim-decoded body parts) + pane.T palette.
    const colorTex = dev.createTexture({ size:[W,H], format:R.fmt,
        usage:GPUTextureUsage.RENDER_ATTACHMENT|GPUTextureUsage.COPY_SRC });
    const depthTex = dev.createTexture({ size:[W,H], format:'depth32float',
        usage:GPUTextureUsage.RENDER_ATTACHMENT });
    const rt = { colorView:colorTex.createView(), depthView:depthTex.createView(), width:W, height:H };
    pane.T.setDirtyPages(null, true); pane.T.updatePalette(pane.pvr);
    const parsed = new TAParser().parse(drawTA, drawTA.length);
    const sn = new Uint32Array(16); sn[0] = (19 & 0x3F) | ((14 & 0x3F) << 16);
    R.renderFrame(parsed, pane.T, sn, pane.vram, {}, rt);

    const enc = R._lastEncoder, bpr = Math.ceil(W*4/256)*256;
    const rb = dev.createBuffer({ size:bpr*H, usage:GPUBufferUsage.COPY_DST|GPUBufferUsage.MAP_READ });
    enc.copyTextureToBuffer({ texture:colorTex }, { buffer:rb, bytesPerRow:bpr }, [W,H]);
    dev.queue.submit([enc.finish()]);
    await rb.mapAsync(GPUMapMode.READ);
    const d = new Uint8Array(rb.getMappedRange()).slice(); rb.unmap();

    // tight-pack BGRA->RGBA rows (drop the row padding) for the PNG, and gather stats.
    const rgba = new Uint8Array(W*H*4);
    let nPix = 0; const distinct = new Set(); let magenta = 0;
    let minx=W, maxx=0, miny=H, maxy=0;
    for (let y=0;y<H;y++) for (let x=0;x<W;x++){
        const s=y*bpr+x*4, o=(y*W+x)*4;
        const b=d[s], g=d[s+1], r=d[s+2], a=d[s+3];
        rgba[o]=r; rgba[o+1]=g; rgba[o+2]=b; rgba[o+3]=255;   // opaque PNG (ignore A for view)
        if (r|g|b) { nPix++;
            if (x<minx)minx=x; if (x>maxx)maxx=x; if (y<miny)miny=y; if (y>maxy)maxy=y;
            distinct.add(((r>>3)<<10)|((g>>3)<<5)|(b>>3));      // ~15-bit color bucket
            if (r>200 && b>200 && g<80) magenta++;             // flat-magenta == decode-failure tell
        }
    }
    // count the render_frame body quads actually parsed (textured sprite blocks)
    const qcount = (parsed.translucent.length + parsed.punchThrough.length + parsed.opaque.length);
    return { nPix, distinct: distinct.size, magenta, qcount,
             op:parsed.opaque.length, pt:parsed.punchThrough.length, tr:parsed.translucent.length,
             bbox:[minx,miny,maxx,maxy], rgba:Array.from(rgba), W, H };
}, FRAME);

if (res.err) { console.log('FAIL:', res.err); console.log(logs.slice(-15).join('\n')); await browser.close(); process.exit(1); }

// write a PNG (zlib-deflate, filter 0 per row)
function writePNG(path, w, h, rgba) {
    const raw = Buffer.alloc((w*4+1)*h);
    for (let y=0;y<h;y++){ raw[y*(w*4+1)]=0; Buffer.from(rgba.slice(y*w*4,(y+1)*w*4)).copy(raw, y*(w*4+1)+1); }
    const idat = zlib.deflateSync(raw, { level:6 });
    const chunk = (type, data) => {
        const len = Buffer.alloc(4); len.writeUInt32BE(data.length,0);
        const td = Buffer.concat([Buffer.from(type), data]);
        const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td)>>>0,0);
        return Buffer.concat([len, td, crc]);
    };
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(w,0); ihdr.writeUInt32BE(h,4); ihdr[8]=8; ihdr[9]=6;
    const sig = Buffer.from([137,80,78,71,13,10,26,10]);
    fs.writeFileSync(path, Buffer.concat([sig, chunk('IHDR',ihdr), chunk('IDAT',idat), chunk('IEND',Buffer.alloc(0))]));
}
let CRC; function crc32(buf){ if(!CRC){CRC=new Uint32Array(256);for(let n=0;n<256;n++){let c=n;for(let k=0;k<8;k++)c=c&1?0xEDB88320^(c>>>1):c>>>1;CRC[n]=c>>>0;}} let c=0xFFFFFFFF;for(let i=0;i<buf.length;i++)c=CRC[(c^buf[i])&0xFF]^(c>>>8);return (c^0xFFFFFFFF)>>>0; }

writePNG(OUT, res.W, res.H, res.rgba);

console.log(logs.filter(l=>/ERR|WGSL|local-gfx|scramble|fail|decode/i.test(l)).slice(-12).join('\n'));
console.log(`\nframe ${FRAME}: render_frame parsed op=${res.op} pt=${res.pt} tr=${res.tr} (quads=${res.qcount})`);
console.log(`textured pixels = ${res.nPix}  distinct colors = ${res.distinct}  bbox = [${res.bbox.join(', ')}]`);
console.log(`flat-magenta pixels (decode-failure tell) = ${res.magenta}`);
console.log(`screenshot: ${OUT}`);

// CLEAN gate: bodies present (pixels), genuinely textured (many distinct colors, not 1-2),
// and NOT a magenta garble. A textured MVC2 body has dozens+ of palette colors.
const clean = res.nPix > 2000 && res.distinct >= 16 && res.magenta < res.nPix * 0.5;
console.log(clean
    ? 'PASS: render_frame-drawn bodies render CLEAN + textured (multi-color, not garble).'
    : 'FAIL: bodies missing / single-color / magenta-garble — diagnose the pvr2 texture path.');
await browser.close();
process.exit(clean ? 0 : 1);
