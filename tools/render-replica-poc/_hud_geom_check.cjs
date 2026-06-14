// Faithful-enough software 2D-canvas shim to exercise the REAL renderHudPass code path
// extracted verbatim from replay.html, then sample the harness validation boxes. This
// proves the new ANGLED layout's geometry produces correctly-placed lit pixels without
// needing a headless WebGPU browser.
const fs = require('fs');
const html = fs.readFileSync('web/render-replica/replay.html', 'utf8');

function parseColor(s) {
  if (typeof s !== 'string') return [255, 255, 255, 255];
  if (s[0] === '#') { let h = s.slice(1); if (h.length === 3) h = h.split('').map(c => c + c).join('');
    return [parseInt(h.slice(0, 2), 16), parseInt(h.slice(2, 4), 16), parseInt(h.slice(4, 6), 16), 255]; }
  const m = s.match(/rgba?\(([^)]+)\)/); if (m) { const p = m[1].split(',').map(x => parseFloat(x));
    return [p[0] | 0, p[1] | 0, p[2] | 0, p[3] == null ? 255 : Math.round(p[3] * 255)]; }
  return [255, 255, 255, 255];
}
class Grad { constructor() { this.stops = []; } addColorStop(o, c) { this.stops.push([o, c]); } _mid() { return this.stops[0] ? this.stops[0][1] : '#fff'; } }
class Ctx {
  constructor(W, H) { this.W = W; this.H = H; this.buf = new Uint8ClampedArray(W * H * 4);
    this.fillStyle = '#000'; this.strokeStyle = '#000'; this.lineWidth = 1; this.imageSmoothingEnabled = false;
    this._path = []; this._stack = []; this._t = { a: 1, d: 1, e: 0, f: 0 }; this._clip = null; }
  save() { this._stack.push({ fill: this.fillStyle, stroke: this.strokeStyle, lw: this.lineWidth, t: { ...this._t }, clip: this._clip }); }
  restore() { const s = this._stack.pop(); if (s) { this.fillStyle = s.fill; this.strokeStyle = s.stroke; this.lineWidth = s.lw; this._t = s.t; this._clip = s.clip; } }
  scale(sx, sy) { this._t.a *= sx; this._t.d *= sy; }
  _tx(x, y) { return [x * this._t.a + this._t.e, y * this._t.d + this._t.f]; }
  createLinearGradient() { return new Grad(); }
  beginPath() { this._path = []; }
  moveTo(x, y) { this._path.push(['M', ...this._tx(x, y)]); }
  lineTo(x, y) { this._path.push(['L', ...this._tx(x, y)]); }
  closePath() { this._path.push(['Z']); }
  clearRect() { this.buf.fill(0); }
  _poly() { const pts = []; for (const seg of this._path) { if (seg[0] === 'M' || seg[0] === 'L') pts.push([seg[1], seg[2]]); } return pts; }
  _resolveFill() { return this.fillStyle instanceof Grad ? this.fillStyle._mid() : this.fillStyle; }
  _put(x, y, c) { if (x < 0 || y < 0 || x >= this.W || y >= this.H) return;
    if (this._clip && !pointInPoly(x + 0.5, y + 0.5, this._clip)) return;
    const i = (y * this.W + x) * 4; const a = c[3] / 255;
    this.buf[i] = this.buf[i] * (1 - a) + c[0] * a; this.buf[i + 1] = this.buf[i + 1] * (1 - a) + c[1] * a;
    this.buf[i + 2] = this.buf[i + 2] * (1 - a) + c[2] * a; this.buf[i + 3] = Math.max(this.buf[i + 3], c[3]); }
  fillRect(x, y, w, h) { const c = parseColor(this._resolveFill()); const [X, Y] = this._tx(x, y);
    const w2 = w * this._t.a, h2 = h * this._t.d;
    for (let yy = Math.floor(Y); yy < Math.ceil(Y + h2); yy++) for (let xx = Math.floor(X); xx < Math.ceil(X + w2); xx++) this._put(xx, yy, c); }
  strokeRect(x, y, w, h) { const c = parseColor(this.strokeStyle); const [X, Y] = this._tx(x, y);
    const w2 = w * this._t.a, h2 = h * this._t.d;
    for (let xx = Math.floor(X); xx <= Math.ceil(X + w2); xx++) { this._put(xx, Math.floor(Y), c); this._put(xx, Math.ceil(Y + h2), c); }
    for (let yy = Math.floor(Y); yy <= Math.ceil(Y + h2); yy++) { this._put(Math.floor(X), yy, c); this._put(Math.ceil(X + w2), yy, c); } }
  fill() { const pts = this._poly(); if (pts.length < 3) return; const c = parseColor(this._resolveFill()); fillPoly(pts, (x, y) => this._put(x, y, c)); }
  stroke() { const pts = this._poly(); const c = parseColor(this.strokeStyle); for (let i = 0; i < pts.length - 1; i++) line(pts[i], pts[i + 1], (x, y) => this._put(x, y, c)); }
  clip() { this._clip = this._poly().slice(); }
  getImageData() { return { data: this.buf }; }
}
function pointInPoly(x, y, pts) { let inside = false; for (let i = 0, j = pts.length - 1; i < pts.length; j = i++) {
  const xi = pts[i][0], yi = pts[i][1], xj = pts[j][0], yj = pts[j][1];
  if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) inside = !inside; } return inside; }
function fillPoly(pts, put) { let minY = 1e9, maxY = -1e9; for (const p of pts) { minY = Math.min(minY, p[1]); maxY = Math.max(maxY, p[1]); }
  for (let y = Math.floor(minY); y <= Math.ceil(maxY); y++) { const xs = [];
    for (let i = 0, j = pts.length - 1; i < pts.length; j = i++) { const yi = pts[i][1], yj = pts[j][1];
      if ((yi <= y && yj > y) || (yj <= y && yi > y)) { const t = (y - yi) / (yj - yi); xs.push(pts[i][0] + t * (pts[j][0] - pts[i][0])); } }
    xs.sort((a, b) => a - b); for (let k = 0; k + 1 < xs.length; k += 2) for (let x = Math.floor(xs[k]); x <= Math.ceil(xs[k + 1]); x++) put(x, y); } }
function line(a, b, put) { const dx = Math.abs(b[0] - a[0]), dy = Math.abs(b[1] - a[1]); const n = Math.max(dx, dy, 1) | 0;
  for (let i = 0; i <= n; i++) put(Math.round(a[0] + (b[0] - a[0]) * i / n), Math.round(a[1] + (b[1] - a[1]) * i / n)); }

const RAM_LO = 0xFFFFFF;
const ram = new Uint8Array(0x1000000);
const rdU8 = (a) => ram[a & RAM_LO];
const rdU16 = (a) => ram[a & RAM_LO] | (ram[(a + 1) & RAM_LO] << 8);
const CHAR_SLOTS = [0x8C268340, 0x8C2688E4, 0x8C268E88, 0x8C26942C, 0x8C2699D0, 0x8C269F74];
const HP_MAX = 144, METER_MAX = 144;
const HUD_IN_MATCH = 0x8C289624, HUD_ROUND = 0x8C28962B, HUD_TIMER = 0x8C289630;
const HUD_P1_FILL = 0x8C289646, HUD_P2_FILL = 0x8C289648, HUD_P1_LVL = 0x8C28964A, HUD_P2_LVL = 0x8C28964B;
const HUD_P1_COMBO = 0x8C289670, HUD_P2_COMBO = 0x8C289672;
const HUD_P1_SLOTS = [0, 2, 4], HUD_P2_SLOTS = [1, 3, 5];
const HUD_BAR_COLS = [['#FF40FF', '#FFFF00'], ['#00FF00', '#FFFF00'], ['#00C0FF', '#FFFF00']];
function hudPointSlot(slots) { for (let i = 0; i < slots.length; i++) { const base = CHAR_SLOTS[slots[i]]; if (rdU8(base + 0x000)) return { base, colIdx: i }; } return null; }
function readHud() { return { inMatch: rdU8(HUD_IN_MATCH), timer: rdU8(HUD_TIMER), round: rdU8(HUD_ROUND),
  p1fill: rdU16(HUD_P1_FILL), p2fill: rdU16(HUD_P2_FILL), p1lvl: rdU8(HUD_P1_LVL), p2lvl: rdU8(HUD_P2_LVL),
  p1combo: rdU16(HUD_P1_COMBO), p2combo: rdU16(HUD_P2_COMBO), p1: hudPointSlot(HUD_P1_SLOTS), p2: hudPointSlot(HUD_P2_SLOTS) }; }

function extract(name) { const i = html.indexOf('function ' + name + '('); if (i < 0) throw new Error('missing ' + name);
  let d = 0, started = false, j = i; for (; j < html.length; j++) { if (html[j] === '{') { d++; started = true; } else if (html[j] === '}') { d--; if (started && d === 0) { j++; break; } } } return html.slice(i, j); }
const src = [extract('hudDigit'), extract('hudDigits'), extract('hudBar'), extract('hudAngledLifeBar'), extract('renderHudPass')].join('\n');
const _hudCanvas = { width: 640, height: 480 };
const ctxObj = new Ctx(640, 480);
function ensureHudOverlay() { return ctxObj; }
const SEG7 = { '0': 'abcdef', '1': 'bc', '2': 'abged', '3': 'abgcd', '4': 'fgbc', '5': 'afgcd', '6': 'afgcde', '7': 'abc', '8': 'abcdefg', '9': 'abcfgd' };
const fn = new Function('ram', 'rdU8', 'rdU16', 'CHAR_SLOTS', 'HP_MAX', 'METER_MAX', 'HUD_IN_MATCH', 'HUD_ROUND', 'HUD_TIMER',
  'HUD_P1_FILL', 'HUD_P2_FILL', 'HUD_P1_LVL', 'HUD_P2_LVL', 'HUD_P1_COMBO', 'HUD_P2_COMBO', 'HUD_P1_SLOTS', 'HUD_P2_SLOTS',
  'HUD_BAR_COLS', 'hudPointSlot', 'readHud', 'ensureHudOverlay', '_hudCanvas', 'SEG7',
  src + '\nreturn renderHudPass;');
const renderHudPass = fn(ram, rdU8, rdU16, CHAR_SLOTS, HP_MAX, METER_MAX, HUD_IN_MATCH, HUD_ROUND, HUD_TIMER,
  HUD_P1_FILL, HUD_P2_FILL, HUD_P1_LVL, HUD_P2_LVL, HUD_P1_COMBO, HUD_P2_COMBO, HUD_P1_SLOTS, HUD_P2_SLOTS,
  HUD_BAR_COLS, hudPointSlot, readHud, ensureHudOverlay, _hudCanvas, SEG7);

const set8 = (a, v) => { ram[a & RAM_LO] = v & 0xFF; };
const set16 = (a, v) => { ram[a & RAM_LO] = v & 0xFF; ram[(a + 1) & RAM_LO] = (v >> 8) & 0xFF; };
set8(HUD_IN_MATCH, 1);
set8(CHAR_SLOTS[0] + 0x000, 1); set8(CHAR_SLOTS[0] + 0x420, 72); set8(CHAR_SLOTS[0] + 0x424, 110); set8(CHAR_SLOTS[0] + 0x001, 5);
set8(CHAR_SLOTS[1] + 0x000, 1); set8(CHAR_SLOTS[1] + 0x420, 130); set8(CHAR_SLOTS[1] + 0x424, 130); set8(CHAR_SLOTS[1] + 0x001, 12);
set8(HUD_TIMER, 73); set8(HUD_ROUND, 1);
set16(HUD_P1_FILL, 144); set16(HUD_P2_FILL, 72); set8(HUD_P1_LVL, 3); set8(HUD_P2_LVL, 1);
set16(HUD_P1_COMBO, 7); set16(HUD_P2_COMBO, 0);
renderHudPass();

const d = ctxObj.buf, W = 640;
const px = (X, Y) => { const i = ((Y | 0) * W + (X | 0)) * 4; return [d[i], d[i + 1], d[i + 2], d[i + 3]]; };
const lit = p => p[3] > 10 && (p[0] | p[1] | p[2]);
const litBox = (gx, gy, gw, gh) => { for (let Y = gy; Y < gy + gh; Y++) for (let X = gx; X < gx + gw; X++) if (lit(px(X, Y))) return true; return false; };
const teamLit = (gx, gy, gw, gh) => { for (let Y = gy; Y < gy + gh; Y++) for (let X = gx; X < gx + gw; X++) { const p = px(X, Y); if (p[3] > 10 && Math.max(p[0], p[1], p[2]) > 140) return true; } return false; };
let nonBlank = 0; for (let i = 0; i < d.length; i += 4) if (d[i + 3] > 10 && (d[i] | d[i + 1] | d[i + 2])) nonBlank++;
// ANGLED layout: P1 life bar outer X=80 len=212 -> inner ~292; HP 72/144=0.5 bright [80..186],
// chip 110/144=0.76 [80..241]. P2 outer X=560 len=212 -> inner ~348; HP 130/144=0.9 [369..560].
// Meters y=458: P1 outer X=20 len=200 full [20..220]; P2 outer X=620 len=200 half [520..620].
const C = {
  p1_hp_lit: teamLit(110, 20, 40, 9),
  p1_chip_lit: litBox(205, 20, 28, 3),
  p2_hp_lit: teamLit(430, 20, 60, 9),
  p1_meter_lit: teamLit(100, 460, 50, 8),
  p2_meter_lit: teamLit(545, 460, 40, 8),
  timer_lit: litBox(305, 9, 30, 22),
  combo_lit: litBox(20, 44, 40, 16),
};
// chip band TOP edge [186..241] must be LIT (dark red) but NOT bright team color (HP top ends 186).
const p1_chip_dark = litBox(205, 20, 28, 3) && !teamLit(205, 20, 28, 3);
// HP color is magenta (#FF40FF: R>200,B>200,G<120). Near the inner tip (x>=270) there must
// be NO magenta HP fill — HP depletes toward center. (The white bar outline there is allowed.)
const isHP = (X, Y) => { const p = px(X, Y); return p[3] > 10 && p[0] > 200 && p[2] > 200 && p[1] < 120; };
let p1_center_nohp = true;
for (let Y = 20; Y < 30 && p1_center_nohp; Y++) for (let X = 270; X < 300; X++) if (isHP(X, Y)) { p1_center_nohp = false; break; }
console.log('nonBlank', nonBlank);
for (const k of Object.keys(C)) console.log((C[k] ? 'PASS' : 'FAIL'), k);
console.log((p1_chip_dark ? 'PASS' : 'FAIL'), 'p1 chip band is DARK red (HP depleted toward center, chip trails)');
console.log((p1_center_nohp ? 'PASS' : 'FAIL'), 'p1 center has NO bright HP (depletes toward center)');
const pass = Object.values(C).every(Boolean) && p1_chip_dark && p1_center_nohp && nonBlank > 200;
console.log(pass ? '\nOVERALL PASS' : '\nOVERALL FAIL');

// ---- emit a PNG screenshot of the HUD overlay over a dark stage-like backdrop ----
const zlib = require('zlib');
function writePNG(path, W, H, rgba) {
  const bg = [22, 26, 34]; // dark stage-ish backdrop so the HUD reads against "bodies+stage"
  const raw = Buffer.alloc((W * 4 + 1) * H);
  for (let y = 0; y < H; y++) { raw[y * (W * 4 + 1)] = 0;
    for (let x = 0; x < W; x++) { const si = (y * W + x) * 4, di = y * (W * 4 + 1) + 1 + x * 4;
      const a = rgba[si + 3] / 255;
      raw[di] = Math.round(rgba[si] * a + bg[0] * (1 - a));
      raw[di + 1] = Math.round(rgba[si + 1] * a + bg[1] * (1 - a));
      raw[di + 2] = Math.round(rgba[si + 2] * a + bg[2] * (1 - a));
      raw[di + 3] = 255; } }
  const idat = zlib.deflateSync(raw);
  const chunk = (type, data) => { const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const tc = Buffer.concat([Buffer.from(type), data]); const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(tc) >>> 0);
    return Buffer.concat([len, tc, crc]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4); ihdr[8] = 8; ihdr[9] = 6;
  const png = Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', idat), chunk('IEND', Buffer.alloc(0))]);
  fs.writeFileSync(path, png);
}
let _crc; function crc32(buf) { if (!_crc) { _crc = []; for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; _crc[n] = c >>> 0; } }
  let c = 0xFFFFFFFF; for (let i = 0; i < buf.length; i++) c = _crc[(c ^ buf[i]) & 0xFF] ^ (c >>> 8); return c ^ 0xFFFFFFFF; }
writePNG('PNG_hud_geom.png', 640, 480, d);
console.log('screenshot: PNG_hud_geom.png (HUD overlay over dark backdrop)');
process.exit(pass ? 0 : 1);
