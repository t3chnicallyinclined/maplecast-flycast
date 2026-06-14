// _probe_effects_draw.mjs — in the live page, poke an effect and introspect EXACTLY what
// EffectsClient.render does: image loaded? rect found? pixels after drawImage?
import puppeteer from 'puppeteer';
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import { extname, join, normalize } from 'node:path';

const ROOT  = normalize(new URL('../../', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const HTTP_PORT = 8138, WS_PORT = 7222;
const REC = join(ROOT, '_satlive.mcrr');
const MIME = { '.html':'text/html', '.mjs':'text/javascript', '.js':'text/javascript', '.wasm':'application/wasm', '.bin':'application/octet-stream', '.json':'application/json', '.png':'image/png', '.css':'text/css' };
const httpd = createServer(async (req, res) => { try { const url = decodeURIComponent(req.url.split('?')[0]); const body = await readFile(join(ROOT, url)); res.writeHead(200, { 'content-type': MIME[extname(url)] || 'application/octet-stream' }); res.end(body); } catch { res.writeHead(404); res.end('404'); } });
await new Promise(r => httpd.listen(HTTP_PORT, r));
const mock = spawn(process.execPath, [join(ROOT, 'tools', 'render-replica-poc', 'mock_replica_live_server.mjs'), REC, String(WS_PORT), '60'], { stdio: 'ignore' });
await new Promise(r => setTimeout(r, 1500));
const browser = await puppeteer.launch({ headless: 'new', args: ['--no-sandbox'] });
const page = await browser.newPage();
await page.setViewport({ width: 900, height: 820 });
const wsUrl = `ws://127.0.0.1:${WS_PORT}`;
await page.goto(`http://127.0.0.1:${HTTP_PORT}/web/render-replica/replay.html?live=${encodeURIComponent(wsUrl)}`, { waitUntil: 'networkidle0', timeout: 90000 });
await page.waitForFunction(() => window.__liveState && window.__liveState().seeded, { timeout: 90000 });
await new Promise(r => setTimeout(r, 2500));

const diag = await page.evaluate(() => {
    // reach the effects object via the render hook's closure isn't possible; re-import not needed —
    // poke then render, then dump the overlay + the effects internals via __effectsOverlay + a manual draw.
    const poke = window.__pokeEffect(0, 320, 240);
    const nodes = window.__effectNodes();
    const drew = window.__renderEffects();
    const c = window.__effectsOverlay();
    let overlayNz = 0, ow = 0, oh = 0;
    if (c) { ow = c.width; oh = c.height; const ctx = c.getContext('2d'); const d = ctx.getImageData(0,0,ow,oh).data; for (let i=0;i<d.length;i+=4) if (d[i+3]) overlayNz++; }
    return { poke, nodeCount: nodes.length, nodes: nodes.slice(0,3), drew, ow, oh, overlayNz };
});
console.log(JSON.stringify(diag, null, 2));
await browser.close(); httpd.close(); mock.kill();
