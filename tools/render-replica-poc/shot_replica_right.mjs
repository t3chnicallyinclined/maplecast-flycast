// shot_replica_right.mjs — render the RIGHT (render-replica) pane from a captured MCRR.
// Drives replay.html?rec=<mcrr> in headless Chrome (WebGPU), scrubs to a frame, and
// composites the WebGPU body canvas + the 2D HUD overlay canvas into one 640x480 PNG.
//   node shot_replica_right.mjs <base_url> <rec_url> <out.png> [frame=last]
import puppeteer from 'puppeteer';
import { writeFileSync } from 'node:fs';

const BASE = process.argv[2];
const REC  = process.argv[3];
const OUT  = process.argv[4] || 'right.png';
const FRAME = process.argv[5] !== undefined ? +process.argv[5] : -1;
const PAGE = `${BASE}/web/render-replica/replay.html?rec=${encodeURIComponent(REC)}`;

const args = ['--enable-unsafe-webgpu','--enable-webgpu-developer-features','--ignore-gpu-blocklist','--no-sandbox',
              '--enable-features=Vulkan','--use-angle=vulkan','--use-gl=angle'];
const browser = await puppeteer.launch({ headless: 'new', args });
const page = await browser.newPage();
await page.setViewport({ width: 1100, height: 900, deviceScaleFactor: 1 });
const logs = [];
page.on('console', m => logs.push('[page] ' + m.text()));
page.on('pageerror', e => logs.push('[pageerror] ' + e.message));

await page.goto(PAGE, { waitUntil: 'networkidle0', timeout: 90000 });
try {
    await page.waitForFunction(() => window.__state && window.__state().nFrames > 0, { timeout: 60000 });
} catch (e) {
    console.error('[timeout waiting for nFrames] page logs:\n' + logs.slice(-40).join('\n'));
    const st = await page.evaluate(() => { try { return JSON.stringify(window.__state ? window.__state() : 'no __state'); } catch (x) { return 'eval err ' + x.message; } });
    console.error('__state =', st);
    await browser.close();
    process.exit(1);
}

const nf = await page.evaluate(() => window.__state().nFrames);
const frame = FRAME < 0 ? nf - 1 : Math.min(FRAME, nf - 1);

// Render the frame, then composite body-canvas + HUD overlay into a data URL we read back.
const dataUrl = await page.evaluate(async (fr) => {
    window.__showFrame(fr);
    await new Promise(r => setTimeout(r, 250));   // let WebGPU present + 2D overlay settle
    const body = document.getElementById('cReplay');     // WebGPU body+stage canvas
    // the HUD overlay + bounds overlays are sibling absolute-positioned canvases
    const sibs = [...body.parentElement.querySelectorAll('canvas')];
    const out = document.createElement('canvas'); out.width = 640; out.height = 480;
    const g = out.getContext('2d'); g.imageSmoothingEnabled = false;
    // draw the body canvas first, then every overlay canvas on top (in DOM order)
    g.drawImage(body, 0, 0, 640, 480);
    for (const c of sibs) {
        if (c === body) continue;
        if (c.style.display === 'none') continue;
        try { g.drawImage(c, 0, 0, 640, 480); } catch (e) {}
    }
    return out.toDataURL('image/png');
}, frame);

const b64 = dataUrl.split(',')[1];
writeFileSync(OUT, Buffer.from(b64, 'base64'));
console.log(logs.slice(-15).join('\n'));
console.log(`[done] wrote ${OUT} (frame ${frame}/${nf})`);
await browser.close();
