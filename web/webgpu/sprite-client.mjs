// sprite-client.mjs — the ROM-asset client proof (Option 6).
//
// Renders MVC2 characters from the ~253-byte GSTA game state ALONE, with NO TA
// stream: preload a sprite atlas once (baked by bake.mjs -> bake_atlas.{png,json}),
// then for each active character draw sprite[char_id][sprite_id] at the reported
// screen position. This is the client that, if it matches the byte-perfect TA
// mirror, proves the ~800x bandwidth model.
//
// Input is the same GSTA broadcast the bake harness consumes:
//   'GSTA'(4) + serialized GameState (wire layout = gamestate.cpp serialize()):
//   25-byte global header, then 6 * 38-byte character blocks. We read the two
//   point characters (render slots 0,1) — active, char_id, facing, palette,
//   screen_x/y, sprite_id.
//
// Canvas2D on purpose: fastest path to a visible side-by-side test, fully
// decoupled from the WebGPU TA renderer. A WebGPU port comes later for the
// pixel-exact diff (plan Phase 2).
//
// KNOWN GAPS (first pass, see ROM-ASSET-CLIENT-PLAN.md):
//   - Palette: the baked crop carries a fixed palette; live skin swaps (the PVR
//     palette-bank system) are NOT reapplied here yet. Wrong colors on skinned
//     chars is expected until palette-from-state lands.
//   - Mirror anchor: flipping uses the character's screen_x as the mirror axis,
//     which is approximate for sprites whose alpha bbox is asymmetric about the
//     origin. Good enough to prove placement; refine in P1.5.
//   - Only the 2 point characters are drawn (assists/projectiles/effects are
//     separate object systems not in the 6 tracked slots).

const GSTA_MAGIC = [71, 83, 84, 65]; // 'G','S','T','A'

export class SpriteClient {
  constructor() {
    // Per-character atlases, lazy-loaded by char_id as characters appear in the
    // state. chars[char_id] = { img:ImageBitmap, sprites:{sid:{...}}, name }.
    this.chars = {};
    this._loading = {};       // char_id -> Promise (de-dupe concurrent fetches)
    this.charBase = null;     // URL base for per-char atlases, e.g. './test-atlas/chars'
    this.screenW = 640; this.screenH = 480;
    this.spriteScale = 1.75;  // constant size factor (MVC2 pans, doesn't dynamic-zoom)
    this._zoom = 1;           // derived camera zoom — INFO ONLY (shown, not applied)
    this._lastFc = null;      // last game frame_counter (for frame-timed velocity)
    this.inMatch = 0;
    // All 6 character slots (P1C1,P2C1,P1C2,P2C2,P1C3,P2C3). The on-screen POINT
    // can be any of a side's 3 — and a called assist is a bench char briefly
    // active — so we read all 6 and render every active one.
    this.slot = Array.from({ length: 6 }, () => this._blank());
    this._lastNote = 'no atlas loaded';
    // State-stream (GSTA) bandwidth — the whole point of Option 6. Rolling 1s window.
    this._bwBytes = 0; this._bwFrames = 0; this._bwT0 = 0;
    this._bwRate = 0; this._bwHz = 0; this._lastSize = 0;
  }
  _blank() {
    return { active:0, char_id:0, sprite_id:-1, screen_x:0, screen_y:0, facing:0, palette:0,
             // prediction: previous screen pos + timestamps -> observed screen velocity
             px:0, py:0, t:0, pt:0, vx:0, vy:0 };
  }

  static isGSTA(d) {
    return d.length >= 4 && d[0]===GSTA_MAGIC[0] && d[1]===GSTA_MAGIC[1]
        && d[2]===GSTA_MAGIC[2] && d[3]===GSTA_MAGIC[3];
  }

  // Point the client at a server dir of per-character atlases
  // (PL{cid:02X}.{json,png}). Characters are then fetched on demand as they
  // appear in the streamed state — only what's picked gets downloaded.
  setCharBase(base) { this.charBase = base; }

  // Lazy-load ONE character's atlas: <charBase>/PL{cid:02X}.{json,png}.
  loadChar(cid) {
    if (this.chars[cid] || this._loading[cid] || !this.charBase) return this._loading[cid];
    const hex = (cid & 0xff).toString(16).padStart(2, '0').toUpperCase();
    const base = `${this.charBase}/PL${hex}`;
    const bust = '?t=' + Date.now();   // re-fetch fresh after an atlas rebuild
    const p = (async () => {
      try {
        const json = await (await fetch(base + '.json' + bust)).json();
        const blob = await (await fetch(base + '.png' + bust)).blob();
        const img = await createImageBitmap(blob);
        this.screenW = json.screenW || this.screenW; this.screenH = json.screenH || this.screenH;
        this.chars[cid] = { img, sprites: json.sprites, name: json.name || ('char' + cid), pal16: json.pal16 };
        console.log('[sprite-client] loaded char', cid, json.name, Object.keys(json.sprites).length, 'sprites');
      } catch (e) {
        this.chars[cid] = { img: null, sprites: {}, name: 'char' + cid, err: String(e) };
        console.error('[sprite-client] char', cid, 'load failed', e);
      } finally { delete this._loading[cid]; }
    })();
    this._loading[cid] = p; return p;
  }

  // Combined-atlas load (file-picker / single-URL fallback): one JSON
  // {chars:{cid:{sprites}}} + one shared PNG. Populates the same per-char map.
  async loadAtlas(atlasJson, pngBlob) {
    const img = await createImageBitmap(pngBlob);
    this.screenW = atlasJson.screenW || 640; this.screenH = atlasJson.screenH || 480;
    for (const cid in (atlasJson.chars || {})) {
      const c = atlasJson.chars[cid];
      this.chars[cid] = { img, sprites: c.sprites, name: c.name || ('char' + cid) };
    }
    this._lastNote = '';
  }
  async loadAtlasFromUrl(base) {
    const bust = '?t=' + Date.now();
    const json = await (await fetch(base + '.json' + bust)).json();
    const blob = await (await fetch(base + '.png' + bust)).blob();
    await this.loadAtlas(json, blob);
    return Object.keys(json.chars || {}).length;
  }

  onGSTA(d) {
    // --- state-stream bandwidth (rolling 1s window) ---
    const _now = (typeof performance !== 'undefined') ? performance.now() : 0;
    if (!this._bwT0) this._bwT0 = _now;
    this._bwBytes += d.byteLength; this._bwFrames++; this._lastSize = d.byteLength;
    const _dt = _now - this._bwT0;
    if (_dt >= 1000) {
      this._bwRate = this._bwBytes / (_dt / 1000);   // bytes/sec
      this._bwHz   = this._bwFrames / (_dt / 1000);
      this._bwBytes = 0; this._bwFrames = 0; this._bwT0 = _now;
    }
    const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
    const B = 4;                       // payload starts after 'GSTA'
    this.inMatch = dv.getUint8(B + 0);
    const now = (typeof performance !== 'undefined') ? performance.now() : 0;
    // Velocity is timed on the GAME frame delta, NOT the jittery network arrival
    // gap — so a late/early packet doesn't wobble the extrapolation speed.
    const fc = dv.getUint32(B + 21, true);
    const dfr = (this._lastFc != null) ? (fc - this._lastFc) : 0; this._lastFc = fc;
    const frameDt = (dfr >= 1 && dfr <= 8) ? dfr * 16.667 : 0;   // ms of game time since last state
    for (let s = 0; s < 6; s++) {
      const ci = B + 25 + s * 38;      // 25-byte global header + 38*slot
      const sl = this.slot[s];
      const nx = dv.getFloat32(ci + 16, true), ny = dv.getFloat32(ci + 20, true);
      if (sl.active && frameDt > 0) {
        const ivx = (nx - sl.screen_x) / frameDt, ivy = (ny - sl.screen_y) / frameDt;
        sl.vx = sl.vx * 0.4 + ivx * 0.6;   // EMA-smoothed px/ms (kills velocity noise)
        sl.vy = sl.vy * 0.4 + ivy * 0.6;
      } else if (!sl.active) { sl.vx = 0; sl.vy = 0; }
      sl.t = now;
      sl.active   = dv.getUint8(ci + 0);
      sl.char_id  = dv.getUint8(ci + 1);
      sl.facing   = dv.getUint8(ci + 2);
      sl.palette  = dv.getUint8(ci + 7);
      sl.pos_x    = dv.getFloat32(ci + 8,  true);   // arena/world X — for the zoom
      sl.screen_x = nx;
      sl.screen_y = ny;
      sl.sprite_id= dv.getUint16(ci + 32, true);
    }
    // Exact size from state: camera zoom = |Δscreen_x / Δpos_x| between two
    // active characters (camera offset cancels). Tracks MVC2's live zoom.
    let a = -1, b = -1;
    for (let s = 0; s < 6; s++) if (this.slot[s].active) { if (a < 0) a = s; else { b = s; break; } }
    if (a >= 0 && b >= 0) {
      const dp = this.slot[a].pos_x - this.slot[b].pos_x;
      if (Math.abs(dp) > 5) {
        const z = Math.abs((this.slot[a].screen_x - this.slot[b].screen_x) / dp);
        if (z > 0.1 && z < 10) this._zoom = z;
      }
    }
    for (let s = 0; s < 6; s++) { const sl = this.slot[s]; if (sl.active) this.loadChar(sl.char_id); }
  }

  // Draw the current state into a 2D context. Returns a small status object.
  render(ctx) {
    const W = ctx.canvas.width, H = ctx.canvas.height;
    ctx.clearRect(0, 0, W, H);
    this._now0 = (typeof performance !== 'undefined') ? performance.now() : 0;
    const sx = W / (this.screenW || 640);
    const sy = H / (this.screenH || 480);
    let drawn = 0, missing = 0, loading = 0, missKeys = [];

    if (!this._held) this._held = new Array(6).fill(null);   // last drawn sprite per slot
    for (let s = 0; s < 6; s++) {
      const sl = this.slot[s];
      if (!sl.active) { this._held[s] = null; continue; }
      const c = this.chars[sl.char_id];
      if (!c) { loading++; continue; }            // this char's atlas still downloading
      if (!c.img) continue;                        // load failed for this char
      let sp = c.sprites[sl.sprite_id];
      if (sp) {
        this._held[s] = { char_id: sl.char_id, sp };
      } else {
        // Sparse atlas: this sprite_id wasn't in the rip. Hold the last known
        // pose for this character (same char_id) instead of blanking → no blink.
        missing++;
        if (missKeys.length < 3) missKeys.push(`${sl.char_id}/0x${(sl.sprite_id&0xffff).toString(16)}`);
        const h = this._held[s];
        if (h && h.char_id === sl.char_id) sp = h.sp; else continue;
      }

      // Render-time extrapolation: advance position by the observed screen
      // velocity over the time since the last state, clamped to ~2 frames so a
      // direction change can't overshoot. Hides network jitter / inter-state gap.
      let exx = sl.screen_x, eyy = sl.screen_y;
      if (this.predict !== false) {
        const dt = Math.min(this._now0 - sl.t, 33);     // ms since last state, cap 33ms
        if (dt > 0) { exx += sl.vx * dt; eyy += sl.vy * dt; }
      }

      // Destination in game space (sprite size × constant scale).
      const S = this.spriteScale || 1;
      const gx = exx + sp.dx*S, gy = eyy + sp.dy*S;
      const dx = gx * sx, dy = gy * sy, dw = sp.wG * S * sx, dh = sp.hG * S * sy;
      const flip = (sl.facing !== sp.facing);

      ctx.save();
      if (flip) {
        // Mirror horizontally about the character's (extrapolated) screen_x.
        const axis = exx * sx;
        ctx.translate(axis, 0); ctx.scale(-1, 1); ctx.translate(-axis, 0);
      }
      ctx.drawImage(c.img, sp.x, sp.y, sp.w, sp.h, dx, dy, dw, dh);
      ctx.restore();
      drawn++;
    }
    this._lastNote = loading ? `loading ${loading} char atlas…`
                   : missing ? `holding (uncaptured) ${missing}: ${missKeys.join(' ')}`
                   : 'all visible poses captured';
    return { drawn, missing, note:this._lastNote };
  }

  // GPU path: emit [{charId, sx,sy,sw,sh (atlas px), dx,dy,dw,dh (canvas px), flip}]
  // for the active characters — same extrapolation + held-pose logic as render().
  buildDrawList(canvasW, canvasH) {
    const scaleX = canvasW / (this.screenW || 640), scaleY = canvasH / (this.screenH || 480);
    const now = (typeof performance !== 'undefined') ? performance.now() : 0;
    if (!this._held) this._held = new Array(6).fill(null);
    const out = []; let loading = 0, missing = 0, missKeys = [];
    for (let s = 0; s < 6; s++) {
      const sl = this.slot[s];
      if (!sl.active) { this._held[s] = null; continue; }
      const c = this.chars[sl.char_id];
      if (!c) { loading++; continue; }
      if (!c.img) continue;
      let sp = c.sprites[sl.sprite_id];
      if (sp) this._held[s] = { char_id: sl.char_id, sp };
      else { missing++; if (missKeys.length<3) missKeys.push(`${sl.char_id}/0x${(sl.sprite_id&0xffff).toString(16)}`);
             const h = this._held[s]; if (h && h.char_id === sl.char_id) sp = h.sp; else continue; }
      let exx = sl.screen_x, eyy = sl.screen_y;
      if (this.predict !== false) { const dt = Math.min(now - sl.t, 33); if (dt > 0) { exx += sl.vx*dt; eyy += sl.vy*dt; } }
      const S = this.spriteScale || 1;   // constant scale; scales about feet/center
      out.push({ charId: sl.char_id, slot: s, sx: sp.x, sy: sp.y, sw: sp.w, sh: sp.h,
        dx: (exx+sp.dx*S)*scaleX, dy: (eyy+sp.dy*S)*scaleY, dw: sp.wG*S*scaleX, dh: sp.hG*S*scaleY,
        flip: (sl.facing !== sp.facing) });
    }
    this._lastNote = loading ? `loading ${loading} char atlas…` : (missing ? `holding ${missing}: ${missKeys.join(' ')}` : 'all visible poses captured');
    return out;
  }

  statsText() {
    const loaded = Object.keys(this.chars);
    const names = loaded.map(c => this.chars[c].name + (this.chars[c].img?'':'!')).join(', ') || '(none yet)';
    const nm = (i) => { const c = this.chars[this.slot[i].char_id]; return c ? c.name : '…'; };
    const LAB = ['P1a','P2a','P1b','P2b','P1c','P2c'];
    const sl = (i) => { const x = this.slot[i];
      return `${LAB[i]} ${x.active?'ON ':'-- '} ${nm(i)}(${x.char_id}) sid=0x${(x.sprite_id&0xffff).toString(16).padStart(4,'0')} f${x.facing}`; };
    const kbps = (this._bwRate/1024).toFixed(2);
    const mbps = (this._bwRate*8/1e6).toFixed(3);
    return `SPRITE CLIENT — ${loaded.length} char atlas loaded\n`
         + `loaded: ${names}\n`
         + `STATE STREAM: ${kbps} KB/s (${mbps} Mbps)\n`
         + `size=${(this.spriteScale||1).toFixed(2)}x  zoom(info,not applied)=${(this._zoom||1).toFixed(2)}\n`
         + `  ${this._bwHz.toFixed(0)} Hz x ${this._lastSize} B/frame\n`
         + `inMatch=${this.inMatch}\n` + [0,1,2,3,4,5].map(sl).join('\n') + '\n'
         + (this._lastNote ? `note: ${this._lastNote}` : '');
  }
}
