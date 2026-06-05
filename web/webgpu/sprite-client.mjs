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
    this.atlas = null;   // parsed bake_atlas.json
    this.img = null;     // ImageBitmap of bake_atlas.png
    this.inMatch = 0;
    this.slot = [this._blank(), this._blank()];
    this._lastNote = 'no atlas loaded';
    // State-stream (GSTA) bandwidth — the whole point of Option 6. Rolling 1s window.
    this._bwBytes = 0; this._bwFrames = 0; this._bwT0 = 0;
    this._bwRate = 0; this._bwHz = 0; this._lastSize = 0;
  }
  _blank() {
    return { active:0, char_id:0, sprite_id:-1, screen_x:0, screen_y:0, facing:0, palette:0 };
  }

  static isGSTA(d) {
    return d.length >= 4 && d[0]===GSTA_MAGIC[0] && d[1]===GSTA_MAGIC[1]
        && d[2]===GSTA_MAGIC[2] && d[3]===GSTA_MAGIC[3];
  }

  // atlasJson: parsed object; pngBlob: a Blob/File of the atlas PNG.
  async loadAtlas(atlasJson, pngBlob) {
    this.atlas = atlasJson;
    this.img = await createImageBitmap(pngBlob);
    this._lastNote = '';
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
    for (let s = 0; s < 2; s++) {
      const ci = B + 25 + s * 38;      // 25-byte global header + 38*slot
      const sl = this.slot[s];
      sl.active   = dv.getUint8(ci + 0);
      sl.char_id  = dv.getUint8(ci + 1);
      sl.facing   = dv.getUint8(ci + 2);
      sl.palette  = dv.getUint8(ci + 7);
      sl.screen_x = dv.getFloat32(ci + 16, true);
      sl.screen_y = dv.getFloat32(ci + 20, true);
      sl.sprite_id= dv.getUint16(ci + 32, true);
    }
  }

  // Draw the current state into a 2D context. Returns a small status object.
  render(ctx) {
    const W = ctx.canvas.width, H = ctx.canvas.height;
    ctx.clearRect(0, 0, W, H);
    if (!this.atlas || !this.img) return { drawn:0, missing:0, note:'no atlas loaded' };
    const sx = W / (this.atlas.screenW || 640);
    const sy = H / (this.atlas.screenH || 480);
    let drawn = 0, missing = 0, missKeys = [];

    if (!this._held) this._held = [null, null];   // last drawn sprite per slot
    for (let s = 0; s < 2; s++) {
      const sl = this.slot[s];
      if (!sl.active) { this._held[s] = null; continue; }
      const c = this.atlas.chars[sl.char_id];
      let sp = c && c.sprites[sl.sprite_id];
      if (sp) {
        this._held[s] = { char_id: sl.char_id, sp };
      } else {
        // Sparse atlas: this sprite_id wasn't baked. Hold the last known pose
        // for this character (same char_id) instead of blanking → no blink.
        missing++;
        if (missKeys.length < 3) missKeys.push(`${sl.char_id}/0x${(sl.sprite_id&0xffff).toString(16)}`);
        const h = this._held[s];
        if (h && h.char_id === sl.char_id) sp = h.sp; else continue;
      }

      // Destination in game space, scaled to the canvas.
      const gx = sl.screen_x + sp.dx, gy = sl.screen_y + sp.dy;
      const dx = gx * sx, dy = gy * sy, dw = sp.wG * sx, dh = sp.hG * sy;
      const flip = (sl.facing !== sp.facing);

      ctx.save();
      if (flip) {
        // Mirror horizontally about the character's screen_x.
        const axis = sl.screen_x * sx;
        ctx.translate(axis, 0); ctx.scale(-1, 1); ctx.translate(-axis, 0);
      }
      ctx.drawImage(this.img, sp.x, sp.y, sp.w, sp.h, dx, dy, dw, dh);
      ctx.restore();
      drawn++;
    }
    this._lastNote = missing ? `holding (uncaptured) ${missing}: ${missKeys.join(' ')}` : 'all visible poses captured';
    return { drawn, missing, note:this._lastNote };
  }

  statsText() {
    const a = this.atlas;
    const nChars = a ? Object.keys(a.chars).length : 0;
    let nSprites = 0; if (a) for (const k in a.chars) nSprites += Object.keys(a.chars[k].sprites).length;
    const sl = (i) => { const x = this.slot[i];
      return `S${i} ${x.active?'act':'---'} char=${x.char_id} sid=0x${(x.sprite_id&0xffff).toString(16).padStart(4,'0')} fac=${x.facing}`; };
    const kbps = (this._bwRate/1024).toFixed(2);
    const mbps = (this._bwRate*8/1e6).toFixed(3);
    return `SPRITE CLIENT ${this.img?'atlas loaded':'NO ATLAS'}\n`
         + `atlas: ${nChars} chars, ${nSprites} sprites\n`
         + `STATE STREAM: ${kbps} KB/s (${mbps} Mbps)\n`
         + `  ${this._bwHz.toFixed(0)} Hz x ${this._lastSize} B/frame\n`
         + `inMatch=${this.inMatch}\n${sl(0)}\n${sl(1)}\n`
         + (this._lastNote ? `note: ${this._lastNote}` : '');
  }
}
