// bake.mjs — sprite bake harness for the WebGPU renderer.
//
// Goal: with the renderer in customBg mode (characters isolated on a
// transparent background, no stage / no HUD), capture each character frame
// keyed by the sprite_id the game reports, and verify that the same
// (char_id, sprite_id) renders identical pixels every time.
//
// Two inputs:
//   onGSTA(frame)     — the server's 'GSTA' state broadcast (char_id, sprite_id,
//                       screen pos, facing). Wire layout mirrors
//                       core/network/maplecast_gamestate.cpp serialize().
//   onRendered(canvas)— called right after each R.renderFrame so we can read
//                       the just-rendered (isolated) frame off the WebGPU canvas.
//
// Skew note: GSTA is broadcast every 3 emu frames (~20Hz) while TA frames are
// 60Hz, so the latest sprite_id can be a couple frames stale vs the rendered
// TA frame. We only capture a sprite once it is SETTLED (held for >= SETTLE
// consecutive GSTA samples), so the rendered frame is guaranteed to be showing
// that sprite — eliminating the skew for the captured set.

const MVC2_CHARS = [
  'Ryu','Zangief','Guile','Morrigan','Anakaris','Strider','Cyclops','Wolverine',
  'Psylocke','Iceman','Rogue','Captain America','Spider-Man','Hulk','Venom','Dr. Doom',
  'Tron Bonne','Jill','Hayato','Ruby Heart','SonSon','Amingo','Marrow','Cable',
  'Abyss I','Abyss II','Abyss III','Chun-Li','Megaman','Roll','Akuma','B.B. Hood',
  'Felicia','Charlie','Sakura','Dan','Cammy','Dhalsim','M. Bison','Ken',
  'Gambit','Juggernaut','Storm','Sabretooth','Magneto','Shuma-Gorath','War Machine','Silver Samurai',
  'Omega Red','Spiral','Colossus','Iron Man','Sentinel','Blackheart','Thanos','Jin'
];
const charName = (id) => MVC2_CHARS[id] || `char${id}`;
// Capture only when the rendered crop has been byte-identical for this many
// consecutive frames -> the character (and everything in the crop) is visually
// HELD. This sidesteps GSTA<->render skew and any animating residual: if the
// HUD/lines/character are moving, the crop changes and we simply don't capture.
// One sample is recorded per held period (rising edge), so a sprite that recurs
// across multiple holds gives a real cross-occurrence stability test.
const STATIC_HOLD = 3;
// Tolerance for the "held" test: the crop counts as unchanged if at most this
// FRACTION of its pixels differ from the previous frame (per-channel noise
// floor of 8/255). Exact byte-identity (0) was too strict on the live prod
// stream — translucent shadow/super-meter shimmer + dither flipped a handful
// of pixels every frame, so the hold counter never advanced and nothing was
// captured. A tiny tolerance still requires the character to be visually held.
const STATIC_TOL = 0.004;   // 0.4% of pixels

export class SpriteBake {
  constructor() {
    this.on = false;
    // Which render slot to bake: 0 = P1 only (default — your character),
    // 1 = P2 only, -1 = both. Restricting to your own (idle) character keeps
    // the moving opponent's unstable, mid-animation captures out of the atlas.
    this.slotFilter = 0;
    // Full-coverage capture: grab the first clean frame of every sprite_id
    // (vs. the default 3-frame "held" gate). For building a complete atlas to
    // mirror the animation. Furniture is poly-filtered out, so frames are clean.
    this.captureAll = false;
    this.inMatch = 0;
    // per render-slot (0=P1 point, 1=P2 point) live + settle tracking
    this.slot = [ this._blankSlot(), this._blankSlot() ];
    // capture store: key "slot|char|sprite" -> { char_id, sprite_id, slot,
    //   hashes:Map<hashHex,count>, occ:count, firstImg:ImageData }
    this.caps = new Map();
    // fixed per-slot crop boxes (fractions of the 640x480 render space).
    // P1 stands left-of-centre, P2 right-of-centre. Mirror for slot 1.
    this.box = { x0:0.10, y0:0.22, x1:0.50, y1:0.97 };
    // 2D scratch canvas for reading the WebGPU canvas
    this._c2 = document.createElement('canvas');
    this._x2 = this._c2.getContext('2d', { willReadFrequently:true });
  }
  _blankSlot(){ return { active:0, char_id:0, sprite_id:-1, screen_x:0, screen_y:0,
                         facing:0, staticRun:0, prevPix:null, prevW:0, prevH:0, lastDiff:1 }; }

  // --- GSTA intake ------------------------------------------------------
  // d: Uint8Array, d[0..3]='GSTA', then 261-byte serialized GameState.
  static isGSTA(d){ return d.length>=4 && d[0]===71 && d[1]===83 && d[2]===84 && d[3]===65; }

  onGSTA(d){
    const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
    const B = 4;                       // payload starts after 'GSTA'
    this.inMatch = dv.getUint8(B+0);
    for (let s=0; s<2; s++){
      const ci = B + 25 + s*49;        // char block: 25-byte global header + 49*slot (GSTA enrich)
      const sl = this.slot[s];
      sl.active    = dv.getUint8(ci+0);
      sl.char_id   = dv.getUint8(ci+1);
      sl.facing    = dv.getUint8(ci+2);
      sl.screen_x  = dv.getFloat32(ci+16, true);
      sl.screen_y  = dv.getFloat32(ci+20, true);
      sl.sprite_id = dv.getUint16(ci+32, true);
    }
  }

  // --- capture ----------------------------------------------------------
  // With the renderer isolating characters (translucent-only -> transparent
  // background, no floor), we crop TIGHT to each character's alpha bounding
  // box. That makes the capture position-independent: the same sprite_id
  // yields the same tight crop no matter where the camera placed it, so a
  // held pose hashes identically. Slot 0 searches the left half, slot 1 the
  // right half, to keep the two characters apart.
  onRendered(webgpuCanvas){
    if (!this.on || !this.inMatch) return;
    const W = webgpuCanvas.width, H = webgpuCanvas.height;
    if (!W || !H) return;
    if (this._c2.width !== W || this._c2.height !== H){ this._c2.width=W; this._c2.height=H; }
    this._x2.clearRect(0,0,W,H);
    try { this._x2.drawImage(webgpuCanvas, 0, 0, W, H); }
    catch(e){ this._lastErr = String(e); return; }
    let full; try { full = this._x2.getImageData(0,0,W,H); } catch(e){ this._lastErr=String(e); return; }
    const px = full.data;

    for (let s=0; s<2; s++){
      if (this.slotFilter>=0 && s!==this.slotFilter) continue;  // skip the slot we're not baking
      const sl = this.slot[s];
      if (!sl.active){ sl.staticRun=0; sl.prevPix=null; continue; }
      // Search region: centered on the game-reported screen position
      // (X/Y_Position_Screen, offsets 0xE0/0xE4). The game screen space is
      // ~640x480; map to canvas. This excludes the frame-wide translucent
      // smear and tracks the character as it moves. Alpha-bbox then tightens
      // to the character WITHIN this region. Falls back to slot-half if the
      // position looks unpopulated (0 / out of range).
      let cx = sl.screen_x * (W/640), cy = sl.screen_y * (H/480);
      const posOK = (sl.screen_x>1 && sl.screen_x<640 && sl.screen_y>1 && sl.screen_y<480);
      let hx0, hx1, vy0, vy1;
      if (posOK) {
        // RD (downward extent) is tight on purpose: screen_y is ~the feet, and
        // BELOW the feet live the perpetually-animated translucent HUD super-
        // meter bar + the floor shadow. Letting the alpha-bbox reach them made
        // the crop change every frame (hold stuck at 1) even when the character
        // pose was held. Stop ~just below the feet so only the body is hashed.
        const RW = W*0.16, RU = H*0.55, RD = H*0.02;  // up = head/jump room; down = feet only
        hx0 = Math.max(0, (cx-RW)|0); hx1 = Math.min(W, (cx+RW)|0);
        vy0 = Math.max(0, (cy-RU)|0); vy1 = Math.min(H, (cy+RD)|0);
      } else {
        const other = this.slot[s^1].active;
        hx0 = !other ? 0 : (s===0 ? 0 : (W>>1));
        hx1 = !other ? W : (s===0 ? (W>>1) : W);
        vy0 = 0; vy1 = H;
      }
      let minx=W, miny=H, maxx=-1, maxy=-1;
      for (let y=vy0; y<vy1; y++){
        const ro = y*W*4;
        for (let x=hx0; x<hx1; x++){
          if (px[ro + x*4 + 3] > 16){          // alpha > 16 => character pixel
            if (x<minx) minx=x; if (x>maxx) maxx=x;
            if (y<miny) miny=y; if (y>maxy) maxy=y;
          }
        }
      }
      if (maxx<minx) { this._diag=`slot${s}: NO pixels in region (sx=${sl.screen_x.toFixed(0)} sy=${sl.screen_y.toFixed(0)})`; sl.staticRun=0; sl.prevPix=null; continue; }
      const cw=maxx-minx+1, ch=maxy-miny+1;
      if (cw<8 || ch<8) { sl.staticRun=0; continue; }    // too small => noise
      let img; try { img = this._x2.getImageData(minx,miny,cw,ch); } catch(e){ this._lastErr=String(e); continue; }
      const hash = this._fnv(img.data);

      // Rendered-"static" gate with tolerance. The crop counts as HELD when it
      // matches the previous frame in size AND differs by <= STATIC_TOL of its
      // pixels. (Exact byte-identity was too strict on the live stream — see
      // STATIC_TOL.) A changed bbox size means the scene moved → reset.
      const cur = img.data;
      const dimsMatch = (sl.prevW===cw && sl.prevH===ch && sl.prevPix && sl.prevPix.length===cur.length);
      let diffFrac = 1;
      if (dimsMatch) {
        let diff = 0; const npx = cur.length>>2; const pp = sl.prevPix;
        for (let i=0;i<cur.length;i+=4){
          if (Math.abs(cur[i]-pp[i])>8 || Math.abs(cur[i+1]-pp[i+1])>8 ||
              Math.abs(cur[i+2]-pp[i+2])>8 || Math.abs(cur[i+3]-pp[i+3])>8) diff++;
        }
        diffFrac = diff / npx;
      }
      sl.prevPix = cur.slice(); sl.prevW = cw; sl.prevH = ch; sl.lastDiff = diffFrac;

      const held = (dimsMatch && diffFrac <= STATIC_TOL);
      if (held) sl.staticRun++; else sl.staticRun = 1;
      const key = `${s}|${sl.char_id}|${sl.sprite_id}`;
      // Capture rule:
      //  - static-hold (default): only a pose HELD for STATIC_HOLD frames →
      //    verification-grade, byte-stable captures (idle).
      //  - captureAll (full coverage): the FIRST clean frame of each new
      //    sprite_id. Furniture is filtered at the poly level, so any frame's
      //    crop is already the clean character — enough to build a complete
      //    atlas for the animation-match test. At 20Hz state this matches the
      //    poses the stream actually reports; 60Hz state makes motion smooth.
      const doCapture = this.captureAll ? !this.caps.has(key)
                                        : (held && sl.staticRun === STATIC_HOLD);
      if (doCapture) {
        let rec = this.caps.get(key);
        if (!rec){
          // Anchor: offset from the game-reported screen pos to the crop's
          // top-left, in GAME space (640x480) so the atlas is resolution-
          // independent. facing records which way the char faced at capture.
          rec = { slot:s, char_id:sl.char_id, sprite_id:sl.sprite_id,
                  hashes:new Map(), occ:0, firstImg:img, w:cw, h:ch,
                  facing:sl.facing,
                  dx:(minx*640/W - sl.screen_x), dy:(miny*480/H - sl.screen_y),
                  wG:(cw*640/W), hG:(ch*480/H) };
          this.caps.set(key, rec);
        }
        rec.occ++;
        rec.hashes.set(hash, (rec.hashes.get(hash)||0)+1);
      }
      this._diag = `slot${s} sx=${sl.screen_x.toFixed(0)} sy=${sl.screen_y.toFixed(0)} crop ${cw}x${ch} hold=${sl.staticRun} diff=${(diffFrac*100).toFixed(1)}%`;
    }
  }

  // Fast dual 32-bit hash (Math.imul) over RGBA bytes -> 16-hex string.
  // ~64-bit effective; collision risk negligible for equality checks, and far
  // faster than BigInt so it won't tank the render loop on ~200KB crops/frame.
  _fnv(b){
    let h1=0x811c9dc5, h2=(0x811c9dc5^0x5bd1e995)>>>0;
    for (let i=0;i<b.length;i++){ const c=b[i];
      h1=Math.imul(h1^c,0x01000193); h2=Math.imul(h2^c,0x85ebca6b); }
    return (h1>>>0).toString(16).padStart(8,'0')+(h2>>>0).toString(16).padStart(8,'0');
  }

  // --- reporting --------------------------------------------------------
  stats(){
    let keys=0, recur=0, stable=0, unstable=0;
    for (const rec of this.caps.values()){
      keys++;
      if (rec.occ>=2){ recur++; if (rec.hashes.size===1) stable++; else unstable++; }
    }
    const pct = recur? (100*stable/recur) : 0;
    return { keys, recur, stable, unstable, pct,
             p1:[...this.caps.values()].filter(r=>r.slot===0).length,
             p2:[...this.caps.values()].filter(r=>r.slot===1).length,
             err:this._lastErr||'' };
  }
  statsText(){
    const s=this.stats();
    const sl=(i)=>{const x=this.slot[i];return `S${i} ${x.active?'act':'---'} sid=0x${(x.sprite_id&0xffff).toString(16).padStart(4,'0')} hold=${x.staticRun}`;};
    let t = `BAKE ${this.on?'ON':'off'}  v5-static\n`;
    t += `${sl(0)}\n${sl(1)}\n`;
    t += `last: ${this._diag||'(no capture yet)'}\n`;
    t += `captured keys : ${s.keys}  (P1 ${s.p1}, P2 ${s.p2})\n`;
    t += `recurred(>=2) : ${s.recur}\n`;
    t += `pixel-STABLE  : ${s.stable}\n`;
    t += `pixel-UNSTABLE: ${s.unstable}\n`;
    t += `STABILITY     : ${s.pct.toFixed(2)}%`;
    if (s.unstable===0 && s.recur>0) t += '  <-- GO';
    if (s.err) t += `\nlastErr: ${s.err}`;
    return t;
  }

  // --- export -----------------------------------------------------------
  // Montage PNG of every captured sprite's first crop + a manifest CSV.
  downloadMontage(){
    const recs=[...this.caps.values()];
    if(!recs.length){ alert('no captures yet'); return; }
    // tight crops are variable-sized -> grid on the max cell size
    const pad=4;
    const cw=Math.max(...recs.map(r=>r.firstImg.width))+pad;
    const ch=Math.max(...recs.map(r=>r.firstImg.height))+pad;
    const cols=Math.ceil(Math.sqrt(recs.length)), rows=Math.ceil(recs.length/cols);
    const cv=document.createElement('canvas'); cv.width=cols*cw; cv.height=rows*ch;
    const cx=cv.getContext('2d');
    recs.forEach((r,i)=>{ const col=i%cols, row=(i/cols)|0; cx.putImageData(r.firstImg, col*cw, row*ch); });
    cv.toBlob(b=>{ const u=URL.createObjectURL(b); const a=document.createElement('a');
      a.href=u; a.download='bake_montage.png'; a.click(); URL.revokeObjectURL(u); });
    let csv='slot,char_id,char_name,sprite_id,occurrences,distinct_hashes,stable,hash0\n';
    for(const r of recs){ const hs=[...r.hashes.keys()];
      csv+=`${r.slot},${r.char_id},${charName(r.char_id)},0x${r.sprite_id.toString(16).padStart(4,'0')},`
          +`${r.occ},${r.hashes.size},${r.hashes.size===1?1:0},${hs[0]}\n`; }
    const cu=URL.createObjectURL(new Blob([csv],{type:'text/csv'}));
    const ca=document.createElement('a'); ca.href=cu; ca.download='bake_manifest.csv'; ca.click(); URL.revokeObjectURL(cu);
  }
  // Client-usable atlas: a packed PNG + a JSON sprite table keyed by
  // char_id -> sprite_id -> {x,y,w,h (atlas px), dx,dy,wG,hG (game space),
  // facing}. This is what the Sprite Client (sprite-client.mjs) consumes to
  // render characters from the 253-byte GSTA state with no TA stream.
  // Distinct from downloadMontage() (a flat verification contact-sheet): this
  // dedupes by (char_id,sprite_id) across render slots and carries the anchor
  // geometry a renderer needs.
  downloadAtlas(){
    const recs=[...this.caps.values()];
    if(!recs.length){ alert('no captures yet'); return; }
    // Dedupe by char|sprite (merge P1/P2 slots; first capture wins). The game
    // stores one sprite_id regardless of facing, so one canonical crop per key.
    const byKey=new Map();
    for(const r of recs){ const k=r.char_id+'|'+r.sprite_id; if(!byKey.has(k)) byKey.set(k,r); }
    const uniq=[...byKey.values()];
    const pad=2;
    const cw=Math.max(...uniq.map(r=>r.firstImg.width))+pad;
    const ch=Math.max(...uniq.map(r=>r.firstImg.height))+pad;
    const cols=Math.ceil(Math.sqrt(uniq.length)), rows=Math.ceil(uniq.length/cols);
    const cv=document.createElement('canvas'); cv.width=cols*cw; cv.height=rows*ch;
    const cx=cv.getContext('2d');
    const atlas={ screenW:640, screenH:480, image:'bake_atlas.png', chars:{} };
    uniq.forEach((r,i)=>{ const col=i%cols, row=(i/cols)|0, px=col*cw, py=row*ch;
      cx.putImageData(r.firstImg, px, py);
      const c=atlas.chars[r.char_id]||(atlas.chars[r.char_id]={name:charName(r.char_id),sprites:{}});
      c.sprites[r.sprite_id]={ x:px, y:py, w:r.firstImg.width, h:r.firstImg.height,
                               dx:+r.dx.toFixed(2), dy:+r.dy.toFixed(2),
                               wG:+r.wG.toFixed(2), hG:+r.hG.toFixed(2), facing:r.facing };
    });
    cv.toBlob(b=>{ const u=URL.createObjectURL(b); const a=document.createElement('a');
      a.href=u; a.download='bake_atlas.png'; a.click(); URL.revokeObjectURL(u); });
    const ju=URL.createObjectURL(new Blob([JSON.stringify(atlas)],{type:'application/json'}));
    const ja=document.createElement('a'); ja.href=ju; ja.download='bake_atlas.json'; ja.click(); URL.revokeObjectURL(ju);
  }
  reset(){ this.caps.clear(); this.slot=[this._blankSlot(),this._blankSlot()]; this._lastErr=''; }
}
