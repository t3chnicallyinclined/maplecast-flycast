// shot_replica_live.mjs — render the RIGHT (render-replica) pane LIVE from prod.
// Drives replay.html?live=<wss> in headless Chrome (WebGPU). Once N live frames have
// flowed, composites the WebGPU body canvas + the 2D HUD overlay into one 640x480 PNG.
//   node shot_replica_live.mjs <base_url> <wss_url> <out.png> [warmupFrames=80]
import puppeteer from 'puppeteer';
import { writeFileSync } from 'node:fs';

const BASE  = process.argv[2];
const WSS   = process.argv[3] || 'wss://nobd.net/replica-live';
const OUT   = process.argv[4] || 'right_live.png';
const WARM  = +(process.argv[5] || 80);
const PAGE  = `${BASE}/web/render-replica/replay.html?live=${encodeURIComponent(WSS)}`;

const args = ['--enable-unsafe-webgpu','--enable-webgpu-developer-features','--ignore-gpu-blocklist','--no-sandbox',
              '--enable-features=Vulkan','--use-angle=vulkan','--use-gl=angle'];
const browser = await puppeteer.launch({ headless: 'new', args });
const page = await browser.newPage();
await page.setViewport({ width: 1100, height: 900, deviceScaleFactor: 1 });
const logs = [];
page.on('console', m => logs.push('[page] ' + m.text()));
page.on('pageerror', e => logs.push('[pageerror] ' + e.message));

await page.goto(PAGE, { waitUntil: 'domcontentloaded', timeout: 90000 });

// Wait until the live link is up AND at least WARM frames have been applied (vframe moving).
try {
    await page.waitForFunction((warm) => {
        const el = document.getElementById('liveframe');
        const lf = document.getElementById('livefps');
        if (!el || el.textContent === '–') return false;
        window.__seenFrames = (window.__seenFrames || 0);
        const v = parseInt(el.textContent, 10);
        if (window.__lastV !== v) { window.__lastV = v; window.__seenFrames++; }
        return window.__seenFrames >= warm;
    }, { timeout: 90000, polling: 100 }, WARM);
} catch (e) {
    console.error('[timeout] live did not flow. logs:\n' + logs.slice(-30).join('\n'));
    await browser.close(); process.exit(1);
}

// give a beat for the latest HUD overlay paint, then composite
const dataUrl = await page.evaluate(async () => {
    await new Promise(r => setTimeout(r, 200));
    const body = document.getElementById('cReplay');
    const sibs = [...body.parentElement.querySelectorAll('canvas')];
    const out = document.createElement('canvas'); out.width = 640; out.height = 480;
    const g = out.getContext('2d'); g.imageSmoothingEnabled = false;
    g.drawImage(body, 0, 0, 640, 480);
    for (const c of sibs) { if (c === body || c.style.display === 'none') continue; try { g.drawImage(c, 0, 0, 640, 480); } catch (e) {} }
    return out.toDataURL('image/png');
});
const vframe = await page.evaluate(() => document.getElementById('liveframe').textContent);

writeFileSync(OUT, Buffer.from(dataUrl.split(',')[1], 'base64'));
console.log(logs.filter(l => l.includes('[live]') || l.includes('error') || l.includes('hud')).slice(-12).join('\n'));
console.log(`[done] wrote ${OUT} (live vframe ${vframe})`);
await browser.close();
