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
    this.spriteScale = 1.0;   // constant size factor — 1.0: baked offsets are already screen-space
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

    // ===== STAF (stripped-TA frame) render path =====
    // Pixel-exact: the server ships the full textured-quad list every frame
    // (STAF) + each unique texture ONCE (TX64), content-addressed by a 64-bit id.
    // We cache tex_id -> canvas and draw each quad's axis-aligned dest rect with
    // its UV sub-rect + PVR blend. No atlas, no VRAM, no ta_parse — see
    // docs/STRIPPED-TA-DESIGN.md. tex_id is a JS string ("hi:lo") so the 64-bit
    // value survives Map keys without precision loss.
    this.stafQuads = [];       // per-tri descriptors {key,blend,shadInstr,ignoreTexA,textured,punch,voff}
    this._stafV = null;        // Float32Array: 8 floats/vert [x,y,u,v,r,g,b,a], 3 verts/tri
    this._stafVCount = 0;
    this._stafTex = new Map(); // tex_id(string) -> {w,h,rgba} decoded texture (from TX64)
    this.stafFrame = 0;
    this.stafOn = true;
    this._stafQuadN = 0; this._stafTexN = 0;

    // ===== Assembly-driven render path (parallel to whole-sprite) =====
    // When true, buildAssemblyDrawList() is the GPU source instead of buildDrawList().
    // Each object's sprite_id resolves to an assembly (a list of part placements);
    // we draw each part rect from the per-char part atlas at its (dx,dy) offset.
    // See docs/ASSEMBLY-DRIVEN-DESIGN.md §2.3.
    this.assemblyMode = false;
    // asm[cid] = { img:ImageBitmap, parts:{idx:{x,y,w,h}}, asm:{sid:[{part,dx,dy,flip,z}]},
    //              palette:[...], pal128:[[r,g,b]...], screenW, screenH, name }
    this.asmChars = {};
    this._asmLoading = {};     // cid -> Promise (de-dupe)
    // CpsX/CpsY game scale (work.asm, Preppy RE) — part dx/dy/w/h are in game px;
    // screen_x/y is already in 640x480 screen space. These factors convert game px
    // to screen px: CpsXScale=0x3FD55555=5/3, CpsYScale=0x40092492=15/7.
    this.asmScaleX = 5/3;   // 1.6667 — CpsXScale from work.asm
    this.asmScaleY = 15/7;  // 2.1429 — CpsYScale from work.asm
    this._asmNote = 'assembly: no atlas';
    this._asmMiss = 0; this._asmDrawn = 0;
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
    for (let i = 0; i < n && o + 20 <= d.length; i++) {
      fx.push({
        hash: dv.getUint32(o, true),
        cx: dv.getInt16(o + 4, true), cy: dv.getInt16(o + 6, true),
        w:  dv.getInt16(o + 8, true), h:  dv.getInt16(o + 10, true),
        // UV sub-rect of the shared EFKYTEX page (u16 normalized) — which frame to draw.
        u0: dv.getUint16(o + 12, true) / 65535, v0: dv.getUint16(o + 14, true) / 65535,
        u1: dv.getUint16(o + 16, true) / 65535, v1: dv.getUint16(o + 18, true) / 65535,
      });
      o += 20;
    }
    this.effects = fx;
    this._lastEfctN = n;
  }

  // 'PALF'(4) + 6×u16 paleffect (char+0x40). Nonzero = that slot's body is hit-
  // flashing (engine swaps it to the hurt palette bank). We tint the body.
  static isPALF(d) { return d.length >= 16 && d[0]===80 && d[1]===65 && d[2]===76 && d[3]===70; } // 'P','A','L','F'
  onPALF(d) {
    const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
    for (let s = 0; s < 6 && 4 + s * 2 + 1 < d.length; s++)
      this.slot[s].paleffect = dv.getUint16(4 + s * 2, true);
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

  // ===== STAF channel ========================================================
  // 64-bit tex_id -> string key (so it can index a Map without float precision loss).
  static texKey(lo, hi) { return (hi >>> 0).toString(16).padStart(8, '0') + ':' + (lo >>> 0).toString(16).padStart(8, '0'); }

  // 'TX64'(4) texId(8) w(2) h(2) rawSize(4) zstd(RGBA). Caller decompresses the
  // RGBA (offset 20) and passes it in — same shape as onTXTR but 64-bit keyed.
  static isTX64(d) {
    return d.length >= 20 && d[0]===84 && d[1]===88 && d[2]===54 && d[3]===52; // 'T','X','6','4'
  }
  // Cache the decoded RGBA (+dims) by 64-bit key. The GL renderer (StafGL) uploads
  // it to a GPUtexture lazily on first use and tracks uploaded keys itself; we just
  // hold the bytes so a re-decode is never needed. (rgba is copied — the source
  // decompress buffer is reused by the next packet.)
  onTX64(key, w, h, rgba) {
    if (!rgba || rgba.length < w * h * 4 || w <= 0 || h <= 0) return;
    this._stafTex.set(key, { w, h, rgba: new Uint8Array(rgba.subarray(0, w * h * 4)) });
    this._stafTexN = this._stafTex.size;
  }

  // 'STAF'(4) frameNum(4) pvr_snapshot[16](64) triCount(u32), then per TRIANGLE:
  //   texId(8) blend(1) listType(1) tspFlags(1)
  //   3 × [ x(i16) y(i16) u(u16) v(u16) col(4 = R,G,B,A) ]      = 11 + 36 = 47 B/tri
  // x,y are 640x480 screen space; u,v are Q0.16 (val/65535). texId 0 = untextured.
  //   blend    = (SrcInstr<<4)|DstInstr  (PVR TSP) -> gl.blendFunc
  //   tspFlags = bit0-1 ShadInstr, bit2 IgnoreTexA, bit3 textured, bit4 punch-through
  // Each triangle carries its REAL per-vertex (x,y,u,v,col); the GPU rasterizes
  // textured triangles (StafGL renderer), exact for any winding/flip/shear. The
  // buffer is ALREADY zstd-decompressed (ZCST stripped by the caller).
  static isSTAF(d) {
    return d.length >= 10 && d[0]===83 && d[1]===84 && d[2]===65 && d[3]===70; // 'S','T','A','F'
  }
  // Parse STAF into flat typed arrays the GL renderer consumes directly:
  //   _stafV:   Float32 [x,y,u,v, r,g,b,a] per vertex (3 verts/tri)   (pos in 640x480, col 0..1)
  //   _stafTri: per-tri {key, blend, shadInstr, ignoreTexA, textured, punch, voff}
  onSTAF(d) {
    const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
    this.stafFrame = dv.getUint32(4, true);
    const triCount = dv.getUint32(72, true);
    let o = 76;
    const maxTri = Math.min(triCount, ((d.length - 76) / 47) | 0);
    // 3 verts/tri × 8 floats/vert
    if (!this._stafV || this._stafV.length < maxTri * 24) this._stafV = new Float32Array(maxTri * 24);
    const V = this._stafV;
    const tris = [];
    let vi = 0;
    for (let i = 0; i < triCount && o + 47 <= d.length; i++) {
      const lo = dv.getUint32(o, true), hi = dv.getUint32(o + 4, true); o += 8;
      const blend = d[o++], listType = d[o++], tsp = d[o++];
      const textured = (lo !== 0 || hi !== 0);
      const voff = vi / 8;                            // first vertex index of this tri
      for (let k = 0; k < 3; k++) {
        const x = dv.getInt16(o, true); o += 2;
        const y = dv.getInt16(o, true); o += 2;
        const u = dv.getUint16(o, true) / 65535; o += 2;
        const v = dv.getUint16(o, true) / 65535; o += 2;
        const r = d[o++] / 255, g = d[o++] / 255, b = d[o++] / 255, a = d[o++] / 255;
        V[vi++] = x; V[vi++] = y; V[vi++] = u; V[vi++] = v;
        V[vi++] = r; V[vi++] = g; V[vi++] = b; V[vi++] = a;
      }
      tris.push({ key: textured ? SpriteClient.texKey(lo, hi) : null,
                  blend, shadInstr: tsp & 3, ignoreTexA: (tsp >> 2) & 1,
                  textured: (tsp >> 3) & 1, punch: (tsp >> 4) & 1, voff });
    }
    this._stafVCount = vi / 8;
    this.stafQuads = tris;          // array of per-tri draw descriptors (z-ordered op->pt->tr)
    this._stafQuadN = tris.length;
  }

  stafStatsText() {
    return `STAF: frame=${this.stafFrame} tris=${this._stafQuadN} texCache=${this._stafTexN}`;
  }

  // 'OBJS'(4) + count(1) + N×[cid(1), sprite_id(2 LE), type(1), x(i16 LE), y(i16 LE)] = 8B each.
  //
  // EFFECT-BLEND WIRE BYTE (assembly path): the stride may be 8B (legacy) or 9B
  // (8B + blend(1)). The trailing byte is the PVR TSP src/dst blend nibble pair
  // packed as (src<<4 | dst), letting fx/super objects request the matching
  // canvas/WebGPU blend (additive glows). Stride is auto-detected from the packet
  // length so the client consumes whichever the server ships. See "FX-BLEND WIRE
  // BYTE" spec in docs/ASSEMBLY-DRIVEN-DESIGN.md notes (and the bottom of this file).
  static isOBJS(d) {
    return d.length >= 5 && d[0]===79 && d[1]===66 && d[2]===74 && d[3]===83; // 'O','B','J','S'
  }
  onOBJS(d) {
    const n = d[4]; const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
    // Detect per-object stride: 9B if (len-5) == n*9, else 8B. Blend byte present iff 9B.
    const body = d.length - 5;
    const stride = (n > 0 && body === n * 9) ? 9 : 8;
    const hasBlend = stride === 9;
    const objs = []; let o = 5;
    for (let i = 0; i < n && o + stride <= d.length; i++) {
      const raw = dv.getUint16(o+1, true);   // sprite_id with 0x8000 hflip bit
      const ob = { cid: d[o], sid: raw & 0x7fff, type: d[o+3],
                   x: dv.getInt16(o+4, true), y: dv.getInt16(o+6, true) };
      if (hasBlend) {
        const b = d[o+8];
        ob.blend = b;                         // raw packed (src<<4|dst)
        ob.blendSrc = (b >> 4) & 0xf;
        ob.blendDst = b & 0xf;
      }
      objs.push(ob);
      o += stride;
    }
    this._objsPrev = this.objects || [];   // keep last frame for the flicker bridge
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

  // Lazy-load ONE character's PART atlas + assembly table for the assembly path:
  //   <charBase>/PL{hex}_parts.png  — packed part rectangles
  //   <charBase>/PL{hex}_parts.json — { <part_idx>: {x,y,w,h} }  (rect in the atlas)
  //   <charBase>/PL{hex}_asm.json   — { sprite_id: [{part, dx, dy, flip, z?}], ... }
  // The two JSONs are kept separate exactly as the sibling baker emits them. We
  // also accept an optional palette/pal128 in the asm JSON (reuse the palette path).
  loadAsmChar(cid) {
    if (this.asmChars[cid] || this._asmLoading[cid] || !this.charBase) return this._asmLoading[cid];
    const hex = (cid & 0xff).toString(16).padStart(2, '0').toUpperCase();
    const base = `${this.charBase}/PL${hex}`;
    const bust = '?t=' + Date.now();
    const p = (async () => {
      try {
        const [partsJson, asmRaw, blob] = await Promise.all([
          fetch(base + '_parts.json' + bust).then(r => { if (!r.ok) throw new Error('parts.json ' + r.status); return r.json(); }),
          fetch(base + '_asm.json'   + bust).then(r => { if (!r.ok) throw new Error('asm.json '   + r.status); return r.json(); }),
          fetch(base + '_parts.png'  + bust).then(r => { if (!r.ok) throw new Error('parts.png '  + r.status); return r.blob(); }),
        ]);
        const img = await createImageBitmap(blob);
        // asm JSON may be the flat { sid:[...] } map, or wrapped { assemblies:{...}, parts:{...}, palette, pal128, screenW, screenH }.
        const asm   = asmRaw.assemblies || asmRaw.asm || asmRaw;
        const parts = asmRaw.parts || partsJson.parts || partsJson;
        if (asmRaw.screenW) this.screenW = asmRaw.screenW;
        if (asmRaw.screenH) this.screenH = asmRaw.screenH;
        this.asmChars[cid] = {
          img, parts, asm,
          palette: asmRaw.palette || partsJson.palette || null,
          pal128:  asmRaw.pal128  || partsJson.pal128  || null,
          name: asmRaw.name || ('char' + cid),
        };
        console.log('[sprite-client] loaded ASM char', cid, this.asmChars[cid].name,
          Object.keys(parts).length, 'parts,', Object.keys(asm).length, 'assemblies');
      } catch (e) {
        this.asmChars[cid] = { img: null, parts: {}, asm: {}, name: 'char' + cid, err: String(e) };
        console.error('[sprite-client] ASM char', cid, 'load failed', e);
      } finally { delete this._asmLoading[cid]; }
    })();
    this._asmLoading[cid] = p; return p;
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
    for (let s = 0; s < 6; s++) { const sl = this.slot[s]; if (sl.active) {
      if (this.assemblyMode) this.loadAsmChar(sl.char_id); else this.loadChar(sl.char_id);
    } }
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

  // Canvas2D assembly render (A/B fallback when WebGPU is off). Consumes the same
  // draw list as the GPU path; maps the optional fx blend byte to a canvas
  // compositing op (additive for glows). No palette recolor here (GPU path only).
  renderAssembly(ctx) {
    const W = ctx.canvas.width, H = ctx.canvas.height;
    ctx.clearRect(0, 0, W, H);
    const list = this.buildAssemblyDrawList(W, H);
    for (const it of list) {
      const c = this.asmChars[it.charId];
      if (!c || !c.img) continue;
      ctx.save();
      // Additive when the fx blend byte requests dst=ONE (1) — glows/energy.
      if (it.blend != null && (it.blend & 0xf) === 1) ctx.globalCompositeOperation = 'lighter';
      if (it.flip) { ctx.translate(it.dx + it.dw, it.dy); ctx.scale(-1, 1); }
      else         { ctx.translate(it.dx, it.dy); }
      ctx.drawImage(c.img, it.sx, it.sy, it.sw, it.sh, 0, 0, it.dw, it.dh);
      ctx.restore();
    }
    return { drawn: this._asmDrawn, missing: this._asmMiss, note: this._asmNote };
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
      // Anisotropic CPS scale (CpsXScale=5/3, CpsYScale=15/7 — work.asm:44-45):
      // rip sprites are CPS-native px, MVC2 stretches Y MORE than X. Apply SX to
      // every X (anchor offset + width), SY to every Y (offset + height). This is
      // the fixed game scale — NOT the derived "sliding" camera zoom (_zoom, info-only).
      const SX = this.asmScaleX || 1, SY = this.asmScaleY || 1;
      const cfl = (sl.facing !== sp.facing);
      const cdx = cfl ? -(sp.dx + sp.wG) : sp.dx;   // mirror the (asymmetric) anchor when flipped
      out.push({ charId: sl.char_id, slot: s, z: 8, sx: sp.x, sy: sp.y, sw: sp.w, sh: sp.h,
        dx: (exx+cdx*SX)*scaleX, dy: (eyy+sp.dy*SY)*scaleY, dw: sp.wG*SX*scaleX, dh: sp.hG*SY*scaleY,
        flip: cfl });
    }
    // FLICKER-TRANSPARENCY BRIDGE: MVC2 draws some semi-transparent effects/
    // projectiles on ALTERNATING frames to fake alpha. Rendered literally they
    // blink. Bridge it: also draw last frame's objects that have NO match this
    // frame (same cid+sid within ~40px) — so an every-other-frame object renders
    // continuously. Objects still present this frame are skipped here (no trail
    // on things that move every frame). Truly-gone objects drop after one frame.
    let drawObjs = this.objects || [];
    if (this.objectsOn !== false && this._objsPrev && this._objsPrev.length) {
      const held = [];
      for (const p of this._objsPrev) {
        let matched = false;
        for (const o of drawObjs) {
          if (o.cid === p.cid && o.sid === p.sid && Math.abs(o.x - p.x) < 40 && Math.abs(o.y - p.y) < 40) { matched = true; break; }
        }
        if (!matched) held.push(p);
      }
      if (held.length) drawObjs = drawObjs.concat(held);
    }
    // Satellite + global objects from the slot table (cape, projectiles, hail,
    // lightning, supers). The slot table gives each its OWN authoritative screen
    // pos and render layer, so we draw exactly there — no owner-relative guess.
    if (this.objectsOn !== false) for (const o of drawObjs) {
      const c = this.chars[o.cid];
      if (!c) { this.loadChar(o.cid); continue; }
      if (!c.img) continue;
      const sp = c.sprites[o.sid];
      if (!sp) {
        // DIAGNOSTIC: this object's sprite_id isn't in the owner's per-character
        // atlas — it's a SHARED effect sprite (hitspark/etc.) we don't have yet.
        // Tally it; the set is the exact extraction list for the effects atlas.
        const k = `PL${o.cid.toString(16).padStart(2,'0').toUpperCase()}/0x${(o.sid&0xffff).toString(16)}`;
        this._objMiss = this._objMiss || new Map();
        this._objMiss.set(k, (this._objMiss.get(k) || 0) + 1);
        continue;
      }
      // AUTHORITATIVE position: the object's own slot-table screen pos (node+0xE0/E4
      // -> o.x/o.y). The old far>130 heuristic flip-flopped between this and the
      // owner's pos as the object crossed the threshold — that was the 'jumpy/skip'
      // look. Drawing at the true pos is both correct and stable.
      const px = o.x, py = o.y;
      if (px < -64 || px > 704 || py < -64 || py > 544) continue;
      const SX = this.asmScaleX || 1, SY = this.asmScaleY || 1;   // anisotropic CPS scale (work.asm:44-45)
      // Orientation: the object sprite lives in the owner's atlas space, so derive
      // flip from the owner's facing (vs the sprite's baked facing).
      let oslot = 0, ofacing = 0;
      for (let s = 0; s < 6; s++) if (this.slot[s].active && this.slot[s].char_id === o.cid) { oslot = s; ofacing = this.slot[s].facing; break; }
      const fl = (ofacing !== sp.facing);
      const dxv = fl ? -(sp.dx + sp.wG) : sp.dx;   // mirror the anchor when flipped
      // z = the REAL render layer (o.type now carries the slot-table layer 0..15).
      // Bodies sit at the mid baseline (z=8), so low-layer satellites (capes) fall
      // behind their owner and high-layer ones (effects/supers) draw in front.
      const z = o.type;
      out.push({ charId: o.cid, slot: oslot, z, sx: sp.x, sy: sp.y, sw: sp.w, sh: sp.h,
        dx: (px + dxv*SX)*scaleX, dy: (py + sp.dy*SY)*scaleY, dw: sp.wG*SX*scaleX, dh: sp.hG*SY*scaleY,
        flip: fl });
    }
    // The renderer groups CONSECUTIVE same-cid sprites and drops chars past maxGroups(8).
    // Sort by cid so each character's body+objects form ONE group, objects (z=-1) behind
    // bodies (z=0). (unshift broke this by scattering mixed-cid objects to the front.)
    out.sort((a, b) => (a.charId - b.charId) || ((a.z || 0) - (b.z || 0)));
    // Periodically dump the shared-effect miss tally to the console — this is the
    // exact list of effect sprite_ids to put in the effects atlas. (window._objMiss
    // also holds it live for inspection.)
    if ((this._dlc = (this._dlc || 0) + 1) % 180 === 0 && this._objMiss && this._objMiss.size) {
      const top = [...this._objMiss.entries()].sort((a,b)=>b[1]-a[1]).slice(0,40);
      console.warn('[effects-miss] sprite_ids not in any per-char atlas (cid/sid x frames):',
        top.map(([k,v])=>`${k}×${v}`).join('  '));
      if (typeof window !== 'undefined') window._objMiss = this._objMiss;
    }
    const missEff = this._objMiss ? this._objMiss.size : 0;
    this._lastNote = loading ? `loading ${loading} char atlas…`
      : (missing ? `holding ${missing}: ${missKeys.join(' ')}`
      : (missEff ? `all poses ok · ${missEff} effect sids missing (see console)` : 'all visible poses captured'));
    return out;
  }

  // ===== ASSEMBLY DRAW LIST (the parallel path) =====
  // Same owners as buildDrawList (6 bodies + N pool objects), but each owner's
  // sprite_id resolves to an ASSEMBLY (a list of part placements) and we emit one
  // quad per part. Output items match buildDrawList's shape so sprite-gpu.mjs
  // consumes them unchanged — plus an optional `blend` for fx objects:
  //   { charId, slot, z, sx,sy,sw,sh (atlas px), dx,dy,dw,dh (canvas px), flip, blend? }
  buildAssemblyDrawList(canvasW, canvasH) {
    const scaleX = canvasW / (this.screenW || 640), scaleY = canvasH / (this.screenH || 480);
    const now = (typeof performance !== 'undefined') ? performance.now() : 0;
    const SX = this.asmScaleX || 1, SY = this.asmScaleY || 1;
    const S  = this.spriteScale || 1;
    const out = [];
    let loading = 0, missing = 0, drawn = 0, missKeys = [];

    // Emit every part of `sprite_id`'s assembly for one owner (body or pool obj).
    // owner: { cid, exx, eyy, facing, slot, zBase, blend? }
    const emitAssembly = (owner, sid) => {
      const c = this.asmChars[owner.cid];
      if (!c) { this.loadAsmChar(owner.cid); loading++; return; }
      if (!c.img) return;                          // load failed
      const recs = c.asm[sid] || c.asm[sid & 0xffff] || c.asm[String(sid)];
      if (!recs || !recs.length) {
        missing++; if (missKeys.length < 3) missKeys.push(`${owner.cid}/0x${(sid&0xffff).toString(16)}`);
        return;
      }
      for (const r of recs) {
        const part = c.parts[r.part] || c.parts[String(r.part)];
        if (!part) continue;
        // flip = owner facing XOR the record's own flip bit.
        const flip = (!!owner.facing) !== (!!r.flip);
        // Part offset is in game px relative to the owner's screen anchor. When
        // mirrored, reflect the part's x-extent across the anchor (dx -> -(dx+w)).
        const pdx = flip ? -(r.dx + part.w) : r.dx;
        const dx = (owner.exx + pdx * SX * S) * scaleX;
        const dy = (owner.eyy + r.dy * SY * S) * scaleY;
        const dw = part.w * SX * S * scaleX;
        const dh = part.h * SY * S * scaleY;
        // z: owner base layer + the record's own intra-assembly z (back-to-front).
        const z = (owner.zBase || 0) * 100 + (r.z || 0);
        const item = { charId: owner.cid, slot: owner.slot, z,
          sx: part.x, sy: part.y, sw: part.w, sh: part.h,
          dx, dy, dw, dh, flip };
        if (owner.blend != null) item.blend = owner.blend;
        out.push(item);
        drawn++;
      }
    };

    // --- bodies (the 6 tracked slots) ---
    for (let s = 0; s < 6; s++) {
      const sl = this.slot[s];
      if (!sl.active) continue;
      let exx = sl.screen_x, eyy = sl.screen_y;
      if (this.predict !== false) { const dt = Math.min(now - sl.t, 33); if (dt > 0) { exx += sl.vx*dt; eyy += sl.vy*dt; } }
      emitAssembly({ cid: sl.char_id, exx, eyy, facing: sl.facing, slot: s, zBase: 0 }, sl.sprite_id);
    }

    // --- pool objects (cape / projectile / fx) — each its own assembly + blend ---
    if (this.objectsOn !== false) for (const o of (this.objects || [])) {
      // Find owner slot for the shared transform (same logic as buildDrawList).
      let osl = null;
      for (let s = 0; s < 6; s++) if (this.slot[s].active && this.slot[s].char_id === o.cid) { osl = this.slot[s]; break; }
      if (!osl) continue;
      let ox = osl.screen_x, oy = osl.screen_y;
      if (this.predict !== false) { const dt = Math.min(now - osl.t, 33); if (dt > 0) { ox += osl.vx*dt; oy += osl.vy*dt; } }
      if ((ox === 0 && oy === 0) || ox < -60 || ox > 700) continue;
      // type 3 = cape: rides the owner. Others: distance decides attached vs spawned.
      const far = (o.type !== 3) && ((Math.abs(o.x - ox) + Math.abs(o.y - oy)) > 130);
      const px = far ? o.x : ox, py = far ? o.y : oy;
      // z layer by category: cape (3) behind body, fx/lightning (1) in front, else just behind.
      const zBase = (o.type === 1) ? 1 : (o.type === 3 ? -2 : -1);
      emitAssembly({ cid: o.cid, exx: px, eyy: py, facing: osl.facing, slot: 0,
                     zBase, blend: o.blend }, o.sid);
    }

    out.sort((a, b) => (a.charId - b.charId) || ((a.z || 0) - (b.z || 0)));
    this._asmDrawn = drawn; this._asmMiss = missing;
    this._asmNote = loading ? `assembly: loading ${loading} char atlas…`
                  : missing ? `assembly: missing ${missing} asm: ${missKeys.join(' ')} (${drawn} parts)`
                  : `assembly: ${drawn} parts drawn`;
    this._lastNote = this._asmNote;
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
      // Draw only the quad's UV sub-rect of the shared EFKYTEX page (not the whole sheet).
      const tw = tex.width, th = tex.height;
      const swp = (e.u1 - e.u0) * tw, shp = (e.v1 - e.v0) * th;
      if (e.u1 != null && swp > 0.5 && shp > 0.5)
        ctx.drawImage(tex, e.u0 * tw, e.v0 * th, swp, shp, e.cx * sx - dw / 2, e.cy * sy - dh / 2, dw, dh);
      else
        ctx.drawImage(tex, e.cx * sx - dw / 2, e.cy * sy - dh / 2, dw, dh);  // fallback (no UV)
    }
    ctx.restore();
  }

  // Hit-flash: MVC2 swaps the VICTIM's body to a "hurt" palette bank (Dat_Pal+0x300,
  // white; electric -> blue-white) for the hit-reaction frames — it's ON the body,
  // not a separate sprite (per bank03:loc_8c035000). We approximate by drawing an
  // ADDITIVE tinted silhouette of each flashing body (slot.paleffect != 0, via PALF),
  // reusing the body draw-list geometry so it lands exactly on the sprite.
  drawFlash(ctx, drawList) {
    if (!drawList || !drawList.length) return;
    let any = false;
    for (let s = 0; s < 6; s++) { const sl = this.slot[s]; if (sl.active && sl.paleffect) { any = true; break; } }
    if (!any) return;
    if (!this._flashTmp) this._flashTmp = document.createElement('canvas');
    const tmp = this._flashTmp, tctx = tmp.getContext('2d');
    ctx.save();
    ctx.globalCompositeOperation = 'lighter';   // additive — brightens the body toward the flash color
    for (const it of drawList) {
      if (it.slot == null) continue;
      const sl = this.slot[it.slot];
      if (!sl || !sl.active || !sl.paleffect) continue;
      const c = this.chars[it.charId]; if (!c || !c.img || it.sw <= 0 || it.sh <= 0) continue;
      // Build a solid-tint silhouette of the sprite's alpha (keeps the body shape).
      tmp.width = it.sw; tmp.height = it.sh;
      tctx.globalCompositeOperation = 'source-over'; tctx.clearRect(0, 0, it.sw, it.sh);
      tctx.drawImage(c.img, it.sx, it.sy, it.sw, it.sh, 0, 0, it.sw, it.sh);
      tctx.globalCompositeOperation = 'source-in';
      tctx.fillStyle = '#cfe0ff';                // electric/white hit-flash tint
      tctx.fillRect(0, 0, it.sw, it.sh);
      ctx.globalAlpha = 0.7;
      if (it.flip) { ctx.save(); ctx.translate(it.dx + it.dw, it.dy); ctx.scale(-1, 1); ctx.drawImage(tmp, 0, 0, it.dw, it.dh); ctx.restore(); }
      else ctx.drawImage(tmp, it.dx, it.dy, it.dw, it.dh);
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
    const loaded = this.assemblyMode ? Object.keys(this.asmChars) : Object.keys(this.chars);
    const src = this.assemblyMode ? this.asmChars : this.chars;
    const names = loaded.map(c => src[c].name + (src[c].img?'':'!')).join(', ') || '(none yet)';
    const nm = (i) => { const c = this.chars[this.slot[i].char_id]; return c ? c.name : '…'; };
    const LAB = ['P1a','P2a','P1b','P2b','P1c','P2c'];
    const sl = (i) => { const x = this.slot[i];
      return `${LAB[i]} ${x.active?'ON ':'-- '} ${nm(i)}(${x.char_id}) sid=0x${(x.sprite_id&0xffff).toString(16).padStart(4,'0')} f${x.facing}`; };
    const kbps = this._bwRate/1024;
    const mbps = (this._bwRate*8/1e6).toFixed(3);
    if (kbps > (this._bwPeak||0)) this._bwPeak = kbps;       // track peak this session
    const vsMirror = kbps > 0.01 ? (1700/kbps).toFixed(0)+'x cheaper than mirror' : '(waiting for GSTA…)';
    const mode = this.assemblyMode ? 'ASSEMBLY (parts)' : 'WHOLE-SPRITE';
    return `SPRITE CLIENT [${mode}] — ${loaded.length} char atlas loaded\n`
         + `loaded: ${names}\n`
         + `━━━ BANDWIDTH: ${kbps.toFixed(2)} KB/s (${mbps} Mbps) ━━━\n`
         + `      peak ${(this._bwPeak||0).toFixed(2)} KB/s · ${vsMirror} (~1700 KB/s)\n`
         + `size=${(this.spriteScale||1).toFixed(2)}x  zoom(info,not applied)=${(this._zoom||1).toFixed(2)}\n`
         + `  ${this._bwHz.toFixed(0)} Hz x ${this._lastSize} B/frame\n`
         + `inMatch=${this.inMatch}\n` + [0,1,2,3,4,5].map(sl).join('\n') + '\n'
         + (this._lastNote ? `note: ${this._lastNote}` : '');
  }
}

// =============================================================================
// FX-BLEND WIRE BYTE — spec (the "merge" for pixel-exact supers/energy)
// =============================================================================
//
// The GSTA OBJS packet gains an OPTIONAL trailing byte per pool object, so fx /
// super / energy objects can request the exact PVR blend the game used. The
// client auto-detects the stride from the packet length — no version flag — so an
// old 8B server and a new 9B server both work against the same client.
//
//   OBJS packet:  'OBJS'(4) + count(1) + count × OBJ
//   OBJ (legacy): cid(1) + sprite_id(2 LE) + type(1) + x(i16 LE) + y(i16 LE)        = 8 B
//   OBJ (blend):  cid(1) + sprite_id(2 LE) + type(1) + x(i16 LE) + y(i16 LE) + blend(1) = 9 B
//
//   Stride is 9 iff (packetLen - 5) == count*9, else 8. (count*8 and count*9 can
//   only collide when count==0, which carries no objects — safe.)
//
// blend byte = (srcFactor << 4) | dstFactor, the PVR TSP instruction word's
// SRC_ALPHA_INSTR (bits 29-31) and DST_ALPHA_INSTR (bits 26-28), each a 3-bit
// PVR blend code packed into a nibble:
//
//   PVR code  meaning            WebGPU GPUBlendFactor   Canvas2D
//   0  ZERO                      'zero'
//   1  ONE                       'one'                   'lighter' (additive) when DST
//   2  OTHER (dst/src color)     'src'/'dst' color
//   3  INVERSE OTHER             'one-minus-…-color'
//   4  SRC ALPHA                 'src-alpha'             'source-over' (default)
//   5  INVERSE SRC ALPHA         'one-minus-src-alpha'
//   6  DST ALPHA                 'dst-alpha'
//   7  INVERSE DST ALPHA         'one-minus-dst-alpha'
//
// The common cases:
//   0x45 = src=SRC_ALPHA(4), dst=INV_SRC_ALPHA(5)  -> normal alpha (default; omit byte)
//   0x11 = src=ONE(1),       dst=ONE(1)            -> additive glow (supers/energy)
//   0x41 = src=SRC_ALPHA(4), dst=ONE(1)            -> premultiplied additive
//
// Client mapping: sprite-gpu.mjs picks the additive pipeline when dst==ONE (the
// glow case); Canvas2D uses globalCompositeOperation='lighter' for dst==ONE.
// Everything else falls back to normal alpha. Default (no byte) == 0x45.
// =============================================================================
