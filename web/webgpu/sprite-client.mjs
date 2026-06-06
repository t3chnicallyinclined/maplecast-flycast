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
    this.sparks = [];          // active hit-sparks {x,y,t0,type}
    this.sparksOn = true;
    this.effects = [];         // server-isolated TA effect quads {hash,cx,cy,w,h} (EFCT packet)
    this.objects = [];         // pool satellite objects {cid,sid,x,y} (OBJS packet) — cape/effects/projectiles
    this.objectsOn = true;
    this._fxCache = new Map(); // texture hash -> canvas (decoded from TXTR packets)
    this._lastEfctN = 0;
    this.effectsOn = true;
  }
  _blank() {
    return { active:0, char_id:0, sprite_id:-1, screen_x:0, screen_y:0, facing:0, palette:0,
             health:0, _ph:-1, _maxhp:144,   // health + previous-health (hits) + max seen (bar full)
             // prediction: previous screen pos + timestamps -> observed screen velocity
             px:0, py:0, t:0, pt:0, vx:0, vy:0 };
  }

  static isGSTA(d) {
    return d.length >= 4 && d[0]===GSTA_MAGIC[0] && d[1]===GSTA_MAGIC[1]
        && d[2]===GSTA_MAGIC[2] && d[3]===GSTA_MAGIC[3];
  }

  // 'EFCT'(4) + count(1) + count*[id(1) cx(i16) cy(i16) w(i16) h(i16)] — the
  // server-isolated TA effect quads (screen space). Routed away from the TA
  // decoder by the page (same as GSTA) so applyFrame never sees it.
  static isEFCT(d) {
    return d.length >= 5 && d[0]===69 && d[1]===70 && d[2]===67 && d[3]===84; // 'E','F','C','T'
  }
  onEFCT(d) {
    const n = d[4];
    const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
    const fx = [];
    let o = 5;
    for (let i = 0; i < n && o + 12 <= d.length; i++) {
      fx.push({
        hash: dv.getUint32(o, true),
        cx: dv.getInt16(o + 4, true), cy: dv.getInt16(o + 6, true),
        w:  dv.getInt16(o + 8, true), h:  dv.getInt16(o + 10, true),
      });
      o += 12;
    }
    this.effects = fx;
    this._lastEfctN = n;
  }

  // 'TXTR'(4) + hash(4) + w(2) + h(2) + zstd(RGBA). The page decompresses and
  // calls onTXTR with the raw RGBA; we cache hash -> canvas for additive draw.
  static isTXTR(d) {
    return d.length >= 12 && d[0]===84 && d[1]===88 && d[2]===84 && d[3]===82; // 'T','X','T','R'
  }
  onTXTR(hash, w, h, rgba) {
    if (!rgba || rgba.length < w * h * 4 || w <= 0 || h <= 0) return;
    let cv = this._fxCache.get(hash);
    if (!cv) { cv = document.createElement('canvas'); cv.width = w; cv.height = h; }
    const ctx = cv.getContext('2d');
    const id = new ImageData(new Uint8ClampedArray(rgba.subarray(0, w * h * 4)), w, h);
    ctx.putImageData(id, 0, 0);
    this._fxCache.set(hash, cv);
  }

  // 'OBJS'(4) + count(1) + N×[cid(1), sprite_id(2 LE), x(i16 LE), y(i16 LE)] = 7B each.
  // Pool satellite objects (cape/effects/projectiles) -> rip sprites. See re-catalog/.
  static isOBJS(d) {
    return d.length >= 5 && d[0]===79 && d[1]===66 && d[2]===74 && d[3]===83; // 'O','B','J','S'
  }
  onOBJS(d) {
    const n = d[4]; const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
    const objs = []; let o = 5;
    for (let i = 0; i < n && o + 8 <= d.length; i++) {
      const raw = dv.getUint16(o+1, true);   // sprite_id with 0x8000 hflip bit
      objs.push({ cid: d[o], sid: raw & 0x7fff, type: d[o+3], x: dv.getInt16(o+4, true), y: dv.getInt16(o+6, true) });
      o += 8;
    }
    this.objects = objs;
  }

  // Point the client at a server dir of per-character atlases
  // (PL{cid:02X}.{json,png}). Characters are then fetched on demand as they
  // appear in the streamed state — only what's picked gets downloaded.
  setCharBase(base) { this.charBase = base; this.loadFxAtlas(); }

  // Load the isolated-effect atlas (fx_atlas.{png,json}) — the 5 universal effect
  // textures the server's EFCT isolation references by id (additive overlays).
  async loadFxAtlas() {
    if (this._fx || this._fxLoading || !this.charBase) return;
    this._fxLoading = true;
    const base = this.charBase.replace(/\/chars\/?$/, '/effects') + '/fx_atlas';
    const bust = '?t=' + Date.now();
    try {
      const json = await (await fetch(base + '.json' + bust)).json();
      const blob = await (await fetch(base + '.png' + bust)).blob();
      this._fxImg = await createImageBitmap(blob);
      this._fx = json;
      console.log('[sprite-client] loaded fx_atlas:', (json.effects || []).length, 'effects');
    } catch (e) { console.warn('[sprite-client] fx_atlas load failed', e); }
    finally { this._fxLoading = false; }
  }

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
        this.chars[cid] = { img, sprites: json.sprites, name: json.name || ('char' + cid), pal128: json.pal128 };
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
    // Global HUD values — already in the state, so the HUD renders from data alone.
    this.hud = {
      timer:   dv.getUint8(B + 1),
      p1lvl:   dv.getUint8(B + 3),  p2lvl:   dv.getUint8(B + 4),
      p1combo: dv.getUint16(B + 5, true), p2combo: dv.getUint16(B + 7, true),
      p1fill:  dv.getUint16(B + 9, true), p2fill:  dv.getUint16(B + 11, true),
    };
    this._maxfill = Math.max(this._maxfill || 1, this.hud.p1fill, this.hud.p2fill);
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
      // Hit-spark: a health drop = a hit landed -> spawn a spark on the defender's
      // upper body. (Guard against round-reset jumps and the first frame.)
      const hp = dv.getUint8(ci + 3);
      if (this.sparksOn && this.inMatch && sl.active && sl._ph >= 0 && hp < sl._ph && (sl._ph - hp) <= 60) {
        const jx = (Math.random()*22 - 11), jy = (Math.random()*16 - 8);
        this.sparks.push({ x: nx + jx, y: ny - 55 + jy, t0: now, type: (sl._ph - hp) >= 14 ? 2 : 0 });
        if (this.sparks.length > 24) this.sparks.shift();
      }
      sl._ph = sl.active ? hp : -1;
      sl.health = hp;
      if (sl.active && hp > sl._maxhp) sl._maxhp = hp;   // round-start full = bar max
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
      const cfl = (sl.facing !== sp.facing);
      const cdx = cfl ? -(sp.dx + sp.wG) : sp.dx;   // mirror the (asymmetric) anchor when flipped
      out.push({ charId: sl.char_id, slot: s, sx: sp.x, sy: sp.y, sw: sp.w, sh: sp.h,
        dx: (exx+cdx*S)*scaleX, dy: (eyy+sp.dy*S)*scaleY, dw: sp.wG*S*scaleX, dh: sp.hG*S*scaleY,
        flip: cfl });
    }
    // Satellite objects (cape, lightning, projectiles) — each = the OWNER's rip
    // sprite at its own screen pos. A character = body (0x144) + these. (re-catalog/)
    if (this.objectsOn !== false) for (const o of (this.objects || [])) {
      const c = this.chars[o.cid];
      if (!c) { this.loadChar(o.cid); continue; }
      if (!c.img) continue;
      const sp = c.sprites[o.sid];
      if (!sp) continue;
      // Find the owner's slot + its LIVE (predicted) screen pos. Attached parts ride
      // the owner; spawned objects (far from owner) keep their own pos.
      let osl = null, oslot = 0;
      for (let s = 0; s < 6; s++) if (this.slot[s].active && this.slot[s].char_id === o.cid) { osl = this.slot[s]; oslot = s; break; }
      if (!osl) continue;
      let ox = osl.screen_x, oy = osl.screen_y;
      if (this.predict !== false) { const dt = Math.min(now - osl.t, 33); if (dt > 0) { ox += osl.vx*dt; oy += osl.vy*dt; } }
      if ((ox === 0 && oy === 0) || ox < -60 || ox > 700) continue;   // off-screen assist -> its parts vanish
      // type 3 = cape (always attached to the owner — no distance guess, fixes the
      // floating second cape). other types: distance decides attached vs spawned.
      const far = (o.type !== 3) && ((Math.abs(o.x - ox) + Math.abs(o.y - oy)) > 130);
      const px = far ? o.x : ox, py = far ? o.y : oy;
      const S = this.spriteScale || 1;
      const fl = (osl.facing !== sp.facing);
      const dxv = fl ? -(sp.dx + sp.wG) : sp.dx;   // mirror the anchor when flipped
      // CAPE-ON-CROUCH (derived from the TA): attached parts ride the BODY's top, not
      // the feet. The body sprite's anchor dy encodes pose height; shift the cape by
      // (current body dy − the char's tallest/standing dy). Track the standing ref.
      let cdy = sp.dy;
      if (o.type === 3) {
        const bsp = c.sprites[osl.sprite_id];
        if (bsp) {
          if (!this._refDy) this._refDy = {};
          const r = this._refDy[o.cid];
          this._refDy[o.cid] = (r === undefined) ? bsp.dy : Math.min(r, bsp.dy);
          cdy = sp.dy + (bsp.dy - this._refDy[o.cid]);   // crouch/jump lowers/raises the cape with the body
        }
      }
      out.push({ charId: o.cid, slot: oslot, z: -1, sx: sp.x, sy: sp.y, sw: sp.w, sh: sp.h,
        dx: (px + dxv*S)*scaleX, dy: (py + cdy*S)*scaleY, dw: sp.wG*S*scaleX, dh: sp.hG*S*scaleY,
        flip: fl });
    }
    // The renderer groups CONSECUTIVE same-cid sprites and drops chars past maxGroups(8).
    // Sort by cid so each character's body+objects form ONE group, objects (z=-1) behind
    // bodies (z=0). (unshift broke this by scattering mixed-cid objects to the front.)
    out.sort((a, b) => (a.charId - b.charId) || ((a.z || 0) - (b.z || 0)));
    this._lastNote = loading ? `loading ${loading} char atlas…` : (missing ? `holding ${missing}: ${missKeys.join(' ')}` : 'all visible poses captured');
    return out;
  }

  // Active hit-sparks for the GPU additive pass — each grows + fades over ~280ms.
  // Returns [{x,y,size,alpha,frame}] in canvas px; prunes expired sparks.
  buildSparkList(canvasW, canvasH) {
    const scaleX = canvasW / (this.screenW || 640), scaleY = canvasH / (this.screenH || 480);
    const now = (typeof performance !== 'undefined') ? performance.now() : 0;
    const DUR = 280, BASE = 50;                    // ms lifetime, base screen px
    this.sparks = this.sparks.filter(sp => (now - sp.t0) < DUR);
    const out = [];
    for (const sp of this.sparks) {
      const age = (now - sp.t0) / DUR;             // 0..1
      const size = BASE * (0.55 + age * 0.9);      // grow
      out.push({ x: sp.x*scaleX, y: sp.y*scaleY, size: size*scaleX,
                 alpha: Math.max(0, 1 - age*age), frame: sp.frame !== undefined ? sp.frame : (sp.type|0) });
    }
    return out;
  }

  // ===== HUD from state — health/meter/combo/timer are already in the GSTA =====
  _pointHealth(slots) {
    for (const s of slots) { const sl = this.slot[s]; if (sl.active) return Math.max(0, Math.min(1, sl.health / (sl._maxhp || 144))); }
    return 0;
  }
  _bar(ctx, x, y, w, h, r, fill, bg, fromRight) {
    r = Math.max(0, Math.min(1, r));
    ctx.fillStyle = bg; ctx.fillRect(x, y, w, h);
    ctx.fillStyle = fill; const fw = w * r;
    ctx.fillRect(fromRight ? x + w - fw : x, y, fw, h);
    ctx.strokeStyle = 'rgba(0,0,0,0.85)'; ctx.lineWidth = 1; ctx.strokeRect(x + 0.5, y + 0.5, w - 1, h - 1);
  }
  // Draw the server-isolated TA effects additively over the scene. Each EFCT
  // descriptor is {id,cx,cy,w,h} in 640x480 screen space; id indexes fx_atlas.
  // (Drawn on the HUD overlay canvas AFTER drawHUD's clear, so call it after.)
  drawEffects(ctx) {
    if (!this.effectsOn || !this.effects.length) return;
    const W = ctx.canvas.width, H = ctx.canvas.height, sx = W / 640, sy = H / 480;
    ctx.save();
    ctx.globalCompositeOperation = 'lighter';   // additive
    for (const e of this.effects) {
      const tex = this._fxCache.get(e.hash); if (!tex) continue;  // texture not received yet
      const dw = Math.max(2, Math.abs(e.w)) * sx, dh = Math.max(2, Math.abs(e.h)) * sy;
      ctx.drawImage(tex, e.cx * sx - dw / 2, e.cy * sy - dh / 2, dw, dh);
    }
    ctx.restore();
  }

  drawHUD(ctx) {
    const W = ctx.canvas.width, H = ctx.canvas.height;
    ctx.clearRect(0, 0, W, H);
    if (!this.inMatch) return;
    ctx.save(); ctx.scale(W / 640, H / 480);
    const hud = this.hud || {}, mf = this._maxfill || 1;
    this._bar(ctx, 18, 16, 292, 14, this._pointHealth([0,2,4]), '#46e84a', '#15401a', false);  // P1 health
    this._bar(ctx, 330, 16, 292, 14, this._pointHealth([1,3,5]), '#46e84a', '#15401a', true);   // P2 health
    this._bar(ctx, 18, 456, 250, 9, (hud.p1fill||0)/mf, '#ffcc33', '#5a3a00', false);            // P1 meter
    this._bar(ctx, 372, 456, 250, 9, (hud.p2fill||0)/mf, '#ffcc33', '#5a3a00', true);            // P2 meter
    ctx.fillStyle = '#ffd24d';
    for (let i = 0; i < (hud.p1lvl||0); i++) ctx.fillRect(18 + i*12, 446, 9, 6);
    for (let i = 0; i < (hud.p2lvl||0); i++) ctx.fillRect(613 - i*12, 446, 9, 6);
    ctx.fillStyle = '#fff'; ctx.font = 'bold 22px monospace'; ctx.textAlign = 'center'; ctx.textBaseline = 'top';
    ctx.fillText(String(hud.timer||0).padStart(2,'0'), 320, 14);
    ctx.font = 'bold 15px monospace';
    if ((hud.p1combo||0) > 1) { ctx.fillStyle = '#ffe14d'; ctx.textAlign = 'left';  ctx.fillText(hud.p1combo + ' HIT', 24, 40); }
    if ((hud.p2combo||0) > 1) { ctx.fillStyle = '#ffe14d'; ctx.textAlign = 'right'; ctx.fillText(hud.p2combo + ' HIT', 616, 40); }
    ctx.restore();
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
