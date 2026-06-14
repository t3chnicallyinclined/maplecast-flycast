// shot_hud_before.mjs — capture the OLD synthetic HUD (the disabled vector-digit renderer)
// for the before/after comparison. Drives the SAME grounded state, calls the retained
// _renderHudPassSynthDISABLED into the legacy overlay, and snapshots it.
import puppeteer from 'puppeteer';
import { createServer } from 'http';
import { readFile, writeFile } from 'fs/promises';
import { extname, join, normalize } from 'path';

const ROOT = process.cwd(), PORT = 8132;
const MIME = { '.html':'text/html','.mjs':'text/javascript','.js':'text/javascript','.json':'application/json',
  '.png':'image/png','.wasm':'application/wasm','.bin':'application/octet-stream','.mcrr':'application/octet-stream' };
const server = createServer(async (req, res) => { try {
  const p = normalize(decodeURIComponent(req.url.split('?')[0])).replace(/^(\.\.[/\\])+/, '');
  const data = await readFile(join(ROOT, p));
  res.writeHead(200, { 'Content-Type': MIME[extname(p)] || 'application/octet-stream', 'Access-Control-Allow-Origin':'*' });
  res.end(data);
} catch { res.writeHead(404); res.end('nf'); } });
await new Promise(r => server.listen(PORT, r));
const BASE = `http://localhost:${PORT}`;
const REC = `${BASE}/tools/render-replica-poc/maxq_86.mcrr`;
const OUT = process.argv[2] || 'PNG_hud_before.png';
const PAGE = `${BASE}/web/render-replica/replay.html?rec=${encodeURIComponent(REC)}`;
const browser = await puppeteer.launch({ headless:'new',
  args:['--enable-unsafe-webgpu','--ignore-gpu-blocklist','--no-sandbox','--use-angle=vulkan','--use-gl=angle'],
  executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe' });
const page = await browser.newPage();
await page.setViewport({ width:900, height:760, deviceScaleFactor:1 });
await page.goto(PAGE, { waitUntil:'networkidle0', timeout:90000 });
await page.waitForFunction(() => window.__state && window.__state().nFrames > 0, { timeout:90000 });
await page.evaluate(f => window.__showFrame(f), 0);
const A = { IN_MATCH:0x8C289624, ROUND:0x8C28962B, TIMER:0x8C289630, P1_FILL:0x8C289646, P2_FILL:0x8C289648,
  P1_LVL:0x8C28964A, P2_LVL:0x8C28964B, P1_COMBO:0x8C289670, P2_COMBO:0x8C289672,
  SLOTS:[0x8C268340,0x8C2688E4,0x8C268E88,0x8C26942C,0x8C2699D0,0x8C269F74] };
const dataUrl = await page.evaluate((A) => {
  const p8 = window.__hudPoke8, p16 = window.__hudPoke16;
  p8(A.IN_MATCH,1); p8(A.ROUND,2); p8(A.TIMER,73);
  p8(A.SLOTS[0]+0,1); p8(A.SLOTS[0]+0x001,0); p8(A.SLOTS[0]+0x420,72); p8(A.SLOTS[0]+0x424,110);
  p8(A.SLOTS[1]+0,1); p8(A.SLOTS[1]+0x001,23); p8(A.SLOTS[1]+0x420,130); p8(A.SLOTS[1]+0x424,130);
  p16(A.P1_FILL,144); p16(A.P2_FILL,72); p8(A.P1_LVL,3); p8(A.P2_LVL,1); p16(A.P1_COMBO,7); p16(A.P2_COMBO,0);
  // call the retained synthetic renderer into the legacy overlay
  window._renderHudPassSynthDISABLED ? window._renderHudPassSynthDISABLED() : 0;
  const c = window.__legacyHudCanvas ? window.__legacyHudCanvas() : null;
  if (!c) return null;
  const o = document.createElement('canvas'); o.width=c.width; o.height=c.height;
  const x = o.getContext('2d'); x.fillStyle='#101418'; x.fillRect(0,0,o.width,o.height); x.drawImage(c,0,0);
  return o.toDataURL('image/png');
}, A);
if (dataUrl) { await writeFile(OUT, Buffer.from(dataUrl.split(',')[1],'base64')); console.log('wrote', OUT); }
else console.log('legacy canvas unavailable');
await browser.close(); server.close(); process.exit(0);
