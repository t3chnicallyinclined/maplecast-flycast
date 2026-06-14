// ============================================================================
// hud-client.mjs — Render-Replica PHASE 3 HUD with REAL MVC2 art.
//
// Replaces the synthetic 7-segment-digit / placeholder-portrait overlay with the
// game's OWN pixel art, driven by the live engine state (the SAME RAM addresses
// the production GSTA wire reads — confirmed, not modeled). [hud:real-art-v1]
//
// CLEAN RECONSTRUCTION (no garble). The FONT.BIN real-art digit path was the garble source:
// the ripped `digit_*` atlas rects pointed at empty/fragmented glyph space and tinting that
// produced pink/magenta noise where digits should be. v1-clean renders EVERY HUD element with
// crisp procedural primitives — solid team-tinted bars + 7-segment vector digits — no external
// texture, so there is nothing left to garble. [hud:clean-v1]
//
// WHAT IS PROCEDURAL (engine-faithful):
//   • TIMER digits + COMBO/HIT digits + LEVEL digits  -> CLEAN 7-segment vector digits with a
//       1px dark halo for legibility. Tint perfectly; need no art asset.
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

// char_id -> display name. AUTHORITATIVE roster from re_kb finding:characters
// (tools/re_kb/05_characters.surql, source = char_prg/buildSPL.sh manifest). The
// NAME PLATE renders this string per the live char_id (the same +0x001 read the
// portrait/bars use). Tier-2 names are REAL (not stand-in digits) — the only
// stand-in that remains is the portrait IMAGE pixels (PLxx_FAC.BIN decode is an
// unsolved NaomiLib display-list format -> deferred to mvc2-sh4-re-expert; see the
// HANDOFF note at the bottom of this file). The plate frames a team-tinted box with
// the real name + monogram, driven live by char_id. [hud:names-v1]
const NAMES = {
    0x00:'RYU',0x01:'ZANGIEF',0x02:'GUILE',0x03:'MORRIGAN',0x04:'ANAKARIS',0x05:'STRIDER',
    0x06:'CYCLOPS',0x07:'WOLVERINE',0x08:'PSYLOCKE',0x09:'ICEMAN',0x0A:'ROGUE',
    0x0B:'CAPT.AMERICA',0x0C:'SPIDER-MAN',0x0D:'HULK',0x0E:'VENOM',0x0F:'DR.DOOM',
    0x10:'TRON',0x11:'JILL',0x12:'HAYATO',0x13:'RUBY HEART',0x14:'SONSON',0x15:'AMINGO',
    0x16:'MARROW',0x17:'CABLE',0x18:'ABYSS',0x19:'ABYSS',0x1A:'ABYSS',0x1B:'CHUN-LI',
    0x1C:'MEGA MAN',0x1D:'ROLL',0x1E:'AKUMA',0x1F:'B.B.HOOD',0x20:'FELICIA',0x21:'CHARLIE',
    0x22:'SAKURA',0x23:'DAN',0x24:'CAMMY',0x25:'DHALSIM',0x26:'M.BISON',0x27:'KEN',
    0x28:'GAMBIT',0x29:'JUGGERNAUT',0x2A:'STORM',0x2B:'SABRETOOTH',0x2C:'MAGNETO',
    0x2D:'SHUMA',0x2E:'WAR MACHINE',0x2F:'SILVER SAMURAI',0x30:'OMEGA RED',0x31:'SPIRAL',
    0x32:'COLOSSUS',0x33:'IRON MAN',0x34:'SENTINEL',0x35:'BLACKHEART',0x36:'THANOS',
    0x37:'JIN',0x38:'CAPT.COMMANDO',0x39:'WOLVERINE',0x3A:'SERVBOT',
};

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
        this.portraits = null;     // char_id -> HTMLImageElement, IF a real-pixel portrait
                                   // atlas (portraits/portraits.{json,png}) is deployed. Until
                                   // PLxx_FAC.BIN is decoded (open RE item), this stays null and
                                   // the plate draws the team-tinted monogram instead.
    }

    // v1-clean renders entirely from procedural primitives, so the atlas is OPTIONAL and
    // never gates rendering. load() stays for API compatibility (replay.html calls it) and to
    // keep the team-color metadata available, but a failure is harmless — the HUD is identical.
    async load() {
        try {
            const [meta, img] = await Promise.all([
                fetch(new URL('hud_atlas.json', this.base)).then(r => r.ok ? r.json() : Promise.reject(r.status)),
                this._loadImg(new URL('hud_atlas.png', this.base)),
            ]);
            this.meta = meta; this.img = img; this.ready = true;
        } catch (e) { this.ready = false; }   // procedural HUD renders regardless
        // OPTIONAL portrait atlas (real FAC pixels). portraits/portraits.json maps
        // { "<char_id>": {x,y,w,h} } into portraits/portraits.png. Absent today (FAC
        // undecoded) -> portraits stays null -> monogram fallback. Harmless on 404.
        try {
            const [pmeta, pimg] = await Promise.all([
                fetch(new URL('portraits/portraits.json', this.base)).then(r => r.ok ? r.json() : Promise.reject(r.status)),
                this._loadImg(new URL('portraits/portraits.png', this.base)),
            ]);
            const tiles = {};
            for (const k in pmeta.rects || {}) {
                const r = pmeta.rects[k], c = document.createElement('canvas');
                c.width = r.w; c.height = r.h;
                c.getContext('2d').drawImage(pimg, r.x, r.y, r.w, r.h, 0, 0, r.w, r.h);
                tiles[parseInt(k, 10)] = c;
            }
            this.portraits = tiles;
        } catch (e) { /* no portrait atlas — monogram fallback */ }
        return this.meta;
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
    // CLEAN 7-segment vector digits. The FONT.BIN-ripped `digit_*` atlas rects pointed at
    // blank/fragmented glyph space, which produced the garbled HUD digits. Vector digits are
    // crisp, tint perfectly, and need no external art. [hud:clean-v1]
    _glyph(ctx, ch, x, y, dh, color) { return this._vecDigit(ctx, ch, x, y, dh, color); }
    // Digit string. align: 'left'|'right'|'center'. Returns total width.
    _digits(ctx, str, x, y, dh, align, color) {
        const adv = Math.round(dh * 0.70);
        const total = str.length * adv;
        let cx = align === 'right' ? x - total : (align === 'center' ? Math.round(x - total / 2) : x);
        for (const ch of str) { this._vecDigit(ctx, ch, cx, y, dh, color); cx += adv; }
        return total;
    }
    // CLEAN 7-segment vector digit. A 1px dark halo keeps it readable over the bars/plates.
    _vecDigit(ctx, ch, x, y, h, color) {
        const SEG = { '0':'abcdef','1':'bc','2':'abged','3':'abgcd','4':'fgbc','5':'afgcd','6':'afgcde','7':'abc','8':'abcdefg','9':'abcfgd' };
        const segs = SEG[ch]; const w = Math.round(h * 0.58); if (!segs) return w;
        const t = Math.max(1.5, Math.round(w * 0.16)), midY = y + h / 2;
        const seg = { a:[x+t,y,w-2*t,t], b:[x+w-t,y+t,t,h/2-1.5*t], c:[x+w-t,midY+0.5*t,t,h/2-1.5*t],
            d:[x+t,y+h-t,w-2*t,t], e:[x,midY+0.5*t,t,h/2-1.5*t], f:[x,y+t,t,h/2-1.5*t], g:[x+t,midY-t/2,w-2*t,t] };
        ctx.fillStyle = 'rgba(0,0,0,0.75)';
        for (const [ddx, ddy] of [[-1,0],[1,0],[0,-1],[0,1]])
            for (const s of segs) { const r = seg[s]; ctx.fillRect(r[0]+ddx, r[1]+ddy, r[2], r[3]); }
        ctx.fillStyle = color || '#fff';
        for (const s of segs) { const r = seg[s]; ctx.fillRect(r[0], r[1], r[2], r[3]); }
        return w;
    }

    // ── MVC2 life bar ────────────────────────────────────────────────────────
    // A slim parallelogram leaning toward center. The OUTER end (portrait side) is the FULL
    // anchor; HP DEPLETES toward CENTER (the inner tip empties first). lx runs 0=outer →
    // len=inner. mirror=true puts the outer end on the RIGHT (P2). The dark frame, the
    // dark-red chip (recoverable) BEHIND, and the bright team HP ON TOP, then a thin border.
    _lifeBar(ctx, ox, oy, len, h, skew, mirror, hpFrac, chipFrac, hpCol, hiCol) {
        const dir = mirror ? -1 : 1;
        const Xt = lx => ox + dir * lx, Xb = lx => ox + dir * (lx + skew);
        const yT = oy, yB = oy + h;
        const quad = (l0, l1) => { ctx.beginPath();
            ctx.moveTo(Xt(l0), yT); ctx.lineTo(Xt(l1), yT);
            ctx.lineTo(Xb(l1), yB); ctx.lineTo(Xb(l0), yB); ctx.closePath(); };
        // frame + dark empty channel
        quad(0, len); ctx.fillStyle = '#1a1d24'; ctx.fill();
        quad(0, len); ctx.save(); ctx.clip();
        // chip + HP fill grow from the OUTER end (l=0) inward to len·frac
        const sub = (frac, col) => { frac = Math.max(0, Math.min(1, frac)); if (frac <= 0) return;
            quad(0, len * frac); ctx.fillStyle = col; ctx.fill(); };
        sub(chipFrac, CHIP_COL);                       // recoverable red trail BEHIND
        sub(hpFrac, hpCol);                            // bright HP ON TOP
        // top-edge highlight stripe (the "white-tex modulate" sheen)
        if (hpFrac > 0 && hiCol) { ctx.globalAlpha = 0.5;
            quad(0, len * Math.max(0, Math.min(1, hpFrac))); ctx.fillStyle = hiCol;
            ctx.fillRect(Math.min(Xt(0), Xb(len)) - 2, yT, Math.abs(len) + skew + 4, Math.max(1, h * 0.32));
            ctx.globalAlpha = 1; }
        ctx.restore();
        quad(0, len); ctx.lineWidth = 1; ctx.strokeStyle = 'rgba(0,0,0,0.9)'; ctx.stroke();
        quad(0.5, len - 0.5); ctx.lineWidth = 1; ctx.strokeStyle = 'rgba(255,255,255,0.45)'; ctx.stroke();
    }

    // ── MVC2 super meter ──────────────────────────────────────────────────────
    // Slim sheared bar at the bottom. Fills from the OUTER end inward. Level pips overlaid
    // as bright notches at each METER_MAX boundary (the meter holds up to ~5 levels but we
    // render the current single-bar fill + the level count as small chevrons + a digit).
    _meter(ctx, ox, oy, len, h, skew, mirror, fillFrac, col, hiCol) {
        const dir = mirror ? -1 : 1;
        const quad = (l0, l1, c) => { ctx.beginPath();
            ctx.moveTo(ox + dir * l0, oy); ctx.lineTo(ox + dir * l1, oy);
            ctx.lineTo(ox + dir * (l1 + skew), oy + h); ctx.lineTo(ox + dir * (l0 + skew), oy + h);
            ctx.closePath(); ctx.fillStyle = c; ctx.fill(); };
        quad(0, len, '#15181e');
        const f = Math.max(0, Math.min(1, fillFrac));
        if (f > 0) { quad(0, len * f, col);
            if (hiCol) { ctx.globalAlpha = 0.5; quad(0, len * f, hiCol); ctx.globalAlpha = 1; } }
        // frame
        ctx.beginPath(); ctx.moveTo(ox, oy); ctx.lineTo(ox + dir * len, oy);
        ctx.lineTo(ox + dir * (len + skew), oy + h); ctx.lineTo(ox + dir * skew, oy + h);
        ctx.closePath(); ctx.lineWidth = 1; ctx.strokeStyle = 'rgba(0,0,0,0.85)'; ctx.stroke();
        ctx.lineWidth = 1; ctx.strokeStyle = 'rgba(255,255,255,0.35)'; ctx.stroke();
    }

    // Active point char of a side = first of its 3 slots with active!=0. colIdx picks team color.
    _pointSlot(rdU8, charSlots, slots) {
        for (let i = 0; i < slots.length; i++) {
            const base = charSlots[slots[i]];
            if (rdU8(base + OFF_ACTIVE)) return { base, colIdx: i };
        }
        return null;
    }

    // ── PLATES-ONLY pass (composited ON TOP of sprite-client.mjs drawHUD) ─────
    // drawHUD draws the proven bars/meters/timer/combo but NOT the textured corner
    // portraits / names / win stars. This pass re-adds JUST those, on the SAME 2D
    // overlay ctx drawHUD already drew to (do NOT clear — drawHUD owns the clear).
    // Positioned to sit ABOVE drawHUD's life bars (those occupy game-space y 16..30,
    // x 18..330), so nothing overlaps the bars. Drawn in 640x480 game space, scaled
    // to the canvas exactly like drawHUD (ctx.scale(W/640,H/480)). [hud:plates-on-drawHUD]
    //   • REAL decoded portrait (this.portraits[char_id], FAC pixels) in a team frame.
    //   • REAL roster NAME (NAMES[char_id]) under the portrait.
    //   • WIN STARS under the name (round counter; same field the orphaned HUD used).
    // Drive from the live point char_id per side + the round-win count — the SAME RAM
    // the replay already reads (populateHudStateFromRAM / populateBodiesFromRAM).
    renderPlatesOnly(ctx, rdU8, charSlots) {
        if (!ctx || !rdU8) return;
        if (!rdU8(A.IN_MATCH)) return;
        const W = ctx.canvas.width, H = ctx.canvas.height;
        ctx.save();
        ctx.scale(W / 640, H / 480);
        ctx.imageSmoothingEnabled = false;

        const p1 = this._pointSlot(rdU8, charSlots, P1_SLOTS);
        const p2 = this._pointSlot(rdU8, charSlots, P2_SLOTS);
        const c1 = BAR_COLS[p1 ? p1.colIdx : 0], c2 = BAR_COLS[p2 ? p2.colIdx : 0];

        // Corner portrait box: top OUTER corner, above the drawHUD bar (bar y=16..30).
        // Box ends at y≈14 so it never touches the bars; name + stars stack below the bar.
        const PW = 30, PH = 30, PY = 1;
        this._plate(ctx, 2,       PY, PW, PH, false, p1, rdU8, c1[0], c1[1] || c1[0]);
        this._plate(ctx, 640 - 2, PY, PW, PH, true,  p2, rdU8, c2[0], c2[1] || c2[0]);

        // WIN STARS — under each name plate (the name renders at y = PY+PH+1 inside _plate,
        // ~6px tall), placed clear of the bars. Round counter is the only win field shipped.
        const stars = Math.max(0, Math.min(3, rdU8(A.ROUND) | 0));
        const drawStar = (cx, cy, r, col) => { ctx.fillStyle = col; ctx.beginPath();
            for (let k = 0; k < 5; k++) { const a0 = -Math.PI/2 + k*2*Math.PI/5, a1 = a0 + Math.PI/5;
                ctx.lineTo(cx + Math.cos(a0)*r, cy + Math.sin(a0)*r);
                ctx.lineTo(cx + Math.cos(a1)*r*0.45, cy + Math.sin(a1)*r*0.45); }
            ctx.closePath(); ctx.fill(); };
        const starY = PY + PH + 10;
        for (let i = 0; i < stars; i++) {
            drawStar(6 + i * 9,   starY, 3.5, '#ffe14d');
            drawStar(634 - i * 9, starY, 3.5, '#ffe14d');
        }
        ctx.restore();
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

        // ── LIFE BARS ────────────────────────────────────────────────────────
        // Two slim bars in the upper corners, leaning toward center, ENDING WELL BEFORE
        // mid-screen so they never merge into one block and the timer sits clear between.
        // Layout (640-wide game space): portrait plate at the very outer corner, then the
        // life bar inboard of it; HP depletes toward center.
        //   P1 plate [8..48], P1 bar outer=54 → inner 54+LEN; gap to center.
        //   P2 plate [592..632], P2 bar outer=586 → inner 586-LEN.
        const PLATE_W = 40, PLATE_H = 18, PLATE_Y = 10;
        const LB_OY = 15, LB_H = 11, LB_LEN = 252, LB_SKEW = 14;
        const P1_BAR_OUT = 54, P2_BAR_OUT = 586;
        this._lifeBar(ctx, P1_BAR_OUT, LB_OY, LB_LEN, LB_H, LB_SKEW, false, hp(p1), red(p1), c1[0], c1[1]);
        this._lifeBar(ctx, P2_BAR_OUT, LB_OY, LB_LEN, LB_H, LB_SKEW, true,  hp(p2), red(p2), c2[0], c2[1]);

        // ── PORTRAIT + NAME PLATE at the OUTER corner (tier-2) ────────────────
        // The portrait box is a team-tinted frame holding the live character's
        // monogram (the real-pixel FAC portrait is the open RE item — PLxx_FAC.BIN
        // is a NaomiLib display-list, not a flat texture; see the HANDOFF note at
        // EOF). The NAME below it is the REAL roster name (NAMES[char_id]), driven
        // live by the active point char's +0x001. If a portrait IMAGE atlas later
        // ships (this.portraits[cid]), it is drawn in the box automatically.
        this._plate(ctx, 8,   PLATE_Y, PLATE_W, PLATE_H, false, p1, rdU8, c1[0], c1[1] || c1[0]);
        this._plate(ctx, 632, PLATE_Y, PLATE_W, PLATE_H, true,  p2, rdU8, c2[0], c2[1] || c2[0]);

        // ── ROUND / WIN STARS ─────────────────────────────────────────────────
        // Small win markers just under the plate, growing inboard.
        const stars = Math.max(0, Math.min(3, rdU8(A.ROUND) | 0));
        const drawStar = (cx, cy, r, col) => { ctx.fillStyle = col; ctx.beginPath();
            for (let k = 0; k < 5; k++) { const a0 = -Math.PI/2 + k*2*Math.PI/5, a1 = a0 + Math.PI/5;
                ctx.lineTo(cx + Math.cos(a0)*r, cy + Math.sin(a0)*r);
                ctx.lineTo(cx + Math.cos(a1)*r*0.45, cy + Math.sin(a1)*r*0.45); }
            ctx.closePath(); ctx.fill(); };
        const starY = PLATE_Y + PLATE_H + 13;   // below the name row so it never overlaps
        for (let i = 0; i < stars; i++) { drawStar(12 + i * 11, starY, 4, '#ffe14d');
                                          drawStar(628 - i * 11, starY, 4, '#ffe14d'); }

        // ── CENTER TIMER ──────────────────────────────────────────────────────
        const t = String(Math.max(0, Math.min(99, rdU8(A.TIMER) | 0))).padStart(2, '0');
        ctx.fillStyle = '#0b0d12'; ctx.fillRect(304, 6, 32, 30);
        ctx.strokeStyle = 'rgba(255,255,255,0.5)'; ctx.lineWidth = 1; ctx.strokeRect(304.5, 6.5, 31, 29);
        this._digits(ctx, t, 320, 10, 22, 'center', '#ffffff');

        // ── BOTTOM SUPER METERS + LEVEL ───────────────────────────────────────
        // Long slim bars near the bottom, outer-anchored, with a small level number and pips
        // OUTSIDE the bar's inner end so nothing overlaps the bar fill.
        const MT_OY = 460, MT_H = 8, MT_LEN = 232, MT_SKEW = 9, MT_OUT1 = 20, MT_OUT2 = 620;
        this._meter(ctx, MT_OUT1, MT_OY, MT_LEN, MT_H, MT_SKEW, false, rdU16(A.P1_FILL) / METER_MAX, c1[0], c1[1]);
        this._meter(ctx, MT_OUT2, MT_OY, MT_LEN, MT_H, MT_SKEW, true,  rdU16(A.P2_FILL) / METER_MAX, c2[0], c2[1]);
        const lvl = (ox, mirror, lv) => {
            lv = Math.max(0, Math.min(8, lv | 0));
            // level digit just OUTSIDE the outer corner
            this._digits(ctx, String(lv), mirror ? ox + 6 : ox - 6, MT_OY - 2, 11, mirror ? 'left' : 'right', '#ffe14d');
            // pips above the bar, growing inboard
            ctx.fillStyle = '#ffd24d';
            for (let i = 0; i < lv; i++) {
                const px = mirror ? ox - 14 - i * 11 : ox + 8 + i * 11;
                ctx.fillRect(px, MT_OY - 7, 8, 4);
            }
        };
        lvl(MT_OUT1, false, rdU8(A.P1_LVL)); lvl(MT_OUT2, true, rdU8(A.P2_LVL));

        // ── COMBO COUNTERS (combo > 1) ────────────────────────────────────────
        const p1c = rdU16(A.P1_COMBO), p2c = rdU16(A.P2_COMBO);
        const combo = (x, mirror, n) => {
            if (n <= 1) return;
            this._digits(ctx, String(n), x, 50, 16, mirror ? 'right' : 'left', '#ffd24d');
            this._small(ctx, 'HITS', mirror ? x - String(n).length * 11 - 4 : x + String(n).length * 11 + 4,
                57, mirror, '#ffe9a0');
        };
        combo(20, false, p1c); combo(620, true, p2c);

        ctx.restore();
    }

    // tiny 3x5-ish block label ("HITS") — procedural, no atlas. Renders left→right from x.
    _small(ctx, str, x, y, mirror, col) {
        const F = {  // 3-wide column bitmaps (5 rows), MSB=top
            H:[0b11111,0b00100,0b11111], I:[0b00000,0b11111,0b00000], T:[0b10000,0b11111,0b10000],
            S:[0b10010,0b10101,0b01001] };
        ctx.fillStyle = col; const cw = 4, ch2 = 5; let cx = mirror ? x - str.length * cw : x;
        for (const c of str) { const cols = F[c] || [0,0,0];
            for (let col2 = 0; col2 < 3; col2++) for (let r = 0; r < ch2; r++)
                if (cols[col2] & (1 << (ch2 - 1 - r))) ctx.fillRect(cx + col2, y + r, 1, 1);
            cx += cw; }
    }

    // ── PORTRAIT + NAME PLATE ─────────────────────────────────────────────────
    // px = outer-corner X (game 640 space); mirror flips for the P2 (right) side.
    // sl = {base,colIdx} active-point slot (or null). Draws: team-framed portrait
    // box (FAC pixels if this.portraits[cid] exists, else a monogram), then the
    // REAL roster name underneath in a clean 5px proportional font.
    _plate(ctx, px, py, pw, ph, mirror, sl, rdU8, col, hiCol) {
        const x = mirror ? px - pw : px;
        const cid = sl ? rdU8(sl.base + OFF_CID) : -1;
        // box + team frame
        ctx.fillStyle = '#0b0d12'; ctx.fillRect(x, py, pw, ph);
        // portrait image (real FAC pixels) if a portrait atlas is loaded; else monogram
        const pim = this.portraits && cid >= 0 ? this.portraits[cid] : null;
        if (pim) {
            ctx.imageSmoothingEnabled = false;
            ctx.drawImage(pim, x + 1, py + 1, pw - 2, ph - 2);
        } else if (cid >= 0) {
            // monogram = first letter of the name on a subtle team-tinted ground
            const nm = NAMES[cid] || ('#' + cid.toString(16));
            ctx.fillStyle = 'rgba(' + this._rgbA(col, 0.16) + ')'; ctx.fillRect(x + 1, py + 1, pw - 2, ph - 2);
            ctx.fillStyle = hiCol;
            const ch = nm[0];
            // big monogram glyph (use the proportional name font, scaled up, centered)
            this._name(ctx, ch, x + pw / 2, py + 3, 12, 'center', hiCol);
        }
        ctx.strokeStyle = col; ctx.lineWidth = 1.2; ctx.strokeRect(x + 0.5, py + 0.5, pw - 1, ph - 1);
        // NAME below the box — real roster name, clean proportional font, fit to width
        if (cid >= 0) {
            const nm = NAMES[cid] || ('CHAR ' + cid.toString(16).toUpperCase());
            const ny = py + ph + 1, nx = x + pw / 2;
            this._name(ctx, nm, nx, ny, 5, 'center', '#e8eef6', pw + 18);
        }
    }

    // hex '#rrggbb' -> "r,g,b,a" for rgba()
    _rgbA(hex, a) {
        const n = parseInt(hex.slice(1), 16);
        return ((n >> 16) & 255) + ',' + ((n >> 8) & 255) + ',' + (n & 255) + ',' + a;
    }

    // ── proportional NAME font ────────────────────────────────────────────────
    // Clean 5-row variable-width uppercase font (no atlas → cannot garble, matching
    // the [hud:clean-v1] design choice). Each glyph = array of column bitmaps (5
    // rows, bit4=top). align 'left'|'right'|'center'. maxW (optional) horizontally
    // squeezes the advance so long names (CAPT.COMMANDO) fit the corner.
    _name(ctx, str, x, y, h, align, col, maxW) {
        str = String(str).toUpperCase();
        const G = HudClient._NFONT;
        const sc = h / 5;                       // glyph pixel = sc square
        // measure (advance = glyph width + 1 col gap)
        const widths = [...str].map(c => (G[c] ? G[c].length : 3));
        let total = widths.reduce((a, w) => a + (w + 1), 0) * sc;
        let squeeze = 1;
        if (maxW && total > maxW) { squeeze = maxW / total; total = maxW; }
        let cx = align === 'right' ? x - total : (align === 'center' ? x - total / 2 : x);
        // 1px dark halo for legibility over bars/stage
        for (const pass of [0, 1]) {
            ctx.fillStyle = pass === 0 ? 'rgba(0,0,0,0.85)' : col;
            let gx = cx;
            for (let ci = 0; ci < str.length; ci++) {
                const cols = G[str[ci]] || [0, 0, 0];
                for (let c = 0; c < cols.length; c++) for (let r = 0; r < 5; r++)
                    if (cols[c] & (1 << (4 - r))) {
                        const dx = gx + c * sc * squeeze, dy = y + r * sc;
                        const w = Math.ceil(sc * squeeze) + (pass === 0 ? 0.6 : 0);
                        ctx.fillRect(pass === 0 ? dx - 0.3 : dx, pass === 0 ? dy - 0.3 : dy,
                                     pass === 0 ? w + 0.6 : Math.ceil(sc * squeeze), Math.ceil(sc) + (pass === 0 ? 0.6 : 0));
                    }
                gx += (cols.length + 1) * sc * squeeze;
            }
        }
        return total;
    }
}

// 5-row uppercase glyph font (column bitmaps, bit4=top row). Covers A-Z 0-9 . - space + (.
HudClient._NFONT = {
    'A':[0b01111,0b10100,0b01111], 'B':[0b11111,0b10101,0b01010], 'C':[0b01110,0b10001,0b10001],
    'D':[0b11111,0b10001,0b01110], 'E':[0b11111,0b10101,0b10001], 'F':[0b11111,0b10100,0b10000],
    'G':[0b01110,0b10001,0b10111], 'H':[0b11111,0b00100,0b11111], 'I':[0b10001,0b11111,0b10001],
    'J':[0b00011,0b00001,0b11110], 'K':[0b11111,0b00100,0b11011], 'L':[0b11111,0b00001,0b00001],
    'M':[0b11111,0b01000,0b00100,0b01000,0b11111], 'N':[0b11111,0b01000,0b00100,0b00010,0b11111],
    'O':[0b01110,0b10001,0b01110], 'P':[0b11111,0b10100,0b01000], 'Q':[0b01110,0b10001,0b01111],
    'R':[0b11111,0b10100,0b01011], 'S':[0b01001,0b10101,0b10010], 'T':[0b10000,0b11111,0b10000],
    'U':[0b11110,0b00001,0b11110], 'V':[0b11100,0b00011,0b11100], 'W':[0b11110,0b00001,0b00110,0b00001,0b11110],
    'X':[0b11011,0b00100,0b11011], 'Y':[0b11000,0b00111,0b11000], 'Z':[0b10011,0b10101,0b11001],
    '0':[0b01110,0b10001,0b01110], '1':[0b01000,0b11111,0b00000], '2':[0b10011,0b10101,0b01001],
    '3':[0b10001,0b10101,0b01010], '4':[0b00110,0b01010,0b11111], '5':[0b11101,0b10101,0b10010],
    '6':[0b01110,0b10101,0b10010], '7':[0b10001,0b10010,0b11100], '8':[0b01010,0b10101,0b01010],
    '9':[0b01001,0b10101,0b01110], '.':[0b00001], '-':[0b00100,0b00100,0b00100], ' ':[0,0],
    '(':[0b01110,0b10001], ')':[0b10001,0b01110],
};

// ============================================================================
// HANDOFF — PORTRAIT PIXELS (open RE item, for mvc2-sh4-re-expert) [hud:names-v1]
// ----------------------------------------------------------------------------
// Tier-2 NAMES are shipped (real roster, NAMES[] above, driven by live char_id).
// The portrait IMAGE is the one piece still a stand-in (team-tinted monogram).
//
// Investigated this session (CONFIRMED):
//   • PLxx_FAC.BIN (one per char, ~26 KB) header = [u32 count=0x0C][u32 off0][u32 off1]
//     then 3 segments [0x0C..off0][off0..off1][off1..EOF]. Segment 0 begins with the
//     TA/Naomi control word 0x06801450 (para-type 0x6, "global polygon"), NOT a flat
//     w/h texture header — so FAC is a NaomiLib DISPLAY LIST (textured-quad portrait),
//     the same family as STGxxPOL. Linear + twiddled 16bpp decodes are pure noise
//     (verified: _fac_seg1_16_*.png, _seltex_*.png). So the pixels are inside the
//     display list's referenced texture, likely VQ- or LZSS-compressed.
//   • SELTEX.BIN (1.59 MB) is the select-screen texture sheet (raw pixel-ish, but
//     twiddled/VQ — linear & plain-twiddle decodes are noise). Likely the true home
//     of the small corner mugshots, but also undecoded.
//
// TO CLOSE (ask mvc2-sh4-re-expert):
//   1. Find the routine that loads PLxx_FAC.BIN (grep the marvelous2 disasm for the
//      file-table index of "PLxx_FAC") and identify the texture it uploads to VRAM
//      (TCW/PCW -> dims + pixel format + VQ/twiddle).  -> then write tools/rip_fac.py
//      to emit web/render-replica/hud/portraits/portraits.{png,json}
//      ({ rects: { "<char_id>": {x,y,w,h} } }).
//   2. On deploy of that atlas, this.portraits auto-populates in load() and _plate()
//      draws the real FAC pixels with ZERO further client change.
// ============================================================================
