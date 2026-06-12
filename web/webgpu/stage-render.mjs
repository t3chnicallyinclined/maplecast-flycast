// stage-render.mjs — STAGE RENDER ADAPTER
// ════════════════════════════════════════════════════════════════════════════
// Wires the completed offline stage decoder (tools/decode_stage_pol.py →
// <out>/STGxx.json + tex_NN.png) into the EXISTING WebGPU rasterizer
// (web/webgpu/pvr2-renderer.mjs) WHOLESALE. This file writes NO new rasterizer;
// it is a thin converter that produces the `parsed` object
// renderFrame(parsed, texMgr, pvrSnap, vram, dbg, renderTarget) expects, plus a
// shim texMgr that serves the decoded PNGs, and a camera that projects the
// stage's full-3D NAOMI world geometry to 640×480 screen space.
//
// ── WHY A CPU-SIDE CAMERA (important) ────────────────────────────────────────
// The decoded STGxx geometry is NOT pre-projected screen space — it is full 3D
// NAOMI world space (STG00 X/Y ∈ ±48000, Z ∈ [-100435, +9099], valid 0..1 UVs,
// per-vertex lit colors). PVR2Renderer's _ndcMat(pvrSnap) is ONLY an orthographic
// screen→clip scale (m[0]=2/W, m[5]=-2/H); it does NOT do perspective. So, exactly
// like the character/HUD quads (which arrive ALREADY in screen space), we must
// hand PVR2Renderer screen-space (x,y) here. This adapter does the perspective
// projection CPU-side, emits screen px + a 1/w depth in vertex.z (the shader does
// uv*z then divides — perspective-correct), and uses STAGE_PVRSNAP so _ndcMat maps
// 640×480 → NDC. The fight camera (fov / eye / look-at) is the ONE calibration
// unknown; DEFAULT_CAM is a sane starting guess, per-stage overridable, and the
// exact matrix is recoverable from bank12 world→screen (loc_8c1216c0) driven by
// camera_x/y (0x8C1F9CD8/CDC on the GSTA wire). See bottom of file.
//
// ── CONTROL-WORD SYNTHESIS (the one genuinely new RE piece) ───────────────────
// PVR2Renderer reads per-poly isp/tsp/tcw/pcw bitfields (pvr2-renderer.mjs:237-254
// stage(), :290-348 drawSlice()). The decoder gives us Ninja mesh fields instead.
// synthControlWords() below maps Ninja→PVR bit-for-bit; each mapping is annotated
// CONFIRMED (read directly out of pvr2-renderer/shaders/texture-manager) or
// INFERRED (sensible Ninja-semantics default, to be tuned against the differ).

const SCREEN_W = 640, SCREEN_H = 480;

// pvrSnap whose _ndcMat yields a 640×480 viewport: g&0x3F=tx, (g>>16)&0x3F=ty,
// w=(tx+1)*32=640 → tx=19 ; h=(ty+1)*32=480 → ty=14. (pvr2-renderer.mjs:464-468)
export const STAGE_PVRSNAP = (() => {
  const s = new Uint32Array(16);
  s[0] = 19 | (14 << 16);
  return s;
})();

// Default fight camera (CALIBRATION KNOBS — tune per stage vs a live STG frame).
// Right-handed perspective looking roughly down -Z; world up = +Y. The fighters
// stand near the world origin on the ground plane.
const DEFAULT_CAM = {
  fovDeg: 30,        // vertical field of view
  eyeX: 0, eyeY: 0, eyeZ: 9000,           // camera world position
  targetX: 0, targetY: 0, targetZ: -2000, // look-at point
  // depth packing: the shader's frag depth = log2(1+100000*z)/34, bigger z = nearer.
  // We emit z = 1/viewDist scaled so the whole stage lands in a small positive band.
  depthScale: 1.0,
};

export class StageRenderAdapter {
  // device: the GPUDevice shared from the main PVR2Renderer (R.dev).
  constructor(device) {
    this.dev = device;
    this.cam = { ...DEFAULT_CAM };
    this._data = null;       // decoded STGxx.json
    this._parsed = null;     // PVR2Renderer parsed object
    this._tm = null;         // shim texMgr
    this._texByIdx = new Map();   // textureIndex -> {texture,sampler,w,h}
    this._surrToIdx = new Map();  // surrogate tcw -> textureIndex
    this._fallback = null;
  }

  // ── a) loadStage: decoded JSON + per-index ImageBitmaps → parsed + textures ──
  // texImgs: ImageBitmap[] aligned to textureList index (textureIndex selects).
  async loadStage(stageJson, texImgs) {
    this._data = stageJson;
    this._uploadTextures(texImgs);
    this._parsed = this._build(stageJson);
    return this._parsed;
  }

  setCamera(partial) { this.cam = { ...this.cam, ...partial }; if (this._data) this._parsed = this._build(this._data); }

  // ── camera: world → screen(640×480) + 1/w depth ──
  _viewBasis() {
    const c = this.cam;
    let fx = c.targetX - c.eyeX, fy = c.targetY - c.eyeY, fz = c.targetZ - c.eyeZ;
    const fl = Math.hypot(fx, fy, fz) || 1; fx /= fl; fy /= fl; fz /= fl; // forward
    // Right-handed look-at with world up = +Y (the stage's vertical axis: sky up,
    // water down). right = normalize(worldUp × forward) so +screenX = +worldX and
    // +screenY = +worldY (upright, not mirrored). worldUp×forward = (1*fz-0*fy, 0*fx-0*fz, 0*fy-1*fx) = (fz,0,-fx)... use the proper cross:
    //   up=(0,1,0); right = up × forward = (1·fz − 0·fy, 0·fx − 0·fz, 0·fy − 1·fx) = (fz, 0, −fx)
    let rx = fz, ry = 0, rz = -fx;
    const rl = Math.hypot(rx, ry, rz) || 1; rx /= rl; ry /= rl; rz /= rl;
    // true up = forward × right (re-orthogonalize; +Y-ish, points screen-up)
    const ux = fy * rz - fz * ry, uy = fz * rx - fx * rz, uz = fx * ry - fy * rx;
    const tanHalf = Math.tan((c.fovDeg * Math.PI / 180) / 2) || 1;
    return { rx, ry, rz, ux, uy, uz, fx, fy, fz, ex: c.eyeX, ey: c.eyeY, ez: c.eyeZ,
             tanHalf, aspect: SCREEN_W / SCREEN_H, depthScale: c.depthScale };
  }

  _project(b, x, y, z) {
    const dx = x - b.ex, dy = y - b.ey, dz = z - b.ez;
    const vx = dx * b.rx + dy * b.ry + dz * b.rz;
    const vy = dx * b.ux + dy * b.uy + dz * b.uz;
    const vw = Math.max(dx * b.fx + dy * b.fy + dz * b.fz, 1e-3); // dist along forward
    const ndcX = vx / (vw * b.tanHalf * b.aspect);
    const ndcY = vy / (vw * b.tanHalf);
    const sx = (ndcX * 0.5 + 0.5) * SCREEN_W;
    const sy = (1 - (ndcY * 0.5 + 0.5)) * SCREEN_H;
    // z = 1/w (perspective-correct depth the shader expects; bigger = nearer)
    const sz = Math.max((1 / vw) * b.depthScale, 1e-9);
    return [sx, sy, sz];
  }

  // ── b) build parsed: flat 28B/vert + opaque[]/translucent[] PP lists ──
  _build(data) {
    const b = this._viewBasis();
    // worst case: every polygon expanded to a triangle list = sum(triangles)*3
    let triTotal = 0;
    for (const model of data.models) for (const mesh of model.meshes)
      for (const p of mesh.polygons) triTotal += Math.max(0, (p.indices ? p.indices.length / 3 : 0));
    const vcount = triTotal * 3;
    const vbuf = new ArrayBuffer(vcount * 28);
    const vf = new Float32Array(vbuf);
    const vb = new Uint8Array(vbuf);
    const opaque = [], translucent = [];
    let vi = 0;

    this._surrToIdx.clear();
    const surrByIdx = new Map();

    const writeVtx = (n, x, y, z, col, u, v) => {
      const fo = n * 7, bo = n * 28;
      vf[fo] = x; vf[fo + 1] = y; vf[fo + 2] = z;
      // baseColor unorm8x4 @+12 in R,G,B,A byte order. Decoder vertex.colors = [r,g,b,a] floats 0..1.
      vb[bo + 12] = (col[0] * 255) & 255; vb[bo + 13] = (col[1] * 255) & 255;
      vb[bo + 14] = (col[2] * 255) & 255; vb[bo + 15] = (col[3] * 255) & 255;
      // offsetColor (specular) — zero; stage meshes carry no per-vertex offset.
      vb[bo + 16] = 0; vb[bo + 17] = 0; vb[bo + 18] = 0; vb[bo + 19] = 0;
      vf[fo + 5] = u; vf[fo + 6] = v;
    };

    for (const model of data.models) for (const mesh of model.meshes) {
      const textured = (mesh.textureIndex >= 0 && mesh.textureIndex < data.textureList.length) ? 1 : 0;
      let surr = 0;
      if (textured) {
        surr = surrByIdx.get(mesh.textureIndex);
        if (surr === undefined) { surr = surrByIdx.size + 1; surrByIdx.set(mesh.textureIndex, surr); this._surrToIdx.set(surr, mesh.textureIndex); }
      }
      const alpha = (mesh.alpha === undefined) ? 1 : mesh.alpha;
      const isTrans = (!mesh.isOpaque) || alpha < 0.999;
      const { isp, tsp, tcw, pcw } = this._synthControlWords(mesh, textured, isTrans, surr);

      for (const p of mesh.polygons) {
        const idx = p.indices;
        if (!idx || idx.length < 3) continue;
        const verts = p.vertices;
        // Expand the decoder's flattened, correctly-wound triangle list into
        // independent 3-vert "strips" (count=3) so PVR2Renderer's strip-fanner
        // emits exactly these triangles regardless of regular/triple group mode.
        for (let t = 0; t + 2 < idx.length; t += 3) {
          const first = vi;
          for (let k = 0; k < 3; k++) {
            const vtx = verts[idx[t + k]];
            const pos = vtx.position;
            const [sx, sy, sz] = this._project(b, pos[0], pos[1], pos[2]);
            let col = vtx.colors || [1, 1, 1, 1];
            if (alpha < 0.999) col = [col[0], col[1], col[2], col[3] * alpha];
            const uv = vtx.uv || [0, 0];
            writeVtx(vi++, sx, sy, sz, col, uv[0], uv[1]);
          }
          (isTrans ? translucent : opaque).push({ first, count: 3, isp, tsp, tcw, pcw, tileclip: 0 });
        }
      }
    }

    return {
      vertexData: new Uint8Array(vbuf, 0, vi * 28),
      vertexCount: vi,
      opaque, punchThrough: [], translucent,
    };
  }

  // ── synthControlWords: Ninja mesh fields → PVR isp/tsp/tcw/pcw bitfields ──
  // Each bit is annotated [CONFIRMED]=read straight from the consumer code, or
  // [INFERRED]=Ninja-semantics default to validate against the differ.
  _synthControlWords(mesh, textured, isTrans, surr) {
    // ─ pcw (Parameter Control Word) ─ consumed: pvr2-renderer.mjs
    //   bit3 textured  (:344 `(pcw>>3)&1` selects texMgr.getTexture vs fallback) [CONFIRMED]
    //   bit1 gouraud   (:249 `(pcw>>1)&1` → shader gouraud, currently unused in frag) [CONFIRMED]
    //   bit2 offset    (:245 `(pcw>>2)&1` → fu.ho → c.rgb+offset; we have no offset → 0) [CONFIRMED]
    //   bits29-31 paraType: stage() ignores it; we set 4 (polygon) for self-doc. [INFERRED]
    const gouraud = 1;                 // stage verts are gouraud-lit (per-vertex colors)
    const offset = 0;                  // no per-vertex specular/offset on stage meshes
    const pcw = (4 << 29) | (textured << 3) | (gouraud << 1) | (offset << 2);

    // ─ tsp (Texture/Shading Params) ─ consumed: pvr2-renderer.mjs + shaders.mjs
    //   bits29-31 SrcBlend, bits26-28 DstBlend (:331 sb/db → pipeline blend) [CONFIRMED]
    //   bit20 useAlpha (:238 `(tsp>>20)&1` → fu.ua; 0 forces c.a=1) [CONFIRMED]
    //   bits6-7  ShadInstr (:237 `(tsp>>6)&3` → fu.si; 1=modulate tex*col) [CONFIRMED]
    //   bit19 ignoreTexAlpha (:244 `(tsp>>19)&1` → fu.ita; force tex.a=1) [CONFIRMED]
    //   texU=(tsp>>3)&7, texV=tsp&7 used by REAL texMgr only; OUR shim ignores them,
    //     so they can stay 0 (shim keys off tcw surrogate). [CONFIRMED — shim path]
    //   Blend: opaque is forced ONE/ZERO by the renderer (:264) regardless, but we
    //   also set it so the staged useAlpha/blend read is self-consistent.
    //   ShadInstr=1 (modulate) so textured stage meshes multiply texel×vertex-light;
    //   untextured meshes show pure vertex color (frag skips the tex branch when ht=0).
    const SB_SRCALPHA = 4, DB_INVSRCALPHA = 5, SB_ONE = 1, DB_ZERO = 0;
    const shadInstr = 1;               // modulate (texel * vertex color) [INFERRED — standard MVC2 stage shading]
    const useAlpha = isTrans ? 1 : 0;  // [INFERRED] trans meshes honor per-vertex/tex alpha; opaque ignore
    const ignoreTexAlpha = 0;          // [INFERRED] let ARGB textures punch their own holes
    const tsp = isTrans
      ? ((SB_SRCALPHA << 29) | (DB_INVSRCALPHA << 26) | (useAlpha << 20) | (ignoreTexAlpha << 19) | (shadInstr << 6))
      : ((SB_ONE << 29) | (DB_ZERO << 26) | (shadInstr << 6));

    // ─ isp (ISP/TSP Instruction) ─ consumed: pvr2-renderer.mjs drawSlice()
    //   bits29-31 DepthMode (:295 `(isp>>29)&7` → DCM[]). For opaque the renderer
    //     SKIPS dm===0 (:234,:297) — must be non-zero. Translucent forces dm=6 (:327).
    //     Use 6=greater-equal for both (matches the renderer's translucent default &
    //     the GOLD opaque cape-fix; pvr2-renderer header). [CONFIRMED — renderer skips dm=0]
    //   bits27-28 CullMode (:295 `(isp>>27)&3`, applied as cm^1 → CM[]). Decoder gives
    //     mesh polys flags.culling/cullingType; stages render fine double-sided, and a
    //     wrong cull HIDES geometry, so default CullMode=0 (→cm^1=1→'none'). [INFERRED]
    //   bit26 ZWriteDisable (:295 `(isp>>26)&1`, zw = bit?0:1). Opaque writes depth
    //     (bit=0); translucent the renderer overrides zw=1 anyway (:328). [CONFIRMED]
    const DM_GEQ = 6, CULL_NONE = 0;
    const zwriteDis = isTrans ? 1 : 0;
    const isp = (DM_GEQ << 29) | (CULL_NONE << 27) | (zwriteDis << 26);

    // ─ tcw (Texture Control Word) ─ OUR shim texMgr keys off this surrogate int
    //   (getTexture(tsp, tcw, vram) → surrToIdx). The real TA tcw packs the VRAM
    //   addr/format; we don't decode from VRAM, so a 1-based surrogate is sufficient
    //   and unique per stage texture. [CONFIRMED — shim path]
    const tcw = surr;

    return { isp, tsp, tcw, pcw };
  }

  // ── c) shim texMgr: surrogate tcw → pre-uploaded GPUTexture (no VRAM decode) ──
  _uploadTextures(texImgs) {
    const dev = this.dev;
    this._texByIdx.clear();
    for (let i = 0; i < (texImgs || []).length; i++) {
      const img = texImgs[i];
      if (!img) continue;
      const w = img.width, h = img.height;
      const texture = dev.createTexture({ size: [w, h], format: 'rgba8unorm',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT });
      dev.queue.copyExternalImageToTexture({ source: img }, { texture }, [w, h]);
      const sampler = dev.createSampler({ minFilter: 'linear', magFilter: 'linear',
        addressModeU: 'repeat', addressModeV: 'repeat' });
      this._texByIdx.set(i, { texture, sampler, w, h });
    }
    const self = this;
    this._tm = {
      getFallbackTexture() {
        if (self._fallback) return self._fallback;
        const t = dev.createTexture({ size: [1, 1], format: 'rgba8unorm',
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
        dev.queue.writeTexture({ texture: t }, new Uint8Array([255, 255, 255, 255]), { bytesPerRow: 4 }, [1, 1]);
        self._fallback = { texture: t, sampler: dev.createSampler({ minFilter: 'nearest', magFilter: 'nearest' }) };
        return self._fallback;
      },
      // PVR2Renderer calls getTexture(tsp, tcw, vram); we key off tcw (surrogate).
      getTexture(_tsp, tcw, _vram) {
        const idx = self._surrToIdx.get(tcw >>> 0);
        if (idx === undefined) return null;
        return self._texByIdx.get(idx) || null;
      },
    };
  }

  // ── d) renderStage: draw the OP layer FIRST (behind chars/effects) ──
  // renderer = a PVR2Renderer (the shared device one, or a dedicated background
  // PVR2Renderer bound via initShared). renderTarget optional for offscreen.
  // dbgExtra lets the caller pass charScale/zoom etc; we force the stage-correct flags.
  renderStage(renderer, renderTarget, dbgExtra) {
    if (!this._parsed || !this._tm || !this._parsed.vertexCount) return false;
    const dbg = Object.assign({
      singlePass: true,        // stages are one pass
      noSort: true,            // submission order (the static stage tree is pre-ordered)
      // background/back layer: clear to opaque black when it's the FIRST/own layer,
      // or transparent when composited as an overlay — caller decides via dbgExtra.
      drawOpaque: true, drawPunch: false, drawTrans: true,
    }, dbgExtra || {});
    renderer.renderFrame(this._parsed, this._tm, STAGE_PVRSNAP, null, dbg, renderTarget);
    return true;
  }

  get parsed() { return this._parsed; }
  get texMgr() { return this._tm; }
}

// Preferred entry point: load by stage id (0..0x10) from a base dir that holds
// <base>/STGxx/STGxx.json + tex_NN.png (the decoder's --out layout).
export async function loadStageById(device, base, stageId) {
  const sid = (stageId >>> 0).toString(16).toUpperCase().padStart(2, '0');
  const data = await (await fetch(`${base}/STG${sid}/STG${sid}.json`)).json();
  const texImgs = await Promise.all((data.textureList || []).map(async (t, i) => {
    const nn = String(i).padStart(2, '0');
    try {
      const blob = await (await fetch(`${base}/STG${sid}/tex_${nn}.png`)).blob();
      return await createImageBitmap(blob);
    } catch { return null; }
  }));
  const adapter = new StageRenderAdapter(device);
  await adapter.loadStage(data, texImgs);
  return adapter;
}

// ════════════════════════════════════════════════════════════════════════════
// REMAINING WIRING (handoff — none of this touches the rasterizer):
//
// 1. CAMERA CALIBRATION (the one real unknown). DEFAULT_CAM is a guess. To pin it:
//    capture a live STG00 TA frame (PVR2Renderer renders it perfectly today),
//    then tune fovDeg/eyeZ/targetZ via setCamera() until the stage features land
//    on the live frame in the webgpu-test.html DIFF (green=truth / red=ours).
//    The exact matrix lives in bank12 world→screen (loc_8c1216c0), driven each
//    frame by camera_x/y (0x8C1F9CD8/CDC on the GSTA wire) — fold those into
//    eyeX/targetX once recovered for scrolling/zoom parity.
//
// 2. stage_id ON THE WIRE → swap stages. stage_id is gamestate (0x8C289638 global
//    / per-stage file = identity). Add it to GSTA (producer side, NOT touched here),
//    then on the client: on change, loadStageById(dev, base, stage_id) and keep the
//    returned adapter; render it each frame before chars.
//
// 3. COMPOSITE ORDER in webgpu-test.html: STAGE → CHARS → EFFECTS. Render the stage
//    adapter FIRST into the same color target (renderStage(R, rt, {})), with the
//    char/sprite path drawing AFTER with loadOp:'load' (don't clear). The stage owns
//    the clear (opaque black) as the back layer; chars/effects composite on top.
//    If chars render to a separate canvas, give the stage its OWN background
//    PVR2Renderer via initShared(bgCanvas, R.dev) and z-order the canvases.
//
// 4. ANIMATION (deferred). stage_anim_timer (0x8C1F9D80) low bit drives an A/B
//    keyframe toggle (loc_8c0338ec). The decoder currently emits the static A-phase;
//    animating needs the decoder to additionally dump keyframe lists and this adapter
//    to swap affected meshes' verts/UVs by (animTimer&1) then rebuild.
// ════════════════════════════════════════════════════════════════════════════
