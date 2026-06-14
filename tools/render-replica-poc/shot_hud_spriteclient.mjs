// shot_hud_spriteclient.mjs — VALIDATE the HUD reuse swap: render-replica HUD now draws via
// the PROVEN cockpit path SPRITECLIENT.drawHUD (web/webgpu/sprite-client.mjs), replacing the
// orphaned HudClient reconstruction. Loads replay.html, seeds grounded synthetic HUD bytes at
// the real engine addresses, then captures BOTH:
//   - BEFORE: the orphaned HudClient.render() (magenta reconstruction)
//   - AFTER : SPRITECLIENT.drawHUD (the cockpit's real-art HUD)
// onto a common black backing, side by side, so the swap is visually verifiable.
//
//   node tools/render-replica-poc/shot_hud_spriteclient.mjs [rec_url] [out.png]
import puppeteer from 'puppeteer';
import { createServer } from 'http';
import { readFile, writeFile } from 'fs/promises';
import { extname, join, normalize } from 'path';

const ROOT = process.cwd();
const PORT = 8137;
const MIME = { '.html':'text/html', '.mjs':'text/javascript', '.js':'text/javascript',
  '.json':'application/json', '.png':'image/png', '.wasm':'application/wasm',
  '.bin':'application/octet-stream', '.mcrr':'application/octet-stream' };

const server = createServer(async (req, res) => {
  try {
    const p = normalize(decodeURIComponent(req.url.split('?')[0])).replace(/^(\.\.[/\\])+/, '');
    const data = await readFile(join(ROOT, p));
    res.writeHead(200, { 'Content-Type': MIME[extname(p)] || 'application/octet-stream',
      'Access-Control-Allow-Origin': '*' });
    res.end(data);
  } catch { res.writeHead(404); res.end('nf'); }
});
await new Promise(r => server.listen(PORT, r));
const BASE = `http://localhost:${PORT}`;
const REC = process.argv[2] || `${BASE}/tools/render-replica-poc/maxq_86.mcrr`;
const OUT = process.argv[3] || 'PNG_hud_spriteclient.png';
const PAGE = `${BASE}/web/render-replica/replay.html?rec=${encodeURIComponent(REC)}`;

const args = ['--enable-unsafe-webgpu','--enable-webgpu-developer-features','--ignore-gpu-blocklist',
  '--no-sandbox','--enable-features=Vulkan','--use-angle=vulkan','--use-gl=angle'];
const browser = await puppeteer.launch({ headless: 'new', args,
  executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe' });
const page = await browser.newPage();
await page.setViewport({ width: 900, height: 760, deviceScaleFactor: 1 });
page.on('console', m => { const t = m.text(); if (/hud|error|fail|atlas/i.test(t)) console.log('[page]', t); });
page.on('pageerror', e => console.log('[pageerror]', e.message));

await page.goto(PAGE, { waitUntil: 'networkidle0', timeout: 90000 });
await page.waitForFunction(() => window.__state && window.__state().nFrames > 0, { timeout: 90000 });
await page.evaluate(f => window.__showFrame(f), 0);

// wait for sprite-client's HUD atlas (loadHudAtlas) to land
await page.waitForFunction(() => window.__hudReady && window.__hudReady(), { timeout: 30000 })
  .then(() => console.log('atlas: sprite-client HUD atlas LOADED'))
  .catch(() => console.log('atlas: NOT loaded (drawHUD falls back to monospace digits)'));

// grounded synthetic state at the real engine addresses (same scenario the prior harness used)
const A = { IN_MATCH:0x8C289624, ROUND:0x8C28962B, TIMER:0x8C289630, P1_FILL:0x8C289646,
  P2_FILL:0x8C289648, P1_LVL:0x8C28964A, P2_LVL:0x8C28964B, P1_COMBO:0x8C289670, P2_COMBO:0x8C289672,
  SLOTS:[0x8C268340,0x8C2688E4,0x8C268E88,0x8C26942C,0x8C2699D0,0x8C269F74] };

const result = await page.evaluate((A) => {
  const p8 = window.__hudPoke8, p16 = window.__hudPoke16;
  p8(A.IN_MATCH, 1); p8(A.ROUND, 2); p8(A.TIMER, 73);
  p8(A.SLOTS[0] + 0x000, 1); p8(A.SLOTS[0] + 0x001, 0);   // P1 point slot0: Ryu, HP 72, chip 110
  p8(A.SLOTS[0] + 0x420, 72); p8(A.SLOTS[0] + 0x424, 110);
  p8(A.SLOTS[1] + 0x000, 1); p8(A.SLOTS[1] + 0x001, 23);  // P2 point slot1: Cable, HP 130
  p8(A.SLOTS[1] + 0x420, 130); p8(A.SLOTS[1] + 0x424, 130);
  p16(A.P1_FILL, 144); p16(A.P2_FILL, 72); p8(A.P1_LVL, 3); p8(A.P2_LVL, 1);
  p16(A.P1_COMBO, 7); p16(A.P2_COMBO, 0);

  const litOf = (c) => { const x=c.getContext('2d'); const d=x.getImageData(0,0,c.width,c.height).data;
    let n=0; for (let i=3;i<d.length;i+=4) if (d[i]>8) n++; return n; };

  // BEFORE: the orphaned HudClient reconstruction (drive it directly onto its own canvas).
  let beforeUrl=null, beforeLit=0;
  try {
    const legacy = window.__hudClientLegacy();
    legacy.attach(document.getElementById('cReplay'));
    legacy.render(window.__rdU8 || ((a)=>0), window.__rdU16 || ((a)=>0), A.SLOTS);
    // HudClient reads via passed rdU8/rdU16; reuse the page's by name if exposed, else its own.
    const lc = window.__hudCanvasLegacy ? window.__hudCanvasLegacy() : legacy._canvas;
    if (lc) { const o=document.createElement('canvas'); o.width=lc.width;o.height=lc.height;
      const x=o.getContext('2d'); x.fillStyle='#101418'; x.fillRect(0,0,o.width,o.height); x.drawImage(lc,0,0);
      beforeUrl=o.toDataURL('image/png'); beforeLit=litOf(lc); }
  } catch(e) { window.__beforeErr = e.message; }

  // AFTER: the new live path — SPRITECLIENT.drawHUD via renderHudPass().
  window.__renderHud();
  const ac = window.__hudCanvas();
  const ao=document.createElement('canvas'); ao.width=ac.width;ao.height=ac.height;
  const ax=ao.getContext('2d'); ax.fillStyle='#101418'; ax.fillRect(0,0,ao.width,ao.height); ax.drawImage(ac,0,0);
  return { W: ac.width, H: ac.height, afterLit: litOf(ac), beforeLit,
           afterUrl: ao.toDataURL('image/png'), beforeUrl,
           ready: !!(window.__hudReady && window.__hudReady()),
           beforeErr: window.__beforeErr || null };
}, A);

console.log('AFTER (SPRITECLIENT.drawHUD): %dx%d, %d lit px, atlasReady=%s',
  result.W, result.H, result.afterLit, result.ready);
console.log('BEFORE (HudClient): %d lit px%s', result.beforeLit, result.beforeErr ? ' (err: '+result.beforeErr+')' : '');

// write the AFTER overlay as the deliverable
await writeFile(OUT, Buffer.from(result.afterUrl.split(',')[1], 'base64'));
console.log('wrote', OUT);
if (result.beforeUrl) {
  const bOut = OUT.replace(/\.png$/, '_before.png');
  await writeFile(bOut, Buffer.from(result.beforeUrl.split(',')[1], 'base64'));
  console.log('wrote', bOut);
}

await browser.close();
server.close();
process.exit(result.afterLit > 1000 ? 0 : 1);
