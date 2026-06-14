// shot_emitter_verify.mjs — ON-SCREEN verification of the EMITTER body draw (depth-sort fix).
//   node shot_emitter_verify.mjs <base> <rec_url> [outPrefix] [frame]
// Drives replay.html (bodymode=emitter), WAITS for the emitter atlases to actually load
// (loadAsmChar resolves async), re-renders, then screenshots + reports per-frame body part
// count, engZ presence, and the body-overlay pixel count (proof of on-canvas pixels).
import puppeteer from 'puppeteer';

const BASE = process.argv[2] || 'http://127.0.0.1:8099';
const REC  = process.argv[3] || (BASE + '/tools/render-replica-poc/_satlive.mcrr');
const OUT  = process.argv[4] || 'PNG_emitter_verify';
const FRAME = process.argv[5] != null ? +process.argv[5] : null;
const PAGE = `${BASE}/web/render-replica/replay.html?bodymode=emitter&rec=${encodeURIComponent(REC)}`;

const SOFTWARE = !!process.env.SOFTWARE;
const args = ['--enable-unsafe-webgpu','--enable-webgpu-developer-features','--ignore-gpu-blocklist','--no-sandbox'];
if (SOFTWARE) args.push('--use-webgpu-adapter=swiftshader','--enable-features=Vulkan');
else args.push('--enable-features=Vulkan','--use-angle=vulkan','--use-gl=angle');
const browser = await puppeteer.launch({ headless: 'new', args });
const page = await browser.newPage();
await page.setViewport({ width: 900, height: 760, deviceScaleFactor: 1 });
const logs = [];
page.on('console', m => logs.push('[page] ' + m.text()));
page.on('pageerror', e => logs.push('[pageerror] ' + e.message));
await page.goto(PAGE, { waitUntil: 'domcontentloaded', timeout: 90000 });
try { await page.waitForFunction(() => window.__state && window.__state().nFrames > 0, { timeout: 90000 }); }
catch (e) { console.log('LOAD FAIL:\n' + logs.slice(-30).join('\n')); await browser.close(); process.exit(2); }

const st = await page.evaluate(() => window.__state());
const N = st.nFrames;
// VFRAME=<n> env -> resolve to the file-frame index (the validated Storm+Cable frame).
let frames;
if (process.env.VFRAME != null) {
  const idx = await page.evaluate((vf) => window.__frameIndexForVframe(vf), +process.env.VFRAME);
  frames = [idx >= 0 ? idx : 0];
  console.log(`VFRAME ${process.env.VFRAME} -> frame index ${frames[0]}`);
} else {
  frames = FRAME != null ? [FRAME] : [0, Math.floor(N/3), Math.floor(2*N/3), N-1].filter((v,i,a)=>a.indexOf(v)===i);
}

async function probe(f) {
  return await page.evaluate(async (fr) => {
    window.__showFrame(fr);
    // wait until the emitter has a non-"no atlas"/loading note OR ~2.5s elapsed (atlas fetch)
    const sc = window._spriteclient;
    const t0 = performance.now();
    while (performance.now() - t0 < 2500) {
      await new Promise(r => setTimeout(r, 100));
      window.__showFrame(fr);
      const loaded = Object.values(sc.asmChars||{}).filter(c => c && c.img).length;
      const want = sc.slot.filter(s => s.active).length;
      if (loaded >= want && want > 0 && (sc._asmDrawn||0) > 0) break;
    }
    window.__showFrame(fr);
    const drawn = sc._asmDrawn || 0;
    const stage = document.getElementById('cReplay');
    const body  = window.__bodyCanvas ? window.__bodyCanvas() : null;
    let bodyPx = 0, bcx = 0, bcy = 0;
    if (body) {
      const ob = document.createElement('canvas'); ob.width = body.width; ob.height = body.height;
      const gb = ob.getContext('2d'); gb.drawImage(body, 0, 0);
      const db = gb.getImageData(0,0,ob.width,ob.height).data;
      for (let y=0;y<ob.height;y++) for (let x=0;x<ob.width;x++){ const i=(y*ob.width+x)*4; if (db[i]|db[i+1]|db[i+2]|db[i+3]){bodyPx++;bcx+=x;bcy+=y;} }
    }
    const dl = window.__lastBodyDrawList || [];
    const engZok = dl.filter(it => it.engZ != null).length;
    const slots = sc.slot.map(s=>({a:s.active,cid:s.char_id,sid:s.sprite_id,engZ:s.engZ!=null?+s.engZ.toFixed(6):null,layer:s.draw_layer}));
    const atlas = Object.entries(sc.asmChars||{}).map(([c,v])=>`${c}:${v&&v.img?'ok':(v&&v.missing?'MISS':'load')}`);
    return { drawn, bodyPx, bcx: bodyPx?bcx/bodyPx:NaN, bcy: bodyPx?bcy/bodyPx:NaN, engZok, dlN: dl.length, slots, atlas, note: sc._asmNote };
  }, f);
}

const out = [];
for (const f of frames) {
  const r = await probe(f);
  out.push({ f, ...r });
  await page.screenshot({ path: `${OUT}_f${String(f).padStart(3,'0')}.png` });
}
console.log(logs.slice(-12).join('\n'));
console.log(`\nEMITTER on-screen verify over ${N} frames:`);
const fx = (v) => (v == null || (typeof v === 'number' && isNaN(v))) ? '-' : (+v).toFixed(0);
for (const r of out) {
  console.log(`  frame ${String(r.f).padStart(3)}: parts=${r.drawn} engZ/parts=${r.engZok}/${r.dlN} bodyOverlayPx=${r.bodyPx} centroid=(${fx(r.bcx)},${fx(r.bcy)}) | ${r.note || '?'}`);
  console.log(`      slots: ${JSON.stringify((r.slots||[]).filter(s=>s.a))}`);
  console.log(`      atlas: ${(r.atlas||[]).join(' ')}`);
}
const anyDrawn = out.some(r => r.drawn > 0);
const anyPx = out.some(r => r.bodyPx > 0);
const allEngZ = out.every(r => r.dlN === 0 || r.engZok === r.dlN);
console.log(`\nparts emitted: ${anyDrawn?'YES':'NO'}; body pixels on canvas: ${anyPx?'YES':'no (software readback may be black)'}; engZ on all parts: ${allEngZ?'YES':'NO'}`);
console.log(`screenshots: ${frames.map(f=>`${OUT}_f${String(f).padStart(3,'0')}.png`).join(', ')}`);
await browser.close();
process.exit(anyDrawn ? 0 : 1);
