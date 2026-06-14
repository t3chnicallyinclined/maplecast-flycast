// shot_stage_effects_live.mjs — drive the ACTUAL replay.html LIVE path with a real prod
// capture (_satlive.mcrr) over the mock replica-live WS, then PROVE the Phase-3 STAGE pass
// renders behind the bodies AND a poked test effect draws. Probes the stage/effect hooks +
// screenshots the page (hardware GPU).
//
//   node shot_stage_effects_live.mjs
import puppeteer from 'puppeteer';
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import { extname, join, normalize } from 'node:path';

const ROOT  = normalize(new URL('../../', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const HTTP_PORT = 8137, WS_PORT = 7221;
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

const mock = spawn(process.execPath, [join(ROOT, 'tools', 'render-replica-poc', 'mock_replica_live_server.mjs'), REC, String(WS_PORT), '60'], { stdio: 'inherit' });
await new Promise(r => setTimeout(r, 1500));

const args = ['--enable-unsafe-webgpu','--enable-webgpu-developer-features','--ignore-gpu-blocklist','--no-sandbox','--enable-features=Vulkan','--use-angle=vulkan','--use-gl=angle'];
const browser = await puppeteer.launch({ headless: 'new', args });
const page = await browser.newPage();
await page.setViewport({ width: 900, height: 820, deviceScaleFactor: 1 });
const logs = [];
page.on('console', m => logs.push('[page] ' + m.text()));
page.on('pageerror', e => logs.push('[pageerror] ' + e.message));

const wsUrl = `ws://127.0.0.1:${WS_PORT}`;
const PAGE = `http://127.0.0.1:${HTTP_PORT}/web/render-replica/replay.html?live=${encodeURIComponent(wsUrl)}`;
await page.goto(PAGE, { waitUntil: 'networkidle0', timeout: 90000 });
await page.waitForFunction(() => window.__liveState && window.__liveState().seeded, { timeout: 90000 });
console.log('[shot] seeded from ZCST prefix');

// let several live frames stream so the stage atlas + effects atlas load
await new Promise(r => setTimeout(r, 3000));

const stageInfo = await page.evaluate(() => window.__stageInfo && window.__stageInfo());
const stageReady = await page.evaluate(() => window.__stageReady && window.__stageReady());
const stageDrew  = await page.evaluate(() => window.__renderStage && window.__renderStage());
console.log('[stage] ready=' + stageReady + ' drew=' + stageDrew + ' info=' + JSON.stringify(stageInfo));

// poke a test effect (dir idx 0 = the 128x128 hitspark) at screen center and render it.
// MUST be atomic — the mock streams frames at 60fps and each frame re-splats the slot
// table/objpool, overwriting a poke done in a separate round-trip. Do poke+render in ONE
// evaluate so no live frame lands between them.
const effReady = await page.evaluate(() => window.__effectsReady && window.__effectsReady());
const eff = await page.evaluate(() => {
    const poke = window.__pokeEffect(0, 320, 240);
    const drewCount = window.__renderEffects();
    return { poke, drewCount };
});
console.log('[effects] ready=' + effReady + ' poke=' + JSON.stringify(eff.poke) + ' drewCount=' + eff.drewCount);
const effCount = eff.drewCount;

// CANVAS READBACK: count non-black pixels on the WebGPU body/stage canvas (proves the stage
// composited, not just that renderStage returned true).
const px = await page.evaluate(async () => {
    const cv = document.getElementById('cReplay');
    // draw the WebGPU canvas into a 2D canvas to sample pixels
    const tmp = document.createElement('canvas'); tmp.width = cv.width; tmp.height = cv.height;
    const ctx = tmp.getContext('2d'); ctx.drawImage(cv, 0, 0);
    const d = ctx.getImageData(0, 0, cv.width, cv.height).data;
    let nz = 0; for (let i = 0; i < d.length; i += 4) if (d[i] | d[i+1] | d[i+2]) nz++;
    return { w: cv.width, h: cv.height, nz, total: cv.width * cv.height };
});
console.log(`[canvas] ${px.w}x${px.h}: ${px.nz}/${px.total} non-black (${(100*px.nz/px.total).toFixed(1)}%)`);

await page.screenshot({ path: join(ROOT, 'PNG_stage_effects_live.png') });
console.log(logs.slice(-25).join('\n'));

// readback the EFFECTS 2D overlay (no GPU needed) to prove the poked quad drew pixels.
const effPx = await page.evaluate(() => {
    const c = window.__effectsOverlay && window.__effectsOverlay();
    if (!c) return { nz: 0, total: 0 };
    const ctx = c.getContext('2d');
    const d = ctx.getImageData(0, 0, c.width, c.height).data;
    let nz = 0; for (let i = 0; i < d.length; i += 4) if (d[i] | d[i+1] | d[i+2] | d[i+3]) nz++;
    return { nz, total: c.width * c.height, w: c.width, h: c.height };
});
console.log(`[effects overlay] ${effPx.w}x${effPx.h}: ${effPx.nz}/${effPx.total} non-transparent px`);

const gpuAvailable = !logs.some(l => l.includes('No WebGPU adapter') || l.includes('No available adapters'));
const stageOK = !gpuAvailable ? 'SKIP (no WebGPU in this headless Chrome — see Dawn harness shot_stage_headless.mjs)'
    : (stageInfo && stageInfo.vertexCount > 0 && stageDrew ? 'RENDERS' : 'FAIL');
const effectsOK = effReady && effCount > 0 && effPx.nz > 100;
console.log('\nSTAGE: ' + stageOK);
console.log('EFFECTS: ' + (effectsOK ? 'poked effect DREW (' + effCount + ' quad, ' + effPx.nz + ' px)' : 'FAIL'));
console.log('screenshot: PNG_stage_effects_live.png');

await browser.close(); httpd.close(); mock.kill();
process.exit(effectsOK ? 0 : 1);
