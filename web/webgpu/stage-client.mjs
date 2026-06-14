// stage-client.mjs — MVC2 stage (background) renderer, driven by GSTA stage_id,
// rendered CLIENT-SIDE through PVR2Renderer as the OP (background) layer.
//
// NO VRAM sync, NO server geometry re-map. Same rip-from-state pattern as the
// characters + HUD: the disc art (STGxxPOL/TEX) is decoded OFFLINE by
// tools/rip_stage.py (a port of ModNao's NaomiLib decoder) into
// atlas/stages/STGxx.json + STGxx_tNN.png; this module loads that for the
// current stage_id and emits the PVR2Renderer parsed-object shape (§0 of
// docs/RENDER-MASTER-PLAN-V2.md):
//
//   parsed = { vertexData:Uint8Array(28B/vert), vertexCount, opaque:[PP], punchThrough:[], translucent:[PP] }
//   PP     = { first, count, isp, tsp, tcw(surrogate), pcw(paraType=4), tileclip:0 }
//
// rendered via:
//   renderFrame(parsed, texMgr, pvrSnap=640x480, null, {singlePass:true, noSort:true, transparentClear:true})
//
// ── PROJECTION (GROUNDED — NO MODEL CAMERA) ───────────────────────────────────
// STGxxPOL geometry is full 3D NAOMI world-space (x in ±48000, real Z depth, valid
// 0..1 UVs, per-vertex colors) — the same NaomiLib model tree the bodies live in.
// MVC2 projects EVERY renderable object (bodies AND the stage tree) through ONE
// frame-global transform: loc_8C122560 (bank12), which the render-replica already
// ports byte-exact as transform_object_122560 (tools/render-replica-poc/
// gen_transform_obj.c) and which the body slot-walk feeds world pos through to
// deposit screen_x/y @node+0xE0/E4. That transform is:
//     XMTRX = M1(@0x8C2D6B18) · M2(@0x8C2D6AD8)       (column-major mat·mat, loc_8c120540)
//     fv    = XMTRX · (x,y,z,1)                        (ftrv,                 loc_8c11F870)
//     inv = 1/fv[3];  sx = fv[0]*inv;  sy = fv[1]*inv;  depth = inv   (persp divide)
// Both source matrices are FRAME-GLOBAL camera state (built once/frame by the proj
// setup loc_8c1216c0) and are SHIPPED in the replica-live read-set "cam_mat" region
// (0x8C2D6AD8 + 0xC0 covers BOTH M2@+0x00 and M1@+0x40). So the client projects the
// stage with the engine's OWN live camera — it scrolls/zooms in lockstep with the
// fighters, ZERO guessed fov/eye. setCamera(M1,M2) drives this each frame; the legacy
// DEFAULT_CAM perspective fallback is kept ONLY for the no-matrix standalone preview.
//
// fv[0]/fv[1] come out already in 640x480 screen pixels (the engine's proj matrix bakes
// the viewport), matching the bodies' deposited +0xE0/E4. PVR2Renderer then applies only
// its NDC matrix from pvrSnap[0]. depth = 1/w (bigger = nearer), same convention as the
// body TA the renderer consumes.

const SCREEN_W = 640, SCREEN_H = 480;

// ── stage_id (wire, @0x8C289638) -> STGxx disc index MAP ──────────────────────
// stage_id is NOT a 0-based file index. CONFIRMED (re_kb 26, live in-match capture
// + byte-exact POL fingerprint, modelCount=83 unique): stage_id 0x11 -> STG0B.
// Other ids are resolved by the fallback (id -> STGxx directly) until each is
// captured live and its loaded POL fingerprinted. Add confirmed entries here.
const STAGE_ID_MAP = {
  0x11: 0x0B,   // CONFIRMED live (re_kb 26): id 17 -> STG0B
};
function resolveStageFile(stageId) {
  if (stageId in STAGE_ID_MAP) return STAGE_ID_MAP[stageId];
  return stageId & 0xFF;   // fallback: treat id as the file index
}

// pvrSnap that makes _ndcMat produce a 640x480 viewport (tx=19,ty=14):
//   w=(tx+1)*32=640, h=(ty+1)*32=480
export const STAGE_PVRSNAP = (() => {
  const s = new Uint32Array(16);
  s[0] = 19 | (14 << 16);
  return s;
})();

// Default fight camera (CALIBRATION KNOBS — tune per stage vs live video).
// A right-handed perspective looking down -Z. World units are large (~1000s),
// the fighters stand near world origin on the ground plane.
const DEFAULT_CAM = {
  fovDeg: 35,        // vertical field of view
  eyeX: 0,           // camera world position
  eyeY: 600,
  eyeZ: 4200,
  targetX: 0,        // look-at point
  targetY: 600,
  targetZ: -2000,
  near: 1,
  far: 200000,
  zScale: 1e-6,      // maps 1/w depth into the small positive range PVR2Renderer uses
};

export class StageClient {
  constructor(base = 'atlas/stages') {
    this.base = base;            // dir holding STGxx.json + STGxx_tNN.png
    this.stageId = -1;           // currently-loaded stage
    this.wantId = -1;            // requested stage (from GSTA)
    this._loading = false;
    this._data = null;           // decoded JSON for current stage
    this._imgs = [];             // ImageBitmap per texture index
    this._parsed = null;         // PVR2Renderer parsed object (built once per stage)
    this._tm = null;             // texMgr shim
    this._dev = null;
    this.cam = { ...DEFAULT_CAM };
    this.animTimer = 0;          // GSTA stage_anim_timer (low bit drives A/B; see anim note)
    this._perStageCam = {};      // optional { [stageId]: camOverride }
    this._M1 = null;             // engine viewport matrix @0x8C2D6B18 (16 floats, col-major)
    this._M2 = null;             // engine proj matrix     @0x8C2D6AD8 (16 floats, col-major)
    this._X  = null;             // cached XMTRX = M1·M2 (rebuilt on camera change)
  }

  // Drive from GSTA each frame. stageId is the WIRE stage_id (@0x8C289638); it is
  // mapped to a STGxx disc index via STAGE_ID_MAP (re_kb 26). animTimer is the u8 wire field.
  setState(stageId, animTimer = 0) {
    this.animTimer = animTimer | 0;
    const fileId = resolveStageFile(stageId | 0);
    if (fileId === this.wantId) return;
    this.wantId = fileId;
    this._ensureLoaded(fileId);
  }

  // ── ENGINE CAMERA: feed the live frame-global matrices (the GROUNDED path) ──
  // M1 = viewport matrix (guest 0x8C2D6B18), M2 = projection matrix (guest 0x8C2D6AD8),
  // each 16 floats in column-major memory order (exactly as load_mat reads them in
  // gen_transform_obj.c). Recomputes XMTRX = M1·M2 (loc_8c120540 semantics) and, if the
  // matrices actually changed, re-projects the loaded stage so it tracks the live camera.
  // Returns true if a re-projection happened. A no-op when called with the same matrices.
  setCamera(M1, M2) {
    if (!M1 || !M2 || M1.length < 16 || M2.length < 16) return false;
    if (this._M1 && this._sameMat(this._M1, M1) && this._sameMat(this._M2, M2)) return false;
    this._M1 = Float32Array.from(M1.subarray ? M1.subarray(0, 16) : M1.slice(0, 16));
    this._M2 = Float32Array.from(M2.subarray ? M2.subarray(0, 16) : M2.slice(0, 16));
    this._X = this._matmulColMaj(this._M1, this._M2);   // XMTRX = M1·M2 (loc_8c120540)
    // GROUNDED ENGINE-TA mode now carries per-vertex WORLD coords (un-projected from the
    // captured screen verts via (M1·M2)^-1 in bake_stage_from_ta.py, re_kb 26 prop-matrix
    // capture). So BOTH paths re-project through the live camera: the full assembled scene
    // (deck + all props, recovered in world space) tracks the live camera in lockstep with
    // the fighters. A TA bake WITHOUT world coords (legacy) stays camera-static (no re-proj).
    if (this._isTA && !(this._data && this._data.hasWorld)) return false;
    if (this._data) {
      this._parsed = this._isTA ? this._buildFromTA(this._data) : this._build(this._data);
      return true;
    }
    return false;
  }

  _sameMat(a, b) { for (let i = 0; i < 16; i++) if (a[i] !== b[i]) return false; return true; }

  // column-major mat·mat: out = X·Mnew where each column of Mnew is ftrv'd through X
  // (loc_8c120540 / matmul_colmaj in gen_transform_obj.c).
  _matmulColMaj(X, Mnew) {
    const out = new Float32Array(16);
    for (let col = 0; col < 4; col++)
      for (let i = 0; i < 4; i++)
        out[col * 4 + i] =
          X[i] * Mnew[col * 4 + 0] + X[i + 4] * Mnew[col * 4 + 1] +
          X[i + 8] * Mnew[col * 4 + 2] + X[i + 12] * Mnew[col * 4 + 3];
    return out;
  }

  async _ensureLoaded(stageId) {
    if (this._loading || stageId < 0) return;
    if (stageId === this.stageId) return;
    this._loading = true;
    const sid = stageId.toString(16).toUpperCase().padStart(2, '0');
    try {
      // GROUNDED PATH (re_kb 26 closed): prefer STGxx_ta.json — a bake of the engine's
      // OWN stage TA (tools/bake_stage_from_ta.py from _stage_gt/engine_ta.bin). It carries
      // the REAL PVR control words (pcw/isp/tsp/tcw) and VRAM-decoded textures + the fully-
      // assembled screen geometry (all 83 models incl the props the POL rip cannot place).
      // Falls back to the POL-rip STGxx.json (world-space deck only, synth control words)
      // when no TA bake exists for this stage.
      let data = null, isTA = false;
      try {
        const r = await fetch(`${this.base}/STG${sid}_ta.json`);
        if (r.ok) { data = await r.json(); isTA = (data.mode === 'engine_ta'); }
      } catch { /* no TA bake */ }
      if (!data) data = await (await fetch(`${this.base}/STG${sid}.json`)).json();
      const imgs = await Promise.all(data.textures.map(async t => {
        try {
          const blob = await (await fetch(`${this.base}/${t.file}`)).blob();
          return await createImageBitmap(blob);
        } catch { return null; }
      }));
      this._data = data;
      this._isTA = isTA;
      this._imgs = imgs;
      this.stageId = stageId;
      const cam = this._perStageCam[stageId];
      this.cam = cam ? { ...DEFAULT_CAM, ...cam } : { ...DEFAULT_CAM };
      this._parsed = isTA ? this._buildFromTA(data) : this._build(data);
      this._uploadTextures();
      console.log(`[stage-client] loaded STG${sid} (${isTA ? 'engine-TA grounded' : 'POL-rip'}): `
        + `${data.meshes.length} meshes, ${data.textures.length} textures`);
    } catch (e) {
      console.warn(`[stage-client] failed to load stage ${sid}`, e);
    } finally {
      this._loading = false;
    }
  }

  // ── GROUNDED build: engine-TA bake -> PVR2Renderer parsed object ──────────────
  // Control words come STRAIGHT from the engine (data.meshes[].pcw/isp/tsp), NOT
  // synthesized. Each vertex carries the engine's captured-frame SCREEN-space `pos`
  // (sx,sy,1/w) AND its un-projected `world` coords (data.hasWorld). When the live
  // camera is available (this._X set via setCamera) and the bake has world coords, we
  // RE-PROJECT world through the live XMTRX (_projectEngine) so the whole assembled
  // scene — deck + every prop, all recovered in world space — tracks the live camera.
  // Otherwise we use the baked screen coords verbatim (camera-static fallback).
  // tcw carries the texture surrogate so getTexture binds the VRAM-decoded texture.
  // intensity (vertex .i, Col_Type=2) is folded into the per-vertex base colour so the
  // shader's modulate (ShadInstr=1) reproduces the engine's shading.
  _buildFromTA(data) {
    const useLive = !!(this._X && data.hasWorld);
    let triTotal = 0;
    for (const m of data.meshes) triTotal += m.tris.length;
    const vcount = triTotal * 3;
    const vbuf = new ArrayBuffer(vcount * 28);
    const vf = new Float32Array(vbuf), vb = new Uint8Array(vbuf);
    const opaque = [], translucent = [];
    let vi = 0;
    this._surrToTex = {};            // surrogate == texIndex here (1:1)
    for (const t of data.textures) this._surrToTex[t.surr] = t.surr - 1;

    const writeVtx = (n, x, y, z, c, u, v) => {
      const fo = n * 7, bo = n * 28;
      vf[fo] = x; vf[fo + 1] = y; vf[fo + 2] = z;
      vb[bo + 12] = c; vb[bo + 13] = c; vb[bo + 14] = c; vb[bo + 15] = 255;
      vf[fo + 5] = u; vf[fo + 6] = v;
    };

    for (const m of data.meshes) {
      if (!m.tris.length) continue;
      const pcw = m.pcw >>> 0, isp = m.isp >>> 0, tsp = m.tsp >>> 0;
      // texture surrogate: map this mesh's tcw addr -> the texture surr we assigned.
      // bake stores per-mesh tcw; surrogate is the texture's surr field. We re-find it
      // by matching addr; simplest: meshes already aligned to the surr list by addr.
      let surr = 0;
      if (m.textured) surr = this._surrForMeshTA(data, m);
      // ── PER-MESH RE-PROJECTION GATE (the never-black fix; re_kb 26 prop-matrix defer) ──
      // The deck/floor/backdrop are authored in true WORLD space (large extent, real
      // negative depth) and un-project + re-project byte-exact through the live camera —
      // so they SCROLL/ZOOM with the fighters. The small props (cannons/chains) carry only
      // LOCAL-space coords in `world` (the runtime NaomiLib matrix that PLACED them on
      // screen was never captured — re_kb 26 deferred item 1). Re-projecting their local
      // coords flings them to ±millions off-screen, which is exactly why the stage went
      // BLACK (only a few faint deck triangles survived). For those props we render the
      // engine's OWN baked SCREEN `pos` verbatim (camera-static but correctly placed).
      // Discriminator (grounded in STG0B world geom): a mesh is world-authored iff its
      // world depth reaches the deck range (minZ < -500) OR its X extent exceeds 1000u.
      const worldMesh = this._meshIsWorldAuthored(m);
      const reproject = useLive && worldMesh;
      // INTENSITY FLOOR (deck-visibility fix). The engine-TA bake decodes a Col_Type=1
      // (packed-color) intensity for each vertex. For the large world-authored structural
      // meshes (deck/floor/backdrop) that intensity came out ~0.01 — modulating the bright
      // deck texture (ShadInstr=1) by ~0 made the deck render BLACK even though it draws on
      // hardware. Those textured meshes ARE the stage art and the engine shows them at full
      // intensity, so floor the base colour to 1.0 for textured world meshes (decal-like
      // pass-through of the deck texture). Small props keep their real per-vertex shading.
      const intensityFloor = (worldMesh && m.textured) ? 1.0 : 0.0;
      // OP list (engine ListType 0). Engine isp DepthMode/cull are honoured by the renderer.
      const first = vi;
      for (const tri of m.tris) {
        for (const v of tri) {
          const iv = Math.max(intensityFloor, (v.i ?? 1));
          const c = Math.max(0, Math.min(255, Math.round(iv * 255)));
          let px = v.pos[0], py = v.pos[1], pz = v.pos[2];
          if (reproject) {
            const [sx, sy, sz] = this._projectEngine(v.world[0], v.world[1], v.world[2]);
            px = sx; py = sy; pz = sz;
          }
          writeVtx(vi++, px, py, pz, c, v.uv[0], v.uv[1]);
        }
      }
      for (let t = first; t + 3 <= vi; t += 3)
        opaque.push({ first: t, count: 3, isp, tsp, tcw: surr, pcw, tileclip: 0 });
    }
    return { vertexData: vb.subarray(0, vi * 28), vertexCount: vi,
             opaque, punchThrough: [], translucent };
  }

  // map a TA mesh to its texture surrogate by TexAddr (the bake assigned surr per
  // distinct (addr,size,fmt) key; meshes share that addr in tcw).
  _surrForMeshTA(data, m) {
    if (!this._taAddrToSurr) {
      this._taAddrToSurr = {};
      for (const t of data.textures) this._taAddrToSurr[t.addr] = t.surr;
    }
    return this._taAddrToSurr[(m.tcw >>> 0) & 0x1FFFFF] || 0;
  }

  // World-authored vs local-prop classifier (the re-projection gate, re_kb 26).
  // A mesh authored in TRUE world space spans the deck's depth (a far-negative minZ) or
  // a large lateral extent; its world coords round-trip through the live camera. A small
  // local-space prop (cannon/chain) carries only ±tens-of-units local coords whose runtime
  // placement matrix was never captured — re-projecting it explodes off-screen, so it must
  // fall back to the engine's baked screen pos. Cached per-mesh (computed once per bake).
  _meshIsWorldAuthored(m) {
    if (m._wa !== undefined) return m._wa;
    let minZ = Infinity, xext = 0;
    for (const tri of m.tris) for (const v of tri) {
      const w = v.world; if (!w) { m._wa = false; return false; }
      if (w[2] < minZ) minZ = w[2];
      const ax = Math.abs(w[0]); if (ax > xext) xext = ax;
    }
    m._wa = (minZ < -500) || (xext > 1000);
    return m._wa;
  }

  // ── camera: world(x,y,z) -> screen(x,y in 640x480, depth=1/w) ──
  _viewProj() {
    const c = this.cam;
    // view matrix (look-at), column ops done inline in _project
    const fx = c.targetX - c.eyeX, fy = c.targetY - c.eyeY, fz = c.targetZ - c.eyeZ;
    let fl = Math.hypot(fx, fy, fz) || 1;
    const f = [fx / fl, fy / fl, fz / fl];                 // forward
    // right = normalize(forward × up) with up = world +Y = (0,1,0):
    //   f × up = (fz, 0, -fx)
    let sx = f[2], sy = 0, sz = -f[0];
    let sl = Math.hypot(sx, sy, sz) || 1; sx /= sl; sy /= sl; sz /= sl;  // right
    // u = s × f
    const ux = sy * f[2] - sz * f[1];
    const uy = sz * f[0] - sx * f[2];
    const uz = sx * f[1] - sy * f[0];
    const tanHalf = Math.tan((c.fovDeg * Math.PI / 180) / 2) || 1;
    return { right: [sx, sy, sz], up: [ux, uy, uz], fwd: f, eye: [c.eyeX, c.eyeY, c.eyeZ], tanHalf, aspect: SCREEN_W / SCREEN_H, zScale: c.zScale };
  }

  _project(vp, x, y, z) {
    // to view space
    const dx = x - vp.eye[0], dy = y - vp.eye[1], dz = z - vp.eye[2];
    const vx = dx * vp.right[0] + dy * vp.right[1] + dz * vp.right[2];
    const vy = dx * vp.up[0] + dy * vp.up[1] + dz * vp.up[2];
    const vzf = dx * vp.fwd[0] + dy * vp.fwd[1] + dz * vp.fwd[2]; // distance along forward (+ in front)
    const w = Math.max(vzf, 1e-3);
    // perspective divide -> NDC (-1..1)
    const ndcX = (vx / (w * vp.tanHalf * vp.aspect));
    const ndcY = (vy / (w * vp.tanHalf));
    // NDC -> 640x480 screen (PVR2Renderer applies its own NDC matrix; emit screen px)
    const sx = (ndcX * 0.5 + 0.5) * SCREEN_W;
    const sy = (1 - (ndcY * 0.5 + 0.5)) * SCREEN_H;
    // depth: PVR2Renderer treats z as 1/w (bigger = nearer). Scale into small positive.
    const sz = (1 / w) / vp.zScale * 1e-6;
    return [sx, sy, Math.max(sz, 1e-9)];
  }

  // ENGINE projection (the grounded path): fv = XMTRX·(x,y,z,1); persp divide.
  // Mirrors transform_object_122560 EXACTLY (ftrv_colmaj + 1/fv[3]). Returns
  // [screenX_px, screenY_px, depth=1/w]. depth clamped > 0 for the renderer's z buffer.
  _projectEngine(x, y, z) {
    const X = this._X;
    const fx = X[0]*x + X[4]*y + X[8]*z  + X[12];
    const fy = X[1]*x + X[5]*y + X[9]*z  + X[13];
    // fv[2] (z) unused for screen xy; fv[3] = w
    const fw = X[3]*x + X[7]*y + X[11]*z + X[15];
    const inv = 1.0 / (fw || 1e-6);
    return [fx * inv, fy * inv, Math.max(inv, 1e-9)];
  }

  // Build the PVR2Renderer parsed object once per stage.
  _build(data) {
    const useEngine = !!this._X;
    const vp = useEngine ? null : this._viewProj();
    // worst-case verts = tris*3
    let triTotal = 0;
    for (const m of data.meshes) triTotal += m.tris.length;
    const vcount = triTotal * 3;
    const vbuf = new ArrayBuffer(vcount * 28);
    const vf = new Float32Array(vbuf);
    const vb = new Uint8Array(vbuf);
    const opaque = [], translucent = [];
    let vi = 0;

    const surrByTex = new Map();   // texIndex -> surrogate int key (1-based)
    this._surrToTex = {};          // surrogate -> texIndex (for texMgr)

    const writeVtx = (n, x, y, z, col, u, v) => {
      const fo = n * 7, bo = n * 28;
      vf[fo] = x; vf[fo + 1] = y; vf[fo + 2] = z;
      // col: [r,g,b,a] bytes in R,G,B,A order
      vb[bo + 12] = col[0]; vb[bo + 13] = col[1]; vb[bo + 14] = col[2]; vb[bo + 15] = col[3];
      vb[bo + 16] = 0; vb[bo + 17] = 0; vb[bo + 18] = 0; vb[bo + 19] = 0; // spc
      vf[fo + 5] = u; vf[fo + 6] = v;
    };

    for (const m of data.meshes) {
      if (!m.tris.length) continue;
      // WORLD-SPACE ASSEMBLY (re_kb 26): rip_stage.py tags each model placed/unplaced.
      // Placed models carry WORLD-space verts (model authored in world space = identity,
      // or pre-multiplied by a captured runtime world matrix). Unplaced = a LOCAL-space
      // prop with no captured matrix; rendering it would collapse it to one screen dot
      // (the old "green blob"). SKIP it until its matrix is captured. (placed is absent
      // in legacy JSON -> default true, unchanged behavior for already-correct stages.)
      if (m.placed === false) continue;
      const textured = (m.texIndex < data.textures.length) ? 1 : 0;
      let surr = 0;
      if (textured) {
        surr = surrByTex.get(m.texIndex);
        if (surr === undefined) { surr = surrByTex.size + 1; surrByTex.set(m.texIndex, surr); this._surrToTex[surr] = m.texIndex; }
      }
      const alpha = (m.alpha === undefined) ? 1 : m.alpha;
      const isTrans = (!m.isOpaque) || alpha < 0.999;
      const first = vi;
      // emit each triangle as its own 3-vert "strip" (count=3); PVR2Renderer
      // _buildIndexBuffer turns count-2 tris from a strip — for a single tri that's 1.
      for (const tri of m.tris) {
        for (const v of tri) {
          const [sx, sy, sz] = useEngine
            ? this._projectEngine(v.pos[0], v.pos[1], v.pos[2])
            : this._project(vp, v.pos[0], v.pos[1], v.pos[2]);
          let col = v.col;
          if (alpha < 0.999) col = [col[0], col[1], col[2], Math.round(col[3] * alpha)];
          writeVtx(vi++, sx, sy, sz, col, v.uv[0], v.uv[1]);
        }
      }
      const count = vi - first;
      // Build PolyParams: one per triangle so each strip is exactly a triangle
      // (avoids degenerate links between unrelated tris). first/count index verts.
      // We push ONE PP spanning all tris of the mesh would degenerate-link; instead
      // push a PP per 3 verts.
      const list = isTrans ? translucent : opaque;
      // Synthesize PVR control words:
      //   pcw: paraType=4, textured bit3, gouraud bit1 (per-vertex colors)
      const pcw = (4 << 29) | (textured << 3) | (1 << 1);
      //   tsp: blend. opaque => ONE/ZERO (PVR2Renderer forces this for opaque anyway);
      //        trans => src-alpha / one-minus-src-alpha; useAlpha bit20; ShadInstr=1 (modulate) bit6
      const tsp = isTrans
        ? ((4 << 29) | (5 << 26) | (1 << 20) | (1 << 6))
        : ((1 << 29) | (0 << 26) | (1 << 6));
      //   isp: DepthMode greater-equal(6) for trans / less-equal for op; CullMode none(0); ZWrite on for op
      const isp = isTrans
        ? ((6 << 29) | (0 << 27) | (1 << 26))   // dm=6, cull=0, zwrite-dis=1
        : ((6 << 29) | (0 << 27) | (0 << 26));   // dm=6 (always-ish), cull none, zwrite on
      for (let t = first; t + 3 <= vi; t += 3) {
        list.push({ first: t, count: 3, isp, tsp, tcw: surr, pcw, tileclip: 0 });
      }
    }

    return {
      vertexData: vb.subarray(0, vi * 28),
      vertexCount: vi,
      opaque, punchThrough: [], translucent,
    };
  }

  // Re-project (e.g. after camera tweak) without re-fetching.
  rebuild() { if (this._data) { this._parsed = this._build(this._data); } }

  // ── texMgr shim (PVR2Renderer protocol): surrogate tcw -> GPUTexture ──
  attachDevice(dev) { this._dev = dev; if (this._data) this._uploadTextures(); }

  _uploadTextures() {
    const dev = this._dev;
    if (!dev || !this._data) return;
    const texByIdx = new Map();   // texIndex -> {texture, sampler, w, h}
    for (let i = 0; i < this._imgs.length; i++) {
      const img = this._imgs[i];
      if (!img) continue;
      const w = img.width, h = img.height;
      const texture = dev.createTexture({ size: [w, h], format: 'rgba8unorm',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT });
      dev.queue.copyExternalImageToTexture({ source: img }, { texture }, [w, h]);
      const sampler = dev.createSampler({ minFilter: 'linear', magFilter: 'linear',
        addressModeU: 'repeat', addressModeV: 'repeat' });
      texByIdx.set(i, { texture, sampler, w, h });
    }
    let fb = null;
    const surrToTex = this._surrToTex || {};
    this._tm = {
      getFallbackTexture() {
        if (fb) return fb;
        const t = dev.createTexture({ size: [1, 1], format: 'rgba8unorm',
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
        dev.queue.writeTexture({ texture: t }, new Uint8Array([255, 255, 255, 255]), { bytesPerRow: 4 }, [1, 1]);
        fb = { texture: t, sampler: dev.createSampler({ minFilter: 'nearest', magFilter: 'nearest' }) };
        return fb;
      },
      getTexture(tsp, surr) {
        if (!surr) return null;
        const ti = surrToTex[surr];
        if (ti === undefined) return null;
        return texByIdx.get(ti) || null;
      },
    };
  }

  // ── render: draw the stage OP layer first (behind chars/effects) ──
  // renderer = a PVR2Renderer bound (initShared) to the background/overlay canvas.
  render(renderer) {
    if (!this._parsed || !this._tm || !this._parsed.vertexCount) return false;
    renderer.renderFrame(this._parsed, this._tm, STAGE_PVRSNAP, null,
      { singlePass: true, noSort: true, transparentClear: true,
        drawOpaque: true, drawPunch: false, drawTrans: true });
    return true;
  }

  get texMgr() { return this._tm; }
  get parsed() { return this._parsed; }
}

// ──────────────────────────────────────────────────────────────────────────────
// STATUS (re_kb 26, resolved 2026-06-13):
//   PROJECTION — DONE. XMTRX = M1(@0x8C2D6B18)·M2(@0x8C2D6AD8), the engine's own
//   frame-global camera, fed live via setCamera(). PROVEN byte-exact: STG0B's placed
//   geometry projects to the engine's actual stage TA (_stage_gt/engine_ta.bin) at
//   0.000px residual (median/mean/p90/max). The DEFAULT_CAM perspective path is the
//   legacy standalone-preview fallback ONLY; the live client always uses setCamera.
//   WORLD ASSEMBLY — DONE for world-authored models. rip_stage.py marks each model
//   placed/unplaced: a model with large vertex extent is authored in world space
//   (identity) and renders correct; small LOCAL-space props need a runtime world
//   matrix and are skipped (placed=false, handled in _build) until captured.
//
// REMAINING (deferred follow-up, cited re_kb 26):
//   (1) PROP MATRICES. The per-node world matrices for small props (e.g. STG0B's 66
//       cannons/chains, ±18u local) are RUNTIME state pushed on the NaomiLib matrix
//       stack @0x8C2D6900 during the tree-walk (bank12 loc_8c122fd0/loc_8c122d00) —
//       NOT in the POL (the +0x0C/+0x10/+0x14 header floats are a bounding-sphere,
//       confirmed). To capture: MAPLECAST_ORACLE_PROBE a per-model PC dumping the
//       stack-top 4x3 (mem window over 0x8C2D6900); remove the camera offline
//       (node_local = (M1·M2)^-1 · captured) -> STGxx_matrices.json which rip_stage.py
//       pre-multiplies into world space. Most stages need NO props (STG00/04/07/09/0D
//       = 0 local models); STG0B is the worst case (only the deck is world-space).
//   (2) STAGE_ID MAP. STAGE_ID_MAP has the confirmed live entry (0x11->STG0B); other
//       ids fall back to id->file. Capture each stage live + fingerprint its loaded
//       POL (modelCount + POL byte-hash, all unique except the 01/0A shared-geometry
//       pair) to complete the map.
//   (3) ANIMATION. stage_anim_timer (0x8C1F9D80) low bit drives an A/B keyframe toggle
//       (loc_8c0338ec); static A-phase for now.
