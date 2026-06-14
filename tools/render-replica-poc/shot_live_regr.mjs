// shot_live_regr.mjs — drive replay.html in TRUE LIVE mode (prod WS) headlessly, let it run the
// exact prod draw path (render_frame TA -> ensureBodyTextures verbatim -> cockpitProcess ->
// pane.render), then re-draw window.__lastDrawTA into an offscreen RT + copyTextureToBuffer
// readback for a PNG. Reproduces the live-match regression the .mcrr file path can't (the file
// parser doesn't read the live palette tail). Also dumps per-quad sel/gfx1/TCW for diagnosis.
//   node shot_live_regr.mjs <base> <liveUrl> <settleFrames> <out.png>
import puppeteer from 'puppeteer';
import fs from 'fs';
import zlib from 'zlib';

const BASE = process.argv[2] || 'http://localhost:8099';
const LIVE = process.argv[3] || 'wss://nobd.net/replica-live';
const SETTLE = parseInt(process.argv[4] || '120', 10);
const OUT = process.argv[5] || 'PNG_live_regr.png';

const args = ['--enable-unsafe-webgpu','--enable-webgpu-developer-features','--ignore-gpu-blocklist',
              '--no-sandbox','--use-webgpu-adapter=swiftshader','--enable-features=Vulkan'];
const browser = await puppeteer.launch({ headless:'new', args });
const page = await browser.newPage();
const logs = [];
page.on('console', m => logs.push('[page] ' + m.text()));
page.on('pageerror', e => logs.push('[ERR] ' + e.message));

const PAGE = `${BASE}/web/render-replica/replay.html?live=${encodeURIComponent(LIVE)}`;
console.log('loading', PAGE);
try {
  await page.goto(PAGE, { waitUntil:'domcontentloaded', timeout:120000 });
  await page.waitForFunction(() => window.__pane && window.__pane.R && window.__pane.R.dev, { timeout:60000, polling:500 });
  console.log('GPU device up');
  // wait until live is seeded AND has drawn at least one frame with quads
  await page.waitForFunction(() => window.__liveState && window.__liveState().seeded && window.__lastDrawTA, { timeout:120000, polling:500 });
  console.log('live seeded + drawing');
} catch (e) {
  console.log('SETUP TIMEOUT:', e.message);
  console.log(logs.slice(-30).join('\n'));
  await browser.close(); process.exit(2);
}

// let it run a while so a stable mid-match pose is on screen
await new Promise(r => setTimeout(r, SETTLE * 16));

const res = await page.evaluate(async () => {
    const { TAParser } = await import(new URL('../webgpu/ta-parser.mjs', location.href));
    const pane = window.__pane, R = pane.R, dev = R.dev, W = 640, H = 480;
    const drawTA = window.__lastDrawTA;
    if (!drawTA) return { err: 'no __lastDrawTA' };

    // per-quad diagnostics from the LAST live frame
    const ta = window.__lastTA;
    const M = window.__frameMod;  // may be undefined; fall back to parse
    const diag = window.__lastQuadDiag || null;

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

    const rgba = new Uint8Array(W*H*4);
    let nPix = 0; const distinct = new Set();
    let minx=W, maxx=0, miny=H, maxy=0;
    for (let y=0;y<H;y++) for (let x=0;x<W;x++){
        const s=y*bpr+x*4, o=(y*W+x)*4;
        const b=d[s], g=d[s+1], r=d[s+2];
        rgba[o]=r; rgba[o+1]=g; rgba[o+2]=b; rgba[o+3]=255;
        if (r|g|b) { nPix++; if (x<minx)minx=x; if (x>maxx)maxx=x; if (y<miny)miny=y; if (y>maxy)maxy=y;
            distinct.add(((r>>3)<<10)|((g>>3)<<5)|(b>>3)); }
    }
    return { nPix, distinct: distinct.size, bbox:[minx,miny,maxx,maxy],
             op:parsed.opaque.length, pt:parsed.punchThrough.length, tr:parsed.translucent.length,
             vframe: window.__liveState().lastFrame, rgba:Array.from(rgba), W, H };
});

if (res.err) { console.log('FAIL:', res.err); console.log(logs.slice(-15).join('\n')); await browser.close(); process.exit(1); }

function writePNG(path, w, h, rgba) {
    const raw = Buffer.alloc((w*4+1)*h);
    for (let y=0;y<h;y++){ raw[y*(w*4+1)]=0; Buffer.from(rgba.slice(y*w*4,(y+1)*w*4)).copy(raw, y*(w*4+1)+1); }
    const idat = zlib.deflateSync(raw, { level:6 });
    const chunk = (type, data) => { const len=Buffer.alloc(4); len.writeUInt32BE(data.length,0);
        const td=Buffer.concat([Buffer.from(type),data]); const crc=Buffer.alloc(4); crc.writeUInt32BE(crc32(td)>>>0,0);
        return Buffer.concat([len,td,crc]); };
    const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w,0); ihdr.writeUInt32BE(h,4); ihdr[8]=8; ihdr[9]=6;
    fs.writeFileSync(path, Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]), chunk('IHDR',ihdr), chunk('IDAT',idat), chunk('IEND',Buffer.alloc(0))]));
}
let CRC; function crc32(buf){ if(!CRC){CRC=new Uint32Array(256);for(let n=0;n<256;n++){let c=n;for(let k=0;k<8;k++)c=c&1?0xEDB88320^(c>>>1):c>>>1;CRC[n]=c>>>0;}} let c=0xFFFFFFFF;for(let i=0;i<buf.length;i++)c=CRC[(c^buf[i])&0xFF]^(c>>>8);return (c^0xFFFFFFFF)>>>0; }

writePNG(OUT, res.W, res.H, res.rgba);
console.log(logs.filter(l=>/ERR|local-gfx|scramble|fail|decode|live\] frame/i.test(l)).slice(-15).join('\n'));
console.log(`\nlive vframe=${res.vframe}: op=${res.op} pt=${res.pt} tr=${res.tr}`);
console.log(`textured pixels=${res.nPix} distinct=${res.distinct} bbox=[${res.bbox.join(', ')}]`);
console.log(`screenshot: ${OUT}`);
await browser.close();
process.exit(0);
