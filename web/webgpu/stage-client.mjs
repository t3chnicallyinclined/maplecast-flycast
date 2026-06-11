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
// ── PROJECTION (THE GAP) ──────────────────────────────────────────────────────
// IMPORTANT FINDING: STGxxPOL geometry is NOT pre-projected screen space. It is
// full 3D NAOMI world-space (x in ±48000, real Z depth, valid 0..1 UVs, per-vertex
// colors) — the same NaomiLib model tree ModNao renders with a free perspective
// camera. MVC2's render code projects it each frame with the NAOMI perspective
// camera (camera_x/y on the wire = 0x8C1F9CD8/CDC). The exact fight-camera matrix
// (fov, eye distance, world->view) is the one remaining unknown — see "FULL VERSION"
// at the bottom of this file. Until that's calibrated, this module projects with a
// configurable perspective camera (this.cam) good enough to place the stage behind
// the characters; per-stage calibration knobs are exposed.
//
// The verts are projected to 640x480 screen space CPU-side here (PVR2Renderer
// consumes screen-space x,y pre-NDC with z = 1/w depth and applies only the NDC
// matrix from pvrSnap[0]).

const SCREEN_W = 640, SCREEN_H = 480;

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
  }

  // Drive from GSTA each frame. stageId is 0..0x10; animTimer is the u8 wire field.
  setState(stageId, animTimer = 0) {
    this.animTimer = animTimer | 0;
    if (stageId === this.wantId) return;
    this.wantId = stageId;
    this._ensureLoaded(stageId);
  }

  async _ensureLoaded(stageId) {
    if (this._loading || stageId < 0) return;
    if (stageId === this.stageId) return;
    this._loading = true;
    const sid = stageId.toString(16).toUpperCase().padStart(2, '0');
    try {
      const data = await (await fetch(`${this.base}/STG${sid}.json`)).json();
      const imgs = await Promise.all(data.textures.map(async t => {
        try {
          const blob = await (await fetch(`${this.base}/${t.file}`)).blob();
          return await createImageBitmap(blob);
        } catch { return null; }
      }));
      this._data = data;
      this._imgs = imgs;
      this.stageId = stageId;
      const cam = this._perStageCam[stageId];
      this.cam = cam ? { ...DEFAULT_CAM, ...cam } : { ...DEFAULT_CAM };
      this._parsed = this._build(data);
      this._uploadTextures();
      console.log(`[stage-client] loaded STG${sid}: ${data.meshes.length} meshes, ${data.textures.length} textures`);
    } catch (e) {
      console.warn(`[stage-client] failed to load stage ${sid}`, e);
    } finally {
      this._loading = false;
    }
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

  // Build the PVR2Renderer parsed object once per stage.
  _build(data) {
    const vp = this._viewProj();
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
          const [sx, sy, sz] = this._project(vp, v.pos[0], v.pos[1], v.pos[2]);
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
// FULL VERSION (camera/projection — the one remaining RE task):
//
// The geometry is full 3D NAOMI world-space, so pixel-exact placement needs MVC2's
// real fight camera. To CALIBRATE:
//   1. Capture a live out-of-match TA video frame for STG00 (PVR2Renderer renders
//      it perfectly today). Note the on-screen position of a few recognisable stage
//      features (e.g. the wood-plank wall texture t01).
//   2. The NAOMI projection lives in the per-frame matrix the game uploads; on the
//      wire camera_x/y = 0x8C1F9CD8/0x8C1F9CDC (maplecast_gamestate.cpp:63-64) shift
//      the eye. RE the world->view->projection in bank12 (loc_8c1216c0 world->screen)
//      to recover fov + eye distance + the world Y of the ground plane.
//   3. Replace DEFAULT_CAM here (and _perStageCam overrides) with the recovered
//      matrix; drive eye by camera_x/y each frame.
// Animation: stage_anim_timer (0x8C1F9D80) low bit drives an A/B keyframe toggle
//   (loc_8c0338ec). To animate, tools/rip_stage.py must additionally decode the
//   keyframe lists and this module swap the affected meshes' UV/verts by animTimer&1.
//   For the first version we render the static A-phase.
