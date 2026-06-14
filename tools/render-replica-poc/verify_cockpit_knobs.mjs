// verify_cockpit_knobs.mjs — prove EVERY live-config-cockpit knob actually changes the drawn
// body. The bodies are produced by SpriteClient.buildAssemblyDrawList + applyCfgToBody (the
// cockpit re-route), exposed each frame as window.__lastBodyDrawList. For each knob we:
//   snapshot the drawList -> set window.__cfg[key]=value -> re-render the frame -> snapshot ->
//   assert the serialized drawList CHANGED (or, for list-toggles handled by the pvr2 dbg, that
//   dbgFromCfg() changed). A knob that doesn't move the output is a FAIL.
//
//   node tools/render-replica-poc/verify_cockpit_knobs.mjs [base] [rec]
import puppeteer from 'puppeteer';

const BASE = process.argv[2] || 'http://localhost:8099';
const REC  = process.argv[3] || (BASE + '/tools/render-replica-poc/maxq_86.mcrr');
const PAGE = `${BASE}/web/render-replica/replay.html?rec=${encodeURIComponent(REC)}`;

const args = ['--enable-unsafe-webgpu','--ignore-gpu-blocklist','--no-sandbox',
  '--enable-features=Vulkan','--use-angle=vulkan','--use-gl=angle'];
const browser = await puppeteer.launch({ headless: 'new', args });
const page = await browser.newPage();
await page.setViewport({ width: 900, height: 800 });
const logs = []; page.on('console', m => logs.push(m.text())); page.on('pageerror', e => logs.push('ERR ' + e.message));
await page.goto(PAGE, { waitUntil: 'networkidle0', timeout: 90000 });
await page.waitForFunction(() => window.__state && window.__state().nFrames > 0, { timeout: 90000 });

// render once + give atlases a beat to load (so there ARE quads to perturb)
await page.evaluate(() => window.__showFrame(0));
await new Promise(r => setTimeout(r, 1500));
await page.evaluate(() => window.__showFrame(0));

const baseQuads = await page.evaluate(() => (window.__lastBodyDrawList || []).length);
console.log('base body draw-list quads:', baseQuads);

// Each entry: [key, value, mode]. mode 'dl' = expect the body draw-list to change; 'dbg' =
// expect the pvr2 dbg object (render-list/blend) to change. (Both reach the live render.)
const KNOBS = [
  ['anchorDX', 40, 'dl'], ['anchorDY', 40, 'dl'], ['scale', 1.5, 'dl'],
  ['flipP1', true, 'dl'], ['flipP2', true, 'dl'],
  ['widePart', false, 'dl'], ['isoPart', 3, 'dl'], ['revOrder', true, 'dl'],
  ['texMirror', false, 'dl'], ['flipForce', 'on', 'dl'], ['flipForce', 'off', 'dl'],
  ['carveSide', 'p1', 'dl'], ['colRev', true, 'dl'], ['inTileMir', true, 'dl'],
  ['drawOpaque', false, 'dbg'], ['drawPunch', false, 'dbg'], ['drawTrans', false, 'dbg'],
  ['depthOverride', true, 'dbg'], ['depthFunc', 1, 'dbg', { depthOverride: true }], ['trDepthWrite', true, 'dbg'],
  ['noSort', true, 'dbg'], ['singlePass', false, 'dbg'],
  ['blendOverride', true, 'dbg'], ['blendSrc', 1, 'dbg', { blendOverride: true }], ['blendDst', 1, 'dbg', { blendOverride: true }],
  ['ptThresh', 0, 'dbg'], ['cull', 'back', 'dbg'], ['wire', true, 'wire'],
];

const results = [];
for (const [key, val, mode, pre] of KNOBS) {
  const r = await page.evaluate((key, val, mode, pre) => {
    const ser = () => JSON.stringify((window.__lastBodyDrawList || []).map(q =>
      [q.charId, q.slot, +q.dx.toFixed(2), +q.dy.toFixed(2), +q.dw.toFixed(2), +q.dh.toFixed(2), q.flip ? 1 : 0, q.flipY ? 1 : 0]));
    const dbgSer = () => JSON.stringify(window.__dbgFromCfg ? window.__dbgFromCfg() : {});
    // wire mode: the knob toggles the separate bounds-overlay canvas display, not the draw-list.
    const wireSer = () => { const bc = window.__boundsCanvas && window.__boundsCanvas(); return bc ? bc.style.display : '?'; };
    const probe = mode === 'dl' ? ser : (mode === 'wire' ? wireSer : dbgSer);
    // reset to defaults first so each knob is tested in isolation; apply any gating pre-state.
    if (window.__cfgReset) window.__cfgReset();
    if (pre) Object.assign(window.__cfg, pre);
    window.__showFrame(0);
    const before = probe();
    const beforeN = (window.__lastBodyDrawList || []).length;
    window.__cfg[key] = val;
    window.__showFrame(0);
    const after = probe();
    const afterN = (window.__lastBodyDrawList || []).length;
    return { changed: before !== after, beforeN, afterN };
  }, key, val, mode, pre || null);
  const ok = r.changed;
  results.push({ key, val, mode, ok, ...r });
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${key.padEnd(14)} = ${String(val).padEnd(6)} [${mode}]  quads ${r.beforeN}->${r.afterN}`);
}

const fails = results.filter(r => !r.ok);
console.log(`\n${results.length - fails.length}/${results.length} knobs change the live render.`);
if (fails.length) console.log('FAILS:', fails.map(f => f.key).join(', '));
console.log(logs.slice(-6).join('\n'));
await browser.close();
process.exit(fails.length ? 1 : 0);
