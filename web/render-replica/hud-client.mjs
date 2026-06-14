// ============================================================================
// hud-client.mjs — Render-Replica PHASE 3 HUD with REAL MVC2 art.
//
// Replaces the synthetic 7-segment-digit / placeholder-portrait overlay with the
// game's OWN pixel art, driven by the live engine state (the SAME RAM addresses
// the production GSTA wire reads — confirmed, not modeled). [hud:real-art-v1]
//
// WHAT IS REAL PIXEL ART vs WHAT IS PROCEDURAL (engine-faithful):
//   • TIMER digits + COMBO/HIT digits + LEVEL digits  -> REAL boxed-digit sprites
//       from FONT.BIN rec1 (64x64 ARGB4444 twiddled HUD glyph sheet), de-rotated to
//       upright and luminance-keyed. Atlas built by tools/rip_hud_atlas.py, served
//       under ./hud/hud_atlas.{png,json}. These are the iconic MVC2 HUD digits.
//   • LIFE BAR + SUPER METER fill  -> PROCEDURAL by design. The engine does NOT use a
//       distinct bar sprite: it draws a WHITE texel polygon MODULATED by a per-team-slot
//       vertex color (loc_8c15FFB0). So the faithful reconstruction is the angled
//       parallelogram filled with the same per-slot team color + the dark-red chip
//       (red_health) trail BEHIND the live HP — NOT a separate art asset. We DO source
//       the white fill texel from the atlas (bar_white) and tint it, matching the
//       engine's "white tex * vertex color" exactly.
//
// FIELD ADDRESSES (CONFIRMED — re_kb finding:replica_live_hud / source:hud_field_addresses,
//   = the production GSTA wire's own reads, NOT guessed):
//   per-char (char base + off): health +0x420 u8, red_health +0x424 u8, char_id +0x001 u8,
//     active +0x000 u8.  globals (page 649): in_match 0x8C289624, round 0x8C28962B,
//     timer 0x8C289630, p1/p2 meter_fill u16 0x8C289646/648, p1/p2 meter_level u8
//     0x8C28964A/64B, p1/p2 combo u16 0x8C289670/672.  HP_MAX=144, METER_MAX=144.
//
// USAGE (replay.html): const hud = new HudClient(baseURL); hud.attach(hostCanvas);
//   hud.load();  // async, non-blocking — vector-digit fallback until ready
//   every frame, AFTER bodies+stage+effects:  hud.render(rdU8, rdU16, charSlots);
// ============================================================================

const HP_MAX = 144, METER_MAX = 144;
const A = {                                     // engine field addresses (global, page 649)
    IN_MATCH: 0x8C289624, ROUND: 0x8C28962B, TIMER: 0x8C289630,
    P1_FILL: 0x8C289646, P2_FILL: 0x8C289648,
    P1_LVL:  0x8C28964A, P2_LVL:  0x8C28964B,
    P1_COMBO:0x8C289670, P2_COMBO:0x8C289672,
};
const OFF_ACTIVE = 0x000, OFF_CID = 0x001, OFF_HP = 0x420, OFF_RED = 0x424;
const P1_SLOTS = [0, 2, 4], P2_SLOTS = [1, 3, 5];

// Per-team life-bar / meter modulate color (loc_8c15FFB0): which of a side's 3 chars
// (C1/C2/C3) is the active point picks magenta / green / cyan. HP fill is the SOLID
// bright color (NOT a full-width gradient); the inner stop is the lighter highlight.
const BAR_COLS = [['#FF40FF', '#FF9CFF'], ['#3CFF3C', '#BBFFAA'], ['#33C8FF', '#AEEBFF']];
const CHIP_COL = '#8a1414';                     // dark recoverable red (red_health trail)

export class HudClient {
    // base = URL of the hud atlas dir (hud_atlas.{png,json} live under <base>).
    constructor(base) {
        this.base = base;
        this.meta = null;          // hud_atlas.json (digit rects + bar_white + colors)
        this.img = null;           // HTMLImageElement of hud_atlas.png
        this.ready = false;
        this._canvas = null;       // 2D overlay canvas (on top of bodies/stage/effects)
        this._ctx = null;
        this._host = null;
    }

    // Load the real-art atlas. Non-blocking: until it resolves, render() uses the
    // built-in vector-digit fallback so the HUD is never blank.
    async load() {
        const url = new URL('hud_atlas', this.base);
        const [meta, img] = await Promise.all([
            fetch(new URL('hud_atlas.json', this.base)).then(r => r.ok ? r.json() : Promise.reject(r.status)),
            this._loadImg(new URL('hud_atlas.png', this.base)),
        ]);
        this.meta = meta; this.img = img; this.ready = true;
        return meta;
    }
    _loadImg(u) {
        return new Promise((res, rej) => {
            const im = new Image();
            im.onload = () => res(im); im.onerror = () => rej('img load');
            im.src = u.href || u;
        });
    }

    // Create the 2D overlay canvas layered EXACTLY over the host (WebGPU body) canvas.
    attach(host) {
        if (this._canvas) return this._ctx;
        this._host = host;
        const c = document.createElement('canvas');
        c.width = host.width; c.height = host.height;
        c.style.cssText = host.style.cssText;
        c.style.background = 'transparent';
        c.style.position = 'absolute';
        c.style.pointerEvents = 'none';
        host.style.position = 'relative';
        host.parentElement.style.position = 'relative';
        host.parentElement.appendChild(c);
        c.style.left = host.offsetLeft + 'px';
        c.style.top = host.offsetTop + 'px';
        this._canvas = c; this._ctx = c.getContext('2d');
        return this._ctx;
    }

    // ── digit rendering ─────────────────────────────────────────────────────
    // Draw one real boxed-digit sprite from the atlas, scaled into a dh-tall box.
    _glyph(ctx, ch, x, y, dh, color) {
        if (this.ready && this.meta && /[0-9]/.test(ch)) {
            const r = this.meta.rects['digit_' + ch];
            if (r) {
                const dw = Math.round(dh * r.w / r.h);
                // tint: draw the white-keyed glyph, then multiply by color
                ctx.save();
                ctx.drawImage(this.img, r.x, r.y, r.w, r.h, x, y, dw, dh);
                if (color && color !== '#ffffff') {
                    ctx.globalCompositeOperation = 'source-atop';
                    ctx.fillStyle = color; ctx.fillRect(x, y, dw, dh);
                }
                ctx.restore();
                return dw;
            }
        }
        return this._vecDigit(ctx, ch, x, y, dh, color);   // fallback
    }
    // Real-art digit string. align: 'left'|'right'|'center'. Returns total width.
    _digits(ctx, str, x, y, dh, align, color) {
        const adv = (this.ready ? Math.round(dh * 0.78) : Math.round(dh * 0.80));
        const dw = adv;
        const total = str.length * adv;
        let cx = align === 'right' ? x - total : (align === 'center' ? x - total / 2 : x);
        for (const ch of str) { this._glyph(ctx, ch, cx, y, dh, color); cx += adv; }
        return total;
    }
    // Vector 7-seg fallback (only used before the atlas finishes loading).
    _vecDigit(ctx, ch, x, y, h, color) {
        const SEG = { '0':'abcdef','1':'bc','2':'abged','3':'abgcd','4':'fgbc','5':'afgcd','6':'afgcde','7':'abc','8':'abcdefg','9':'abcfgd' };
        const segs = SEG[ch]; const w = Math.round(h * 0.62); if (!segs) return w;
        const t = Math.max(2, Math.round(w * 0.16)), midY = y + h / 2;
        ctx.fillStyle = color || '#fff';
        const seg = { a:[x+t,y,w-2*t,t], b:[x+w-t,y+t,t,h/2-1.5*t], c:[x+w-t,midY+0.5*t,t,h/2-1.5*t],
            d:[x+t,y+h-t,w-2*t,t], e:[x,midY+0.5*t,t,h/2-1.5*t], f:[x,y+t,t,h/2-1.5*t], g:[x+t,midY-t/2,w-2*t,t] };
        for (const s of segs) { const r = seg[s]; ctx.fillRect(r[0], r[1], r[2], r[3]); }
        return w;
    }

    // ── procedural life bar (white-tex * vertex color, loc_8c15FFB0) ────────
    // Angled parallelogram swept toward center. HP depletes toward center; the bright
    // HP anchors at the OUTER (portrait) end; dark-red chip trails behind it.
    _lifeBar(ctx, ox, oy, len, h, skew, mirror, hpFrac, chipFrac, hpCol) {
        const dir = mirror ? -1 : 1;
        const X = lx => ox + dir * lx, Xb = lx => ox + dir * (lx + skew);
        const yT = oy, yB = oy + h;
        const outline = () => { ctx.beginPath(); ctx.moveTo(X(0), yT); ctx.lineTo(X(len), yT);
            ctx.lineTo(Xb(len), yB); ctx.lineTo(Xb(0), yB); ctx.closePath(); };
        outline(); ctx.fillStyle = 'rgba(0,0,0,0.55)'; ctx.fill();
        outline(); ctx.save(); ctx.clip();
        const sub = (frac, col) => {
            frac = Math.max(0, Math.min(1, frac)); if (frac <= 0) return;
            const fl = len * frac;
            ctx.beginPath(); ctx.moveTo(X(0), yT); ctx.lineTo(X(fl), yT);
            ctx.lineTo(Xb(fl), yB); ctx.lineTo(Xb(0), yB); ctx.closePath();
            ctx.fillStyle = col; ctx.fill();
        };
        sub(chipFrac, CHIP_COL);              // chip trail BEHIND
        sub(hpFrac, hpCol);                   // bright HP ON TOP
        ctx.restore();
        outline(); ctx.lineWidth = 1.2; ctx.strokeStyle = 'rgba(255,255,255,0.9)'; ctx.stroke();
    }

    // ── super meter (sheared, fill from outer corner inward) ────────────────
    _meter(ctx, ox, oy, len, h, skew, mirror, fillFrac, col) {
        const dir = mirror ? -1 : 1;
        const tri = (l0, l1, c) => { ctx.beginPath();
            ctx.moveTo(ox + dir * l0, oy); ctx.lineTo(ox + dir * l1, oy);
            ctx.lineTo(ox + dir * (l1 + skew), oy + h); ctx.lineTo(ox + dir * (l0 + skew), oy + h);
            ctx.closePath(); ctx.fillStyle = c; ctx.fill(); };
        tri(0, len, 'rgba(0,0,0,0.55)');
        const f = Math.max(0, Math.min(1, fillFrac));
        if (f > 0) tri(0, len * f, col);
        ctx.beginPath(); ctx.moveTo(ox, oy); ctx.lineTo(ox + dir * len, oy);
        ctx.lineTo(ox + dir * (len + skew), oy + h); ctx.lineTo(ox + dir * skew, oy + h);
        ctx.closePath(); ctx.lineWidth = 1; ctx.strokeStyle = 'rgba(255,255,255,0.7)'; ctx.stroke();
    }

    // Active point char of a side = first of its 3 slots with active!=0. colIdx picks team color.
    _pointSlot(rdU8, charSlots, slots) {
        for (let i = 0; i < slots.length; i++) {
            const base = charSlots[slots[i]];
            if (rdU8(base + OFF_ACTIVE)) return { base, colIdx: i };
        }
        return null;
    }

    // ── the frame ───────────────────────────────────────────────────────────
    // rdU8/rdU16: RAM accessors from replay.html. charSlots: the 6 char-struct bases.
    render(rdU8, rdU16, charSlots) {
        if (!this._canvas) return;
        const ctx = this._ctx, W = this._canvas.width, H = this._canvas.height;
        ctx.clearRect(0, 0, W, H);
        if (!rdU8(A.IN_MATCH)) return;
        ctx.save();
        ctx.scale(W / 640, H / 480);
        ctx.imageSmoothingEnabled = false;

        const p1 = this._pointSlot(rdU8, charSlots, P1_SLOTS);
        const p2 = this._pointSlot(rdU8, charSlots, P2_SLOTS);
        const c1 = BAR_COLS[p1 ? p1.colIdx : 0], c2 = BAR_COLS[p2 ? p2.colIdx : 0];
        const hp  = sl => sl ? Math.max(0, Math.min(1, rdU8(sl.base + OFF_HP) / HP_MAX)) : 0;
        const red = sl => sl ? Math.max(0, Math.min(1, rdU8(sl.base + OFF_RED) / HP_MAX)) : 0;

        // life bars (P1 top-left / P2 top-right, swept toward center)
        const LB_OY = 18, LB_H = 15, LB_LEN = 212, LB_SKEW = 26, P1_OUT = 80, P2_OUT = 560;
        this._lifeBar(ctx, P1_OUT, LB_OY, LB_LEN, LB_H, LB_SKEW, false, hp(p1), red(p1), c1[0]);
        this._lifeBar(ctx, P2_OUT, LB_OY, LB_LEN, LB_H, LB_SKEW, true,  hp(p2), red(p2), c2[0]);

        // portrait/name plate (char_id digits, real font) at each OUTER end
        const plate = (px, mirror, sl, col) => {
            const pw = 56, ph = 20, x = mirror ? px - pw : px;
            ctx.fillStyle = 'rgba(0,0,0,0.6)'; ctx.fillRect(x, LB_OY - 2, pw, ph);
            ctx.strokeStyle = col; ctx.lineWidth = 1; ctx.strokeRect(x + 0.5, LB_OY - 1.5, pw - 1, ph - 1);
            const cid = sl ? rdU8(sl.base + OFF_CID) : 0;
            const id = String(cid).padStart(2, '0');
            this._digits(ctx, id, mirror ? x + pw - 4 : x + 4, LB_OY + 1, 15, mirror ? 'right' : 'left', col);
        };
        plate(18, false, p1, c1[0]);
        plate(622, true, p2, c2[0]);

        // round / win-star pips (round_counter)
        const stars = Math.max(0, Math.min(3, rdU8(A.ROUND) | 0));
        const star = (sx, mirror) => { ctx.fillStyle = '#ffe14d';
            for (let i = 0; i < stars; i++) {
                const cx = mirror ? sx - i * 11 : sx + i * 11, cy = LB_OY + LB_H + 6, r = 3.5;
                ctx.beginPath();
                for (let k = 0; k < 5; k++) { const a0 = -Math.PI/2 + k*2*Math.PI/5, a1 = a0 + Math.PI/5;
                    ctx.lineTo(cx + Math.cos(a0)*r, cy + Math.sin(a0)*r);
                    ctx.lineTo(cx + Math.cos(a1)*r*0.45, cy + Math.sin(a1)*r*0.45); }
                ctx.closePath(); ctx.fill(); } };
        star(20, false); star(620, true);

        // bottom super meters + level digits
        const MT_OY = 458, MT_H = 9, MT_LEN = 200, MT_SKEW = 16;
        this._meter(ctx, 20, MT_OY, MT_LEN, MT_H, MT_SKEW, false, rdU16(A.P1_FILL) / METER_MAX, c1[0]);
        this._meter(ctx, 620, MT_OY, MT_LEN, MT_H, MT_SKEW, true,  rdU16(A.P2_FILL) / METER_MAX, c2[0]);
        const lvl = (ox, mirror, lv) => {
            ctx.fillStyle = '#ffd24d';
            for (let i = 0; i < (lv || 0); i++)
                ctx.fillRect(ox + (mirror ? -1 : 1) * (i * 14) - (mirror ? 9 : 0), MT_OY - 11, 9, 6);
            this._digits(ctx, String(lv | 0), mirror ? ox - MT_LEN - 4 : ox + MT_LEN + 4, MT_OY - 14, 13,
                mirror ? 'right' : 'left', '#ffffff');
        };
        lvl(22, false, rdU8(A.P1_LVL)); lvl(618, true, rdU8(A.P2_LVL));

        // center timer (real boxed digits)
        const t = String(Math.max(0, Math.min(99, rdU8(A.TIMER) | 0))).padStart(2, '0');
        ctx.fillStyle = 'rgba(0,0,0,0.45)'; ctx.fillRect(302, 4, 36, 30);
        this._digits(ctx, t, 320, 6, 26, 'center', '#ffffff');

        // combo counters (combo > 1)
        const p1c = rdU16(A.P1_COMBO), p2c = rdU16(A.P2_COMBO);
        if (p1c > 1) this._digits(ctx, String(p1c), 24,  44, 18, 'left',  '#ffe14d');
        if (p2c > 1) this._digits(ctx, String(p2c), 616, 44, 18, 'right', '#ffe14d');

        ctx.restore();
    }
}
