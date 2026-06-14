// shot_tagbars.mjs — VALIDATE the 3-bar tag-team life-bar HUD (hud-client.mjs
// renderTagLifeBars). Loads replay.html, seeds FULL 6-slot teams (3 chars per side,
// each with health/red_health/char_id/active at the real engine addresses), then
// captures the HUD overlay so the 3 stacked angled bars per side are visible.
//   node tools/render-replica-poc/shot_tagbars.mjs [out.png]
import puppeteer from 'puppeteer';
import { createServer } from 'http';
import { readFile, writeFile } from 'fs/promises';
import { extname, join, normalize } from 'path';

const ROOT = process.cwd();
const PORT = 8139;
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
const OUT = process.argv[2] || 'PNG_tagbars.png';
const REC = `${BASE}/tools/render-replica-poc/maxq_86.mcrr`;
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
await page.waitForFunction(() => window.__hudReady && window.__hudReady(), { timeout: 30000 })
  .then(() => console.log('atlas: HUD atlas LOADED'))
  .catch(() => console.log('atlas: NOT loaded (fallback)'));

const A = { IN_MATCH:0x8C289624, ROUND:0x8C28962B, TIMER:0x8C289630, P1_FILL:0x8C289646,
  P2_FILL:0x8C289648, P1_LVL:0x8C28964A, P2_LVL:0x8C28964B, P1_COMBO:0x8C289670, P2_COMBO:0x8C289672,
  SLOTS:[0x8C268340,0x8C2688E4,0x8C268E88,0x8C26942C,0x8C2699D0,0x8C269F74] };

const result = await page.evaluate((A) => {
  const p8 = window.__hudPoke8, p16 = window.__hudPoke16;
  p8(A.IN_MATCH, 1); p8(A.ROUND, 2); p8(A.TIMER, 99);
  // P1 team (slots 0,2,4): STORM(0x2A) active, SENTINEL(0x34), CABLE(0x17)
  const setSlot = (s, cid, active, hp, red) => { const b = A.SLOTS[s];
    p8(b+0x000, active); p8(b+0x001, cid); p8(b+0x420, hp); p8(b+0x424, red); };
  setSlot(0, 0x2A, 1, 144, 144);   // P1 C1 STORM active, full
  setSlot(2, 0x34, 0, 96,  120);   // P1 C2 SENTINEL reserve, chipped
  setSlot(4, 0x17, 0, 144, 144);   // P1 C3 CABLE reserve, full
  // P2 team (slots 1,3,5): CABLE active + two reserves
  setSlot(1, 0x17, 1, 130, 130);   // P2 C1 CABLE active
  setSlot(3, 0x17, 0, 0,   40);    // P2 C2 reserve KO'd (hp 0)
  setSlot(5, 0x17, 0, 70,  90);    // P2 C3 reserve chipped
  p16(A.P1_FILL, 144); p16(A.P2_FILL, 72); p8(A.P1_LVL, 3); p8(A.P2_LVL, 1);
  p16(A.P1_COMBO, 7); p16(A.P2_COMBO, 0);

  const litOf = (c) => { const x=c.getContext('2d'); const d=x.getImageData(0,0,c.width,c.height).data;
    let n=0; for (let i=3;i<d.length;i+=4) if (d[i]>8) n++; return n; };

  window.__renderHud();
  const ac = window.__hudCanvas();
  const ao=document.createElement('canvas'); ao.width=ac.width; ao.height=ac.height;
  const ax=ao.getContext('2d'); ax.fillStyle='#101418'; ax.fillRect(0,0,ao.width,ao.height); ax.drawImage(ac,0,0);
  return { W: ac.width, H: ac.height, lit: litOf(ac), url: ao.toDataURL('image/png'),
           ready: !!(window.__hudReady && window.__hudReady()) };
}, A);

console.log('tagbars HUD: %dx%d, %d lit px, atlasReady=%s', result.W, result.H, result.lit, result.ready);
await writeFile(OUT, Buffer.from(result.url.split(',')[1], 'base64'));
console.log('wrote', OUT);

await browser.close();
server.close();
process.exit(result.lit > 1000 ? 0 : 1);
