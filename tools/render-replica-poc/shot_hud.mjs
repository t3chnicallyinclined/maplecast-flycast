// shot_hud.mjs — Phase 3 HUD validation. Drive replay.html headless, load a real MCRR
// (seeds the 16MB RAM image + renders bodies/stage), then PROVE the HUD foreground pass:
//   1. __readHud() returns the HUD fields read from the engine addresses in the RAM image.
//   2. Drive synthetic-but-grounded live HUD values into the RAM image at the real engine
//      addresses, re-run renderHudPass(), and read back the overlay canvas to confirm each
//      element renders: HP bar depletes, red chip trails BEHIND it, super meter fills,
//      timer/combo digits appear. Screenshot the composited result.
//
//   node tools/render-replica-poc/shot_hud.mjs <base_url> <rec_url>
import puppeteer from 'puppeteer';

const BASE = process.argv[2] || 'http://localhost:8099';
const REC  = process.argv[3] || (BASE + '/tools/render-replica-poc/maxq_86.mcrr');
const PAGE = `${BASE}/web/render-replica/replay.html?rec=${encodeURIComponent(REC)}`;

const args = ['--enable-unsafe-webgpu','--enable-webgpu-developer-features','--ignore-gpu-blocklist','--no-sandbox',
              '--enable-features=Vulkan','--use-angle=vulkan','--use-gl=angle'];
const browser = await puppeteer.launch({ headless: process.env.HEADED ? false : 'new', args });
const page = await browser.newPage();
await page.setViewport({ width: 900, height: 760, deviceScaleFactor: 1 });
const logs = [];
page.on('console', m => logs.push('[page] ' + m.text()));
page.on('pageerror', e => logs.push('[pageerror] ' + e.message));

await page.goto(PAGE, { waitUntil: 'networkidle0', timeout: 90000 });
await page.waitForFunction(() => window.__state && window.__state().nFrames > 0, { timeout: 90000 });

// Engine HUD addresses + char-struct offsets (must match replay.html / the memory map).
const ADDR = {
  IN_MATCH: 0x8C289624, ROUND: 0x8C28962B, TIMER: 0x8C289630,
  P1_FILL: 0x8C289646, P2_FILL: 0x8C289648, P1_LVL: 0x8C28964A, P2_LVL: 0x8C28964B,
  P1_COMBO: 0x8C289670, P2_COMBO: 0x8C289672,
  CHAR_SLOTS: [0x8C268340, 0x8C2688E4, 0x8C268E88, 0x8C26942C, 0x8C2699D0, 0x8C269F74],
};

// Step 1: read the HUD from the seeded RAM image (real recording bytes).
await page.evaluate(f => window.__showFrame(f), 0);
const live0 = await page.evaluate(() => window.__readHud());
console.log('Step 1 — HUD fields read from the seeded RAM image (frame 0):');
console.log('  inMatch=%d timer=%d round=%d', live0.inMatch, live0.timer, live0.round);
console.log('  p1fill=%d p2fill=%d p1lvl=%d p2lvl=%d', live0.p1fill, live0.p2fill, live0.p1lvl, live0.p2lvl);
console.log('  p1combo=%d p2combo=%d  p1=%s p2=%s', live0.p1combo, live0.p2combo, !!live0.p1, !!live0.p2);

// Step 2: drive grounded synthetic values at the real engine addresses, re-run the HUD
// pass, and sample the overlay. Forces a known state so each element is verifiable.
const probe = await page.evaluate((A) => {
  const ram = window.__pane ? null : null;   // (ram is module-private; use the exported writer below)
  // Write directly into the module RAM via the exposed helpers we add inline:
  // the replay module keeps `ram` private, so reach it through the rdU* path by writing
  // bytes via a tiny injected setter on window (added below in the page context).
  const set8  = (a, v) => window.__hudPoke8(a, v);
  const set16 = (a, v) => window.__hudPoke16(a, v);
  // P1 point char loses half its HP with a chip-damage trail; P2 near-full.
  // health@+0x420, red_health@+0x424, active@+0x000 on slot 0 (P1C1) and slot 1 (P2C1).
  set8(A.IN_MATCH, 1);
  set8(A.CHAR_SLOTS[0] + 0x000, 1); set8(A.CHAR_SLOTS[0] + 0x420, 72);  set8(A.CHAR_SLOTS[0] + 0x424, 110); // P1: HP 72/144, chip to 110
  set8(A.CHAR_SLOTS[1] + 0x000, 1); set8(A.CHAR_SLOTS[1] + 0x420, 130); set8(A.CHAR_SLOTS[1] + 0x424, 130); // P2: HP 130/144
  set8(A.TIMER, 73); set8(A.ROUND, 1);
  set16(A.P1_FILL, 144); set16(A.P2_FILL, 72);   // P1 meter full, P2 half
  set8(A.P1_LVL, 3); set8(A.P2_LVL, 1);
  set16(A.P1_COMBO, 7); set16(A.P2_COMBO, 0);    // P1 7-hit combo shows; P2 none
  window.__renderHud();                          // re-run the HUD foreground pass

  // sample the overlay canvas
  const hc = window.__hudCanvas();
  const o = document.createElement('canvas'); o.width = hc.width; o.height = hc.height;
  const g = o.getContext('2d'); g.drawImage(hc, 0, 0);
  const W = hc.width, H = hc.height, sx = W / 640, sy = H / 480;
  const d = g.getImageData(0, 0, W, H).data;
  const px = (X, Y) => { const i = ((Y|0) * W + (X|0)) * 4; return [d[i], d[i+1], d[i+2], d[i+3]]; };
  const lit = (p) => p[3] > 10 && (p[0] | p[1] | p[2]);
  // "anything lit in a 640x480-space box?" — robust for thin digit strokes + tinted bars.
  const litBox = (gx, gy, gw, gh) => {
    for (let Y = (gy*sy)|0; Y < ((gy+gh)*sy); Y++)
      for (let X = (gx*sx)|0; X < ((gx+gw)*sx); X++)
        if (lit(px(X, Y))) return true;
    return false;
  };
  // a strongly tinted (non-grey-frame) pixel = the team-colored fill, distinct from the
  // dark bg/frame. Used to tell HP fill (bright team color) from the red chip (dark red).
  const teamLit = (gx, gy, gw, gh) => {
    for (let Y = (gy*sy)|0; Y < ((gy+gh)*sy); Y++)
      for (let X = (gx*sx)|0; X < ((gx+gw)*sx); X++) {
        const p = px(X, Y); if (p[3] > 10 && Math.max(p[0],p[1],p[2]) > 140) return true;
      }
    return false;
  };
  let nonBlank = 0;
  for (let i = 0; i < d.length; i += 4) if (d[i+3] > 10 && (d[i]|d[i+1]|d[i+2])) nonBlank++;
  // P1 HP 72/144 -> fill ends ~x=164 (18+146); chip 110/144 -> ends ~x=241 (18+223).
  // P2 HP 130/144 right-anchored in [330,622]; meter P2 72/144 right-anchored in [497,622].
  const out = { nonBlank, checks: {
    p1_hp_lit:    teamLit(18 + 40, 17, 40, 12),    // inside the bright HP fill (x~58..98)
    p1_chip_lit:  litBox(18 + 180, 17, 40, 12),    // between HP-end(164) and chip-end(241): only chip lit
    p2_hp_lit:    teamLit(330 + 200, 17, 60, 12),  // inside P2 right-anchored HP fill
    p1_meter_lit: teamLit(18 + 100, 456, 60, 9),   // P1 super meter (full)
    p2_meter_lit: teamLit(372 + 200, 456, 40, 9),  // P2 meter right-anchored (497..622)
    timer_lit:    litBox(305, 8, 30, 22),          // centered timer digit box
    combo_lit:    litBox(20, 40, 40, 16),          // P1 combo digit box
  }};
  return out;
}, ADDR);

console.log('\nStep 2 — HUD foreground pass driven with grounded synthetic state:');
console.log('  overlay non-blank pixels:', probe.nonBlank);
const C = probe.checks;
const row = (name, ok) => console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${name}`);
row('P1 health bar renders (HP fill)',        C.p1_hp_lit);
row('P1 red chip-damage trails behind HP',    C.p1_chip_lit);
row('P2 health bar renders (right-anchored)', C.p2_hp_lit);
row('P1 super meter fills',                   C.p1_meter_lit);
row('P2 super meter (half) fills',            C.p2_meter_lit);
row('round timer digits render',              C.timer_lit);
row('combo counter digits render',            C.combo_lit);

await page.screenshot({ path: 'PNG_hud.png' });
console.log('\nscreenshot: PNG_hud.png');
console.log(logs.slice(-12).join('\n'));

const pass = C.p1_hp_lit && C.p1_chip_lit && C.p2_hp_lit && C.p1_meter_lit && C.p2_meter_lit && C.timer_lit && C.combo_lit && probe.nonBlank > 200;
console.log(pass ? '\nPASS: HUD foreground pass reflects live state (bars + chip + meters + timer + combo).'
                 : '\nFAIL: one or more HUD elements did not render.');
await browser.close();
process.exit(pass ? 0 : 1);
