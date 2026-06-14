// shot_hud_realart.mjs — validate the REAL-ART HUD (hud-client.mjs) in a real headless
// browser. Loads replay.html, seeds a real MCRR, drives grounded synthetic HUD bytes into
// the RAM image at the engine addresses, forces the HUD pass, and reads back the overlay.
// Captures the HUD overlay alone (transparent->black) so the real FONT.BIN digit sprites
// + the team-tinted angled bars are verifiable independent of the WebGPU body readback.
//
//   node tools/render-replica-poc/shot_hud_realart.mjs <base_url> <rec_url> <out.png>
import puppeteer from 'puppeteer';
import { createServer } from 'http';
import { readFile } from 'fs/promises';
import { extname, join, normalize } from 'path';

const ROOT = process.cwd();
const PORT = 8131;
const MIME = { '.html':'text/html', '.mjs':'text/javascript', '.js':'text/javascript',
  '.json':'application/json', '.png':'image/png', '.wasm':'application/wasm',
  '.bin':'application/octet-stream', '.mcrr':'application/octet-stream' };

const server = createServer(async (req, res) => {
  try {
    const p = normalize(decodeURIComponent(req.url.split('?')[0])).replace(/^(\.\.[/\\])+/, '');
    const fp = join(ROOT, p);
    const data = await readFile(fp);
    res.writeHead(200, { 'Content-Type': MIME[extname(fp)] || 'application/octet-stream',
      'Access-Control-Allow-Origin': '*' });
    res.end(data);
  } catch { res.writeHead(404); res.end('nf'); }
});
await new Promise(r => server.listen(PORT, r));
const BASE = `http://localhost:${PORT}`;
const REC = process.argv[3] || `${BASE}/tools/render-replica-poc/maxq_86.mcrr`;
const OUT = process.argv[4] || 'PNG_hud_realart.png';
const PAGE = `${BASE}/web/render-replica/replay.html?rec=${encodeURIComponent(REC)}`;

const args = ['--enable-unsafe-webgpu','--enable-webgpu-developer-features','--ignore-gpu-blocklist',
  '--no-sandbox','--enable-features=Vulkan','--use-angle=vulkan','--use-gl=angle'];
const browser = await puppeteer.launch({ headless: 'new', args,
  executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe' });
const page = await browser.newPage();
await page.setViewport({ width: 900, height: 760, deviceScaleFactor: 1 });
page.on('console', m => { const t = m.text(); if (/hud|error|fail/i.test(t)) console.log('[page]', t); });
page.on('pageerror', e => console.log('[pageerror]', e.message));

await page.goto(PAGE, { waitUntil: 'networkidle0', timeout: 90000 });
await page.waitForFunction(() => window.__state && window.__state().nFrames > 0, { timeout: 90000 });
await page.evaluate(f => window.__showFrame(f), 0);

// wait for the real-art atlas to load
await page.waitForFunction(() => window.__hudReady && window.__hudReady(), { timeout: 30000 })
  .then(() => console.log('atlas: real-art HUD atlas LOADED'))
  .catch(() => console.log('atlas: NOT loaded in time (vector fallback)'));

const live0 = await page.evaluate(() => window.__readHud());
console.log('Step 1 — engine fields from seeded RAM (frame 0): inMatch=%d timer=%d round=%d p1=%s p2=%s',
  live0.inMatch, live0.timer, live0.round, !!live0.p1, !!live0.p2);

// Drive grounded synthetic state at the REAL engine addresses, then re-render the HUD pass.
const A = { IN_MATCH:0x8C289624, ROUND:0x8C28962B, TIMER:0x8C289630, P1_FILL:0x8C289646,
  P2_FILL:0x8C289648, P1_LVL:0x8C28964A, P2_LVL:0x8C28964B, P1_COMBO:0x8C289670, P2_COMBO:0x8C289672,
  SLOTS:[0x8C268340,0x8C2688E4,0x8C268E88,0x8C26942C,0x8C2699D0,0x8C269F74] };
const result = await page.evaluate((A) => {
  const p8 = window.__hudPoke8, p16 = window.__hudPoke16;
  p8(A.IN_MATCH, 1); p8(A.ROUND, 2); p8(A.TIMER, 73);
  // P1 point = slot0 (C1, magenta): HP 72/144, chip to 110/144; cid 0 (Ryu)
  p8(A.SLOTS[0] + 0x000, 1); p8(A.SLOTS[0] + 0x001, 0);
  p8(A.SLOTS[0] + 0x420, 72); p8(A.SLOTS[0] + 0x424, 110);
  // P2 point = slot1 (C1, magenta): HP 130/144, no chip; cid 23 (Cable)
  p8(A.SLOTS[1] + 0x000, 1); p8(A.SLOTS[1] + 0x001, 23);
  p8(A.SLOTS[1] + 0x420, 130); p8(A.SLOTS[1] + 0x424, 130);
  p16(A.P1_FILL, 144); p16(A.P2_FILL, 72); p8(A.P1_LVL, 3); p8(A.P2_LVL, 1);
  p16(A.P1_COMBO, 7); p16(A.P2_COMBO, 0);
  window.__renderHud();
  const c = window.__hudCanvas();
  // count non-blank px + sample the timer center + the P1 bar
  const ctx = c.getContext('2d'); const W = c.width, H = c.height;
  const d = ctx.getImageData(0, 0, W, H).data; let lit = 0;
  for (let i = 3; i < d.length; i += 4) if (d[i] > 8) lit++;
  return { W, H, lit, ready: window.__hudReady && window.__hudReady() };
}, A);
console.log('Step 2 — HUD overlay: %dx%d, %d lit px, atlasReady=%s', result.W, result.H, result.lit, result.ready);

// capture the overlay canvas alone (on a black backing so digits/bars are visible)
const dataUrl = await page.evaluate(() => {
  const c = window.__hudCanvas();
  const o = document.createElement('canvas'); o.width = c.width; o.height = c.height;
  const x = o.getContext('2d'); x.fillStyle = '#101418'; x.fillRect(0, 0, o.width, o.height);
  x.drawImage(c, 0, 0); return o.toDataURL('image/png');
});
const b64 = dataUrl.split(',')[1];
await (await import('fs/promises')).writeFile(OUT, Buffer.from(b64, 'base64'));
console.log('wrote', OUT);

await browser.close();
server.close();
process.exit(result.lit > 1000 ? 0 : 1);
