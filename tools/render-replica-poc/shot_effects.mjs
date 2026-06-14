// shot_effects.mjs — validate the PHASE-3 EFFECTS pass in headless Chrome.
// Loads a real .mcrr (which carries the live Effect-Poly directory), POKES a synthetic
// effect node bound via the grounded binding (gfx15C -> a real directory entry) at a known
// anchor, runs the effects pass, and reads back the additive overlay canvas to PROVE the
// effect quad drew centered on the anchor. Screenshots a clean frame + the effect frame.
//   node tools/render-replica-poc/shot_effects.mjs <base_url> <rec_url> [dirIdx] [sx] [sy]
import puppeteer from 'puppeteer';

const BASE = process.argv[2] || 'http://localhost:8099';
const REC  = process.argv[3] || (BASE + '/tools/render-replica-poc/rot_cap.mcrr');
const IDX  = +(process.argv[4] ?? 0);
const SX   = +(process.argv[5] ?? 320);
const SY   = +(process.argv[6] ?? 240);
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
// wait for the effects atlas to finish loading (effectsReady gate)
await page.waitForFunction(() => window.__effectsReady && window.__effectsReady() === true, { timeout: 30000 });

// clean frame first (no effect)
await page.evaluate(() => window.__showFrame(0));
await new Promise(r => setTimeout(r, 200));
await page.screenshot({ path: 'PNG_effects_clean.png' });

// poke + render the effect, then read back the additive overlay's non-zero pixels.
const res = await page.evaluate((idx, sx, sy) => {
    window.__showFrame(0);                       // re-seed the frame (clears poked state)
    const poked = window.__pokeEffect(idx, sx, sy);
    const drew = window.__renderEffects();        // run the effects pass with the poked node
    const nodes = window.__effectNodes();
    // read back the effects overlay: count non-transparent px + their centroid
    const o = window.__effectsOverlay();
    let n = 0, cx = 0, cy = 0, minx=1e9, maxx=-1e9, miny=1e9, maxy=-1e9;
    if (o) {
        const g = o.getContext('2d');
        const d = g.getImageData(0, 0, o.width, o.height).data;
        for (let y = 0; y < o.height; y++) for (let x = 0; x < o.width; x++) {
            const i = (y*o.width + x)*4;
            if (d[i+3] > 8) { n++; cx += x; cy += y;
                if(x<minx)minx=x; if(x>maxx)maxx=x; if(y<miny)miny=y; if(y>maxy)maxy=y; }
        }
        // map the overlay-pixel centroid back to game space (overlay is canvas-sized)
        const W = o.width, H = o.height;
        if (n) { cx = cx/n * 640/W; cy = cy/n * 480/H;
                 minx=minx*640/W; maxx=maxx*640/W; miny=miny*480/H; maxy=maxy*480/H; }
    }
    return { poked, drew, nodes, n, cx, cy, w: maxx-minx, h: maxy-miny, sx, sy };
}, IDX, SX, SY);

await new Promise(r => setTimeout(r, 200));
await page.screenshot({ path: 'PNG_effects_poked.png' });

console.log(logs.slice(-20).join('\n'));
console.log('\nPOKE:', JSON.stringify(res.poked));
console.log('resolved nodes:', JSON.stringify(res.nodes));
console.log(`effects pass drew ${res.drew} quad(s)`);
console.log(`overlay non-transparent px: ${res.n}  centroid=(${res.cx.toFixed(0)},${res.cy.toFixed(0)})  bbox≈${res.w.toFixed(0)}x${res.h.toFixed(0)}  expected anchor=(${SX},${SY})`);

const centerOK = res.n > 100 && Math.abs(res.cx - SX) < 12 && Math.abs(res.cy - SY) < 12;
console.log(centerOK
    ? 'PASS: effect quad rendered, centered on the node anchor (point-centered binding correct).'
    : 'FAIL: effect did not render at the expected anchor.');
console.log('screenshots: PNG_effects_clean.png, PNG_effects_poked.png');
await browser.close();
process.exit(centerOK ? 0 : 1);
