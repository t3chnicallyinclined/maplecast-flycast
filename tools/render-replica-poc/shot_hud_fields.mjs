// shot_hud_fields.mjs — VALIDATE the two new HUD field reads:
//   (1) PER-SIDE WIN STARS from char[pointSlot]+0x540 (num_wins)
//   (2) DETERMINISTIC ∞ from game-mode byte 0x8C26828C (replaces held-99 heuristic)
// Loads replay.html with a recorded MCRR, pokes the fields via __hudPoke8/16, re-runs
// the HUD pass (__renderHud), and screenshots the HUD overlay. Captures 3 panels:
//   A) infinite-time (gameMode=0x02) + P1=2 wins, P2=1 win   -> expect ∞ + 2|1 stars
//   B) normal-time   (gameMode=0x07) + same wins             -> expect "99" + 2|1 stars
//   C) zero wins both sides, infinite                        -> expect ∞ + no stars
//   node tools/render-replica-poc/shot_hud_fields.mjs
import puppeteer from 'puppeteer';
import { createServer } from 'http';
import { readFile, writeFile } from 'fs/promises';
import { extname, join, normalize } from 'path';

const ROOT = process.cwd();
const PORT = 8142;
const MIME = { '.html':'text/html', '.mjs':'text/javascript', '.js':'text/javascript',
  '.json':'application/json', '.png':'image/png', '.wasm':'application/wasm',
  '.bin':'application/octet-stream', '.mcrr':'application/octet-stream' };
const server = createServer(async (req, res) => {
  try {
    const p = normalize(decodeURIComponent(req.url.split('?')[0])).replace(/^(\.\.[/\\])+/, '');
    const data = await readFile(join(ROOT, p));
    res.writeHead(200, { 'Content-Type': MIME[extname(p)] || 'application/octet-stream', 'Access-Control-Allow-Origin':'*' });
    res.end(data);
  } catch { res.writeHead(404); res.end('nf'); }
});
await new Promise(r => server.listen(PORT, r));
const BASE = `http://localhost:${PORT}`;
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
  .then(() => console.log('atlas: HUD atlas LOADED')).catch(() => console.log('atlas: fallback'));

const A = { IN_MATCH:0x8C289624, TIMER:0x8C289630, GAME_MODE:0x8C26828C,
  P1_FILL:0x8C289646, P2_FILL:0x8C289648, P1_LVL:0x8C28964A, P2_LVL:0x8C28964B,
  P1_COMBO:0x8C289670, P2_COMBO:0x8C289672, OFF_WINS:0x540,
  SLOTS:[0x8C268340,0x8C2688E4,0x8C268E88,0x8C26942C,0x8C2699D0,0x8C269F74] };

async function panel(label, gameMode, infMode, p1wins, p2wins) {
  return await page.evaluate(({A, gameMode, infMode, p1wins, p2wins}) => {
    window._infTimeMode = infMode;     // the gate value drawHUD compares against
    const p8 = window.__hudPoke8, p16 = window.__hudPoke16;
    p8(A.IN_MATCH, 1); p8(A.TIMER, 99); p8(A.GAME_MODE, gameMode);
    const setSlot = (s, cid, active, hp, red, wins) => { const b = A.SLOTS[s];
      p8(b+0x000, active); p8(b+0x001, cid); p8(b+0x420, hp); p8(b+0x424, red); p8(b+A.OFF_WINS, wins); };
    // P1 team (0,2,4): point = STORM with p1wins; reserves 0 wins
    setSlot(0, 0x2A, 1, 144, 144, p1wins);
    setSlot(2, 0x34, 0, 120, 130, 0);
    setSlot(4, 0x17, 0, 144, 144, 0);
    // P2 team (1,3,5): point = CABLE with p2wins
    setSlot(1, 0x17, 1, 130, 130, p2wins);
    setSlot(3, 0x0F, 0, 0,   40,  0);
    setSlot(5, 0x2C, 0, 70,  90,  0);
    p16(A.P1_FILL, 144); p16(A.P2_FILL, 72); p8(A.P1_LVL, 3); p8(A.P2_LVL, 1);
    p16(A.P1_COMBO, 7); p16(A.P2_COMBO, 0);
    window.__renderHud();
    const ac = window.__hudCanvas();
    const ax = ac.getContext('2d'); const d = ax.getImageData(0,0,ac.width,ac.height).data;
    let lit=0; for (let i=3;i<d.length;i+=4) if (d[i]>8) lit++;
    return { W: ac.width, H: ac.height, lit, url: ac.toDataURL('image/png') };
  }, {A, gameMode, infMode, p1wins, p2wins});
}

// gate value 0x02 = infinite (per re_kb finding); 0x07 = "some other normal mode" control.
const pA = await panel('inf+2/1', 0x02, 0x02, 2, 1);   // ∞ shown, 2|1 stars
const pB = await panel('norm+2/1',0x07, 0x02, 2, 1);   // numeric 99, 2|1 stars
const pC = await panel('inf+0/0', 0x02, 0x02, 0, 0);   // ∞ shown, no stars

// stitch the three HUD canvases vertically into one PNG for eyeball + diff
const stitched = await page.evaluate(({a,b,c}) => {
  const load = (u)=>new Promise(r=>{const im=new Image();im.onload=()=>r(im);im.src=u;});
  return Promise.all([load(a),load(b),load(c)]).then(ims=>{
    const cv=document.createElement('canvas'); cv.width=ims[0].width; cv.height=ims[0].height*3+16;
    const x=cv.getContext('2d'); x.fillStyle='#0c1016'; x.fillRect(0,0,cv.width,cv.height);
    x.fillStyle='#fff'; x.font='10px monospace';
    ims.forEach((im,i)=>{ const y=i*(im.height+8); x.drawImage(im,0,y);
      x.fillText(['A inf gm=0x02  P1=2 P2=1 -> EXPECT INF + 2|1 stars',
                  'B norm gm=0x07 P1=2 P2=1 -> EXPECT 99 + 2|1 stars',
                  'C inf gm=0x02 P1=0 P2=0 -> EXPECT INF + no stars'][i], 4, y+10); });
    return cv.toDataURL('image/png');
  });
}, {a:pA.url, b:pB.url, c:pC.url});

console.log('panels lit px: A=%d B=%d C=%d', pA.lit, pB.lit, pC.lit);
await writeFile('_hud_fields_after.png', Buffer.from(stitched.split(',')[1], 'base64'));
console.log('wrote _hud_fields_after.png');

// zoomed top-center crop, P1=3 wins / P2=1 win, infinite -> confirm star COUNT + ∞
const crop = await page.evaluate(({A}) => {
  window._infTimeMode = 0x02;
  const p8 = window.__hudPoke8, p16 = window.__hudPoke16;
  p8(A.IN_MATCH,1); p8(A.TIMER,99); p8(A.GAME_MODE,0x02);
  const set=(s,act,cid,w)=>{const b=A.SLOTS[s];p8(b,act);p8(b+0x001,cid);p8(b+0x420,144);p8(b+0x424,144);p8(b+A.OFF_WINS,w);};
  set(0,1,0x2A,3); set(2,0,0x34,0); set(4,0,0x17,0);
  set(1,1,0x17,1); set(3,0,0x0F,0); set(5,0,0x2C,0);
  window.__renderHud();
  const ac = window.__hudCanvas();
  const sx = ac.width/640, sy = ac.height/480;
  const cv = document.createElement('canvas'); cv.width = 200*4; cv.height = 40*4;
  const x = cv.getContext('2d'); x.imageSmoothingEnabled=false; x.fillStyle='#202830'; x.fillRect(0,0,cv.width,cv.height);
  x.drawImage(ac, 230*sx, 0, 200*sx, 40*sy, 0,0, 200*4, 40*4);
  return cv.toDataURL('image/png');
}, {A});
await writeFile('_hud_stars_crop.png', Buffer.from(crop.split(',')[1],'base64'));
console.log('wrote _hud_stars_crop.png (P1=3 left, P2=1 right, gm=0x02 inf, 4x zoom)');
await browser.close(); server.close();
process.exit(0);
