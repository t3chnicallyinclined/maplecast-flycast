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
const SETTLE = 3;  // GSTA samples a sprite must hold before we trust the render

export class SpriteBake {
  constructor() {
    this.on = false;
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
                         facing:0, settleId:-1, settleN:0 }; }

  // --- GSTA intake ------------------------------------------------------
  // d: Uint8Array, d[0..3]='GSTA', then 261-byte serialized GameState.
  static isGSTA(d){ return d.length>=4 && d[0]===71 && d[1]===83 && d[2]===84 && d[3]===65; }

  onGSTA(d){
    const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
    const B = 4;                       // payload starts after 'GSTA'
    this.inMatch = dv.getUint8(B+0);
    for (let s=0; s<2; s++){
      const ci = B + 25 + s*38;        // char block: 25-byte global header + 38*slot
      const sl = this.slot[s];
      sl.active    = dv.getUint8(ci+0);
      sl.char_id   = dv.getUint8(ci+1);
      sl.facing    = dv.getUint8(ci+2);
      sl.screen_x  = dv.getFloat32(ci+16, true);
      sl.screen_y  = dv.getFloat32(ci+20, true);
      const sid    = dv.getUint16(ci+32, true);
      // settle tracking
      if (sid === sl.settleId) sl.settleN++;
      else { sl.settleId = sid; sl.settleN = 1; }
      sl.sprite_id = sid;
    }
  }

  // --- capture ----------------------------------------------------------
  onRendered(webgpuCanvas){
    if (!this.on || !this.inMatch) return;
    const W = webgpuCanvas.width, H = webgpuCanvas.height;
    if (!W || !H) return;
    // pull the isolated frame off the WebGPU canvas into a 2D canvas
    if (this._c2.width !== W || this._c2.height !== H){ this._c2.width=W; this._c2.height=H; }
    this._x2.clearRect(0,0,W,H);
    try { this._x2.drawImage(webgpuCanvas, 0, 0, W, H); }
    catch(e){ this._lastErr = String(e); return; }

    for (let s=0; s<2; s++){
      const sl = this.slot[s];
      if (!sl.active) continue;
      if (sl.settleN < SETTLE) continue;        // skew guard: only settled sprites
      const fx0 = (s===1) ? (1-this.box.x1) : this.box.x0;
      const fx1 = (s===1) ? (1-this.box.x0) : this.box.x1;
      let x0=(fx0*W)|0, x1=(fx1*W)|0, y0=(this.box.y0*H)|0, y1=(this.box.y1*H)|0;
      x0=Math.max(0,x0); y0=Math.max(0,y0); x1=Math.min(W,x1); y1=Math.min(H,y1);
      const cw=x1-x0, ch=y1-y0; if (cw<=0||ch<=0) continue;
      let img; try { img = this._x2.getImageData(x0,y0,cw,ch); } catch(e){ this._lastErr=String(e); continue; }
      const hash = this._fnv(img.data);
      const key = `${s}|${sl.char_id}|${sl.sprite_id}`;
      let rec = this.caps.get(key);
      if (!rec){ rec = { slot:s, char_id:sl.char_id, sprite_id:sl.sprite_id,
                         hashes:new Map(), occ:0, firstImg:img }; this.caps.set(key, rec); }
      rec.occ++;
      rec.hashes.set(hash, (rec.hashes.get(hash)||0)+1);
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
    let t = `BAKE ${this.on?'ON':'off'} (settled sprites only)\n`;
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
    const cw=recs[0].firstImg.width, ch=recs[0].firstImg.height;
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
  reset(){ this.caps.clear(); this.slot=[this._blankSlot(),this._blankSlot()]; this._lastErr=''; }
}
