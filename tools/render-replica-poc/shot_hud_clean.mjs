// shot_hud_clean.mjs — VISUAL HUD verification. Drive replay.html headless on a recording,
// force a known in-match HUD state at the real engine addresses, run the HUD pass, and
// SCREENSHOT THE COMPOSITED CANVAS REGION (canvas + body + HUD overlay) so we can LOOK at
// whether the HUD is clean (not just geometry-assert). Crops tight to the 640x480 stack.
//
//   node tools/render-replica-poc/shot_hud_clean.mjs [base] [rec] [out.png]
import puppeteer from 'puppeteer';

const BASE = process.argv[2] || 'http://localhost:8099';
const REC  = process.argv[3] || (BASE + '/tools/render-replica-poc/maxq_86.mcrr');
const OUT  = process.argv[4] || 'PNG_hud_clean.png';
const PAGE = `${BASE}/web/render-replica/replay.html?rec=${encodeURIComponent(REC)}`;

const args = ['--enable-unsafe-webgpu','--enable-webgpu-developer-features','--ignore-gpu-blocklist',
  '--no-sandbox','--enable-features=Vulkan','--use-angle=vulkan','--use-gl=angle'];
const browser = await puppeteer.launch({ headless: 'new', args });
const page = await browser.newPage();
await page.setViewport({ width: 900, height: 800, deviceScaleFactor: 2 });
const logs = [];
page.on('console', m => logs.push('[page] ' + m.text()));
page.on('pageerror', e => logs.push('[pageerror] ' + e.message));

await page.goto(PAGE, { waitUntil: 'networkidle0', timeout: 90000 });
await page.waitForFunction(() => window.__state && window.__state().nFrames > 0, { timeout: 90000 });
await page.evaluate(f => window.__showFrame(f), 0);
// give async atlas/hud loads a beat
await new Promise(r => setTimeout(r, 1200));

const ADDR = {
  IN_MATCH: 0x8C289624, ROUND: 0x8C28962B, TIMER: 0x8C289630,
  P1_FILL: 0x8C289646, P2_FILL: 0x8C289648, P1_LVL: 0x8C28964A, P2_LVL: 0x8C28964B,
  P1_COMBO: 0x8C289670, P2_COMBO: 0x8C289672,
  CHAR_SLOTS: [0x8C268340, 0x8C2688E4, 0x8C268E88, 0x8C26942C, 0x8C2699D0, 0x8C269F74],
};

// Force a rich, known HUD state so every element is exercised, then run the HUD pass.
await page.evaluate((A) => {
  const s8 = (a, v) => window.__hudPoke8(a, v), s16 = (a, v) => window.__hudPoke16(a, v);
  s8(A.IN_MATCH, 1);
  // P1 point char: char_id 1, HP 90/144 with chip to 124 (recoverable trail visible)
  s8(A.CHAR_SLOTS[0] + 0x000, 1); s8(A.CHAR_SLOTS[0] + 0x001, 1);
  s8(A.CHAR_SLOTS[0] + 0x420, 90); s8(A.CHAR_SLOTS[0] + 0x424, 124);
  // P2 point char: char_id 18, HP 132/144
  s8(A.CHAR_SLOTS[1] + 0x000, 1); s8(A.CHAR_SLOTS[1] + 0x001, 18);
  s8(A.CHAR_SLOTS[1] + 0x420, 132); s8(A.CHAR_SLOTS[1] + 0x424, 132);
  s8(A.TIMER, 73); s8(A.ROUND, 2);
  s16(A.P1_FILL, 144); s16(A.P2_FILL, 78);
  s8(A.P1_LVL, 3); s8(A.P2_LVL, 1);
  s16(A.P1_COMBO, 7); s16(A.P2_COMBO, 0);
  window.__renderHud();
}, ADDR);

await new Promise(r => setTimeout(r, 300));

// Screenshot tight to the canvas stack (the body canvas + HUD overlay live over #cReplay).
const rect = await page.evaluate(() => {
  const c = document.getElementById('cReplay'); const r = c.getBoundingClientRect();
  return { x: r.x, y: r.y, w: r.width, h: r.height };
});
await page.screenshot({ path: OUT, clip: { x: rect.x, y: rect.y, width: rect.w, height: rect.h } });
console.log('canvas-region screenshot:', OUT, JSON.stringify(rect));
console.log(logs.slice(-15).join('\n'));
await browser.close();
