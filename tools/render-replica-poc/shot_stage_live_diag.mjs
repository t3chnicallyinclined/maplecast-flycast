// shot_stage_live_diag.mjs — REPRODUCE the live black-stage and CAPTURE the swallowed
// [stage] error. Drives the ACTUAL replay.html LIVE path with _satlive.mcrr over the mock
// replica-live WS, waits for the stage atlas to load, then reports __stageInfo, the
// __renderStage() return, every [stage]/[bodies] console line, and #cReplay pixel count.
//
//   node shot_stage_live_diag.mjs
import puppeteer from 'puppeteer';
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import { extname, join, normalize } from 'node:path';

const ROOT  = normalize(new URL('../../', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const HTTP_PORT = 8139, WS_PORT = 7223;
const REC = join(ROOT, '_satlive.mcrr');
const MIME = { '.html':'text/html', '.mjs':'text/javascript', '.js':'text/javascript', '.wasm':'application/wasm', '.bin':'application/octet-stream', '.json':'application/json', '.png':'image/png', '.css':'text/css' };

const httpd = createServer(async (req, res) => {
    try {
        const url = decodeURIComponent(req.url.split('?')[0]);
        const body = await readFile(join(ROOT, url));
        res.writeHead(200, { 'content-type': MIME[extname(url)] || 'application/octet-stream' });
        res.end(body);
    } catch { res.writeHead(404); res.end('404'); }
});
await new Promise(r => httpd.listen(HTTP_PORT, r));
console.log(`[shot] static http://127.0.0.1:${HTTP_PORT}`);

const mock = spawn(process.execPath, [join(ROOT, 'tools', 'render-replica-poc', 'mock_replica_live_server.mjs'), REC, String(WS_PORT), '60'], { stdio: 'ignore' });
await new Promise(r => setTimeout(r, 1500));

const args = ['--enable-unsafe-webgpu','--enable-webgpu-developer-features','--ignore-gpu-blocklist','--no-sandbox','--enable-features=Vulkan','--use-angle=vulkan','--use-gl=angle'];
const browser = await puppeteer.launch({ headless: 'new', args });
const page = await browser.newPage();
await page.setViewport({ width: 900, height: 820, deviceScaleFactor: 1 });
const logs = [];
page.on('console', m => { const t = m.text(); logs.push(t); if (/\[stage|\[bodies|stage-client|webgpu|adapter|GPUValidation|GPU/i.test(t)) console.log('  PAGE> ' + t); });
page.on('pageerror', e => console.log('  PAGEERR> ' + e.message));

const wsUrl = `ws://127.0.0.1:${WS_PORT}`;
const PAGE = `http://127.0.0.1:${HTTP_PORT}/web/render-replica/replay.html?live=${encodeURIComponent(wsUrl)}`;
await page.goto(PAGE, { waitUntil: 'networkidle0', timeout: 90000 });
await page.waitForFunction(() => window.__liveState && window.__liveState().seeded, { timeout: 90000 });
console.log('[shot] seeded');

// wait up to 12s for the stage atlas to load (loadedId becomes 11)
try {
  await page.waitForFunction(() => { const s = window.__stageInfo && window.__stageInfo(); return s && s.loadedId === 11 && s.vertexCount > 0; }, { timeout: 12000 });
  console.log('[shot] stage atlas loaded');
} catch { console.log('[shot] WARNING: stage atlas did NOT load within 12s'); }

await new Promise(r => setTimeout(r, 1500));

const info = await page.evaluate(() => window.__stageInfo && window.__stageInfo());
const ready = await page.evaluate(() => window.__stageReady && window.__stageReady());
console.log('[stageInfo] ' + JSON.stringify(info) + ' ready=' + ready);

// Call __renderStage() directly and grab any [stage] warn it logs right after.
logs.length = 0;
const drew = await page.evaluate(() => { try { return window.__renderStage(); } catch(e){ return 'THREW: ' + e.message; } });
await new Promise(r => setTimeout(r, 200));
const stageLines = logs.filter(l => /\[stage/.test(l));
console.log('[__renderStage] returned: ' + drew);
console.log('[stage warn lines] ' + (stageLines.length ? stageLines.join(' | ') : '(none)'));

// pixel count on the WebGPU canvas
const px = await page.evaluate(async () => {
    const cv = document.getElementById('cReplay');
    const tmp = document.createElement('canvas'); tmp.width = cv.width; tmp.height = cv.height;
    const ctx = tmp.getContext('2d'); ctx.drawImage(cv, 0, 0);
    const d = ctx.getImageData(0, 0, cv.width, cv.height).data;
    let nz = 0; for (let i = 0; i < d.length; i += 4) if (d[i] | d[i+1] | d[i+2]) nz++;
    return { w: cv.width, h: cv.height, nz, total: cv.width * cv.height };
});
console.log(`[#cReplay] ${px.w}x${px.h}: ${px.nz}/${px.total} non-black (${(100*px.nz/px.total).toFixed(1)}%)`);

await page.screenshot({ path: join(ROOT, '_stage_gt', 'PNG_live_stage_diag.png') });
console.log('screenshot: _stage_gt/PNG_live_stage_diag.png');

await browser.close(); httpd.close(); mock.kill();
process.exit(0);
