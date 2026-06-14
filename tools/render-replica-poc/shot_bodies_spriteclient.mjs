// shot_bodies_spriteclient.mjs — validate the render-replica BODIES now draw via the proven
// SpriteClient/SpriteGPU emitter (replacing render_frame+body_decoder). Loads a real capture,
// renders frames, and reads:
//   - __bodyDrawn(): parts emitted by buildAssemblyDrawList -> SG (proof the sprite-client ran)
//   - composited centroid of #cReplay (stage) + the body overlay canvas (proof of on-canvas pixels)
// Screenshots the full page (all stacked canvases) so the bodies are visible.
//
//   node tools/render-replica-poc/shot_bodies_spriteclient.mjs <base_url> <rec_url> [outPrefix]
import puppeteer from 'puppeteer';

const BASE = process.argv[2] || 'http://127.0.0.1:8099';
const REC  = process.argv[3] || (BASE + '/tools/render-replica-poc/_satlive.mcrr');
const OUT  = process.argv[4] || 'PNG_bodies_spriteclient';
const PAGE = `${BASE}/web/render-replica/replay.html?rec=${encodeURIComponent(REC)}`;

const SOFTWARE = !!process.env.SOFTWARE;
const args = ['--enable-unsafe-webgpu','--enable-webgpu-developer-features','--ignore-gpu-blocklist','--no-sandbox'];
if (SOFTWARE) args.push('--use-webgpu-adapter=swiftshader','--enable-features=Vulkan');
else args.push('--enable-features=Vulkan','--use-angle=vulkan','--use-gl=angle');

const browser = await puppeteer.launch({ headless: process.env.HEADED ? false : 'new', args });
const page = await browser.newPage();
await page.setViewport({ width: 900, height: 760, deviceScaleFactor: 1 });
const logs = [];
page.on('console', m => logs.push('[page] ' + m.text()));
page.on('pageerror', e => logs.push('[pageerror] ' + e.message));

await page.goto(PAGE, { waitUntil: 'networkidle0', timeout: 90000 });
await page.waitForFunction(() => window.__state && window.__state().nFrames > 0, { timeout: 90000 });

// give the lazy atlas fetches (loadAsmChar) a moment to land after the first render
async function probeFrame(f) {
    return await page.evaluate(async (fr) => {
        window.__showFrame(fr);
        await new Promise(r => setTimeout(r, 120));
        window.__showFrame(fr);   // re-render now that atlases are resident
        const drawn = window.__bodyDrawn ? window.__bodyDrawn() : -1;
        // composite the stage (#cReplay) + the body overlay into one bitmap, count non-black px
        const stage = document.getElementById('cReplay');
        const body  = window.__bodyCanvas ? window.__bodyCanvas() : null;
        const o = document.createElement('canvas'); o.width = stage.width; o.height = stage.height;
        const g = o.getContext('2d');
        g.drawImage(stage, 0, 0);
        if (body) g.drawImage(body, 0, 0);
        const d = g.getImageData(0, 0, o.width, o.height).data;
        let sx = 0, n = 0, bodyPx = 0;
        for (let y = 0; y < o.height; y++) for (let x = 0; x < o.width; x++) {
            const i = (y * o.width + x) * 4; if (d[i] | d[i+1] | d[i+2]) { sx += x; n++; }
        }
        // body-only px (the overlay alone)
        if (body) {
            const ob = document.createElement('canvas'); ob.width = body.width; ob.height = body.height;
            const gb = ob.getContext('2d'); gb.drawImage(body, 0, 0);
            const db = gb.getImageData(0, 0, ob.width, ob.height).data;
            for (let i = 0; i < db.length; i += 4) if (db[i] | db[i+1] | db[i+2] | db[i+3]) bodyPx++;
        }
        const st = window.__state();
        const slots = (window._spriteclient ? window._spriteclient.slot : []).map(s => ({ a: s.active, cid: s.char_id, sid: s.sprite_id }));
        return { drawn, n, cx: n ? sx/n : NaN, bodyPx, slots, asmNote: window._spriteclient ? window._spriteclient._asmNote : '' };
    }, f);
}

const st = await page.evaluate(() => window.__state());
const N = st.nFrames;
const probes = [0, Math.floor(N/3), Math.floor(2*N/3), N - 1].filter((v,i,a) => a.indexOf(v) === i);
const out = [];
for (const f of probes) {
    const r = await probeFrame(f);
    out.push({ f, ...r });
    await page.screenshot({ path: `${OUT}_f${String(f).padStart(3,'0')}.png` });
}

console.log(logs.slice(-30).join('\n'));
console.log(`\nBODIES via SpriteClient/SpriteGPU emitter (frames over ${N}-frame capture):`);
for (const r of out)
    console.log(`  frame ${String(r.f).padStart(3)}: parts drawn=${r.drawn}  bodyOverlayPx=${r.bodyPx}  composited=${r.n}px centroidX=${(r.cx==null||isNaN(r.cx))?'-':r.cx.toFixed(1)}  | ${r.asmNote}`);
console.log('  slots[f0]:', JSON.stringify(out[0].slots));
const anyDrawn = out.some(r => r.drawn > 0);
const anyBodyPx = out.some(r => r.bodyPx > 0);
console.log(`\nparts emitted by buildAssemblyDrawList: ${anyDrawn ? 'YES' : 'NO'}; body overlay has pixels: ${anyBodyPx ? 'YES (real GPU)' : 'no (software WebGPU readback may be black)'}`);
const pass = anyDrawn;   // the sprite-client emitter produced body parts = the swap is live
console.log(pass ? 'PASS: render-replica bodies are drawn by the proven SpriteClient emitter path.'
                 : 'FAIL: sprite-client emitted 0 body parts (check atlas load / slot population).');
console.log('screenshots:', probes.map(f => `${OUT}_f${String(f).padStart(3,'0')}.png`).join(', '));
await browser.close();
process.exit(pass ? 0 : 1);
