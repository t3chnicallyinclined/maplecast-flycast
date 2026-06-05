// sprite-gpu.mjs — WebGPU 2D sprite renderer for the ROM-asset client (Option 6).
//
// Draws character sprites as instanced textured quads into the PostProcessor's
// offscreen target, then blits through the effect chain — so the sprite client
// gets the SAME bloom/CRT/scanline/etc. effects as the live TA render.
//
// Skin / hit-flash recolor (reuses the renderer's palette technique): each
// character's body-16 colors (the rip's default palette, per-char from the JSON)
// are match-replaced in the fragment shader with the LIVE palette — T._pal at PVR
// bank 256+128*slot, the very palette RAM pvr2-renderer reads. Default skin =>
// live==default => no-op; community skin / hit-flash => recolor for free.
//
// We still generate the quad geometry ourselves (the 253-byte state carries no
// draw commands) — that's the only "from scratch" part; everything downstream
// (effects, palette) reuses the renderer's machinery. Canvas2D is the fallback.

import { PostProcessor } from './post-process.mjs?v=2';

const SHADER = `
struct VSOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f, @location(1) @interpolate(flat) palBase: u32 };
struct U { canvas: vec2f, pad: vec2f };
@group(0) @binding(0) var atlasTex: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;
@group(0) @binding(2) var<uniform> u: U;
@group(0) @binding(3) var<storage, read> pal: array<vec4f>;   // per group: [0..15]=default body, [16..31]=live body

@vertex
fn vs(@builtin(vertex_index) vi: u32,
      @location(0) dest: vec4f,    // x,y,w,h canvas px
      @location(1) auv: vec4f,     // u0,v0,u1,v1 atlas UV
      @location(2) flip: f32,
      @location(3) palBase: f32) -> VSOut {
  var corners = array<vec2f,6>(
    vec2f(0.,0.), vec2f(1.,0.), vec2f(0.,1.),
    vec2f(0.,1.), vec2f(1.,0.), vec2f(1.,1.));
  let c = corners[vi];
  let px = dest.x + c.x * dest.z;
  let py = dest.y + c.y * dest.w;
  let clip = vec2f(px / u.canvas.x * 2. - 1., 1. - py / u.canvas.y * 2.);
  var ux = c.x;
  if (flip > 0.5) { ux = 1. - c.x; }
  let uv = vec2f(mix(auv.x, auv.z, ux), mix(auv.y, auv.w, c.y));
  var o: VSOut; o.pos = vec4f(clip, 0., 1.); o.uv = uv; o.palBase = u32(palBase + 0.5); return o;
}
@fragment
fn fs(i: VSOut) -> @location(0) vec4f {
  let col = textureSample(atlasTex, samp, i.uv);
  if (col.a < 0.5) { discard; }
  // full-palette recolor: nearest default color -> live color, but ONLY where the
  // live palette differs from default. So super-glow/auras tint everything, skins
  // leave unchanged sub-palettes alone, and color collisions keep the original.
  var bi = -1; var bd = 0.02;
  for (var k = 0u; k < 128u; k = k + 1u) {
    let d = distance(col.rgb, pal[i.palBase + k].rgb);
    if (d < bd) { bd = d; bi = i32(k); }
  }
  var rgb = col.rgb;
  if (bi >= 0) {
    let dcol = pal[i.palBase + u32(bi)].rgb;
    let lcol = pal[i.palBase + 128u + u32(bi)].rgb;
    if (distance(dcol, lcol) > 0.012) { rgb = lcol; }
  }
  return vec4f(rgb, col.a);
}

// ---- hit-spark pass (additive; black contributes nothing) ----
struct SOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f, @location(1) @interpolate(flat) a: f32 };
@vertex
fn vs_spark(@builtin(vertex_index) vi: u32,
            @location(0) dest: vec4f,   // x,y = CENTER, z,w = size
            @location(1) auv: vec4f, @location(2) alpha: f32) -> SOut {
  var corners = array<vec2f,6>(vec2f(0.,0.),vec2f(1.,0.),vec2f(0.,1.),vec2f(0.,1.),vec2f(1.,0.),vec2f(1.,1.));
  let c = corners[vi];
  let px = dest.x + (c.x - 0.5) * dest.z;
  let py = dest.y + (c.y - 0.5) * dest.w;
  let clip = vec2f(px / u.canvas.x * 2. - 1., 1. - py / u.canvas.y * 2.);
  let uv = vec2f(mix(auv.x, auv.z, c.x), mix(auv.y, auv.w, c.y));
  var o: SOut; o.pos = vec4f(clip, 0., 1.); o.uv = uv; o.a = alpha; return o;
}
@fragment
fn fs_spark(i: SOut) -> @location(0) vec4f {
  let col = textureSample(atlasTex, samp, i.uv);
  return vec4f(col.rgb * i.a, 1.0);
}`;

const INST_STRIDE = 10 * 4;   // dest(4) + auv(4) + flip(1) + palBase(1)

export class SpriteGPU {
  constructor() {
    this.ok = false; this.chars = {}; this.charPal = {};
    this.maxInst = 64; this.maxGroups = 8; this.recolor = true;
  }

  init(device, canvas) {
    try {
      this.dev = device; this.canvas = canvas;
      this.ctx = canvas.getContext('webgpu');
      this.fmt = navigator.gpu.getPreferredCanvasFormat();
      this.ctx.configure({ device, format: this.fmt, alphaMode: 'opaque' });
      this.PP = new PostProcessor(); this.PP.init(device, this.fmt);
      this.sampler = device.createSampler({ minFilter: 'nearest', magFilter: 'nearest' });
      this.ubuf = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
      this.inst = device.createBuffer({ size: this.maxInst * INST_STRIDE, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
      this.instData = new Float32Array(this.maxInst * 10);
      this.palBuf = device.createBuffer({ size: this.maxGroups * 256 * 16, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
      this.palData = new Float32Array(this.maxGroups * 256 * 4);   // per group: 128 default + 128 live
      this.bgl = device.createBindGroupLayout({ entries: [
        { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
        { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
        { binding: 2, visibility: GPUShaderStage.VERTEX, buffer: { type: 'uniform' } },
        { binding: 3, visibility: GPUShaderStage.FRAGMENT, buffer: { type: 'read-only-storage' } },
      ]});
      const mod = device.createShaderModule({ code: SHADER });
      this.pipe = device.createRenderPipeline({
        layout: device.createPipelineLayout({ bindGroupLayouts: [this.bgl] }),
        vertex: { module: mod, entryPoint: 'vs', buffers: [{
          arrayStride: INST_STRIDE, stepMode: 'instance', attributes: [
            { shaderLocation: 0, offset: 0,  format: 'float32x4' },
            { shaderLocation: 1, offset: 16, format: 'float32x4' },
            { shaderLocation: 2, offset: 32, format: 'float32' },
            { shaderLocation: 3, offset: 36, format: 'float32' },
          ]}]},
        fragment: { module: mod, entryPoint: 'fs', targets: [{ format: this.fmt }] },
        primitive: { topology: 'triangle-list' },
      });
      // hit-spark pipeline: additive blend, no palette (bindings 0,1,2)
      this.sparkBgl = device.createBindGroupLayout({ entries: [
        { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
        { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
        { binding: 2, visibility: GPUShaderStage.VERTEX, buffer: { type: 'uniform' } },
      ]});
      this.sparkPipe = device.createRenderPipeline({
        layout: device.createPipelineLayout({ bindGroupLayouts: [this.sparkBgl] }),
        vertex: { module: mod, entryPoint: 'vs_spark', buffers: [{
          arrayStride: 36, stepMode: 'instance', attributes: [
            { shaderLocation: 0, offset: 0,  format: 'float32x4' },
            { shaderLocation: 1, offset: 16, format: 'float32x4' },
            { shaderLocation: 2, offset: 32, format: 'float32' },
          ]}]},
        fragment: { module: mod, entryPoint: 'fs_spark', targets: [{ format: this.fmt,
          blend: { color: { srcFactor: 'one', dstFactor: 'one', operation: 'add' },
                   alpha: { srcFactor: 'one', dstFactor: 'one', operation: 'add' } } }] },
        primitive: { topology: 'triangle-list' },
      });
      this.sparkInst = device.createBuffer({ size: 32 * 36, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
      this.sparkInstData = new Float32Array(32 * 9);
      this.fxInst = device.createBuffer({ size: 48 * 36, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
      this.fxInstData = new Float32Array(48 * 9);   // live TA effect quads (per-quad texture)
      this.ok = true;
    } catch (e) { console.error('[sprite-gpu] init failed, Canvas2D fallback:', e); this.ok = false; }
    return this.ok;
  }

  // default body palette (rip palette[0..15]) for a char, [[r,g,b],...] 0-255
  setCharPalette(charId, pal128) {
    if (!pal128) return;
    this.charPal[charId] = pal128.map(c => [c[0] / 255, c[1] / 255, c[2] / 255]);
  }

  setAtlas(charId, imageBitmap) {
    if (!this.ok || !imageBitmap) return;
    const w = imageBitmap.width, h = imageBitmap.height;
    const maxDim = (this.dev.limits && this.dev.limits.maxTextureDimension2D) || 8192;
    if (w > maxDim || h > maxDim) {
      console.warn('[sprite-gpu] atlas', charId, w + 'x' + h, '> maxTextureDimension2D', maxDim, '— skipped (needs a wider rebuild)');
      const prev = this.chars[charId]; this.chars[charId] = { skip: true, _pal: prev && prev._pal }; return;
    }
    try {
      const tex = this.dev.createTexture({
        size: [w, h], format: 'rgba8unorm',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT,
      });
      this.dev.queue.copyExternalImageToTexture({ source: imageBitmap }, { texture: tex }, [w, h]);
      const bg = this.dev.createBindGroup({ layout: this.bgl, entries: [
        { binding: 0, resource: tex.createView() },
        { binding: 1, resource: this.sampler },
        { binding: 2, resource: { buffer: this.ubuf } },
        { binding: 3, resource: { buffer: this.palBuf } },
      ]});
      this.chars[charId] = { tex, bg, w, h };
    } catch (e) { console.error('[sprite-gpu] setAtlas failed', charId, e); this.chars[charId] = { skip: true }; }
  }

  // The hit-spark strip (N frames of frameW wide, side by side).
  setSparkAtlas(imageBitmap, frameW) {
    if (!this.ok || !imageBitmap) return;
    const w = imageBitmap.width, h = imageBitmap.height;
    try {
      const tex = this.dev.createTexture({ size: [w, h], format: 'rgba8unorm',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT });
      this.dev.queue.copyExternalImageToTexture({ source: imageBitmap }, { texture: tex }, [w, h]);
      this.sparkBg = this.dev.createBindGroup({ layout: this.sparkBgl, entries: [
        { binding: 0, resource: tex.createView() },
        { binding: 1, resource: this.sampler },
        { binding: 2, resource: { buffer: this.ubuf } },
      ]});
      this.sparkW = w; this.sparkH = h; this.sparkFrame = frameW || 64;
    } catch (e) { console.error('[sprite-gpu] setSparkAtlas failed', e); }
  }

  // sprites: [{charId, slot, sx,sy,sw,sh (atlas px), dx,dy,dw,dh (canvas px), flip}]
  // T: TextureManager (T._pal = live PVR palette RAM, RGBA, 1024 entries).
  render(sprites, dbg, T, sparks, effects) {
    if (!this.ok) return;
    const cw = this.canvas.width, ch = this.canvas.height;
    this.dev.queue.writeBuffer(this.ubuf, 0, new Float32Array([cw, ch, 0, 0]));
    this.PP.ensureTargets(cw, ch, (dbg && dbg.resScale) || 1);
    const rt = this.PP.getRenderTarget();
    const livePal = (this.recolor && T && T._pal) ? T._pal : null;

    const byChar = new Map();
    for (const s of sprites) {
      const c = this.chars[s.charId]; if (!c || !c.bg) continue;
      if (!byChar.has(s.charId)) byChar.set(s.charId, []);
      byChar.get(s.charId).push(s);
    }
    let n = 0, gi = 0; const groups = [];
    for (const [cid, list] of byChar) {
      if (gi >= this.maxGroups || n >= this.maxInst) break;
      const palBase = gi * 256;
      const def = this.charPal[cid];
      const slot = (list[0].slot | 0);
      const bankE = 256 + 128 * slot;     // PVR entry start for this slot's palette region (128 entries)
      for (let k = 0; k < 128; k++) {
        const od = (palBase + k) * 4, ol = (palBase + 128 + k) * 4;
        const dc = def && def[k]; const dr = dc ? dc[0] : 0, dg = dc ? dc[1] : 0, db = dc ? dc[2] : 0;
        this.palData[od] = dr; this.palData[od + 1] = dg; this.palData[od + 2] = db; this.palData[od + 3] = 1;
        if (livePal) {
          const pe = (bankE + k) * 4;
          this.palData[ol] = livePal[pe] / 255; this.palData[ol + 1] = livePal[pe + 1] / 255; this.palData[ol + 2] = livePal[pe + 2] / 255; this.palData[ol + 3] = 1;
        } else { // no live palette -> live = default (recolor is a no-op)
          this.palData[ol] = dr; this.palData[ol + 1] = dg; this.palData[ol + 2] = db; this.palData[ol + 3] = 1;
        }
      }
      const first = n;
      for (const s of list) {
        if (n >= this.maxInst) break;
        const c = this.chars[cid], o = n * 10;
        this.instData[o] = s.dx; this.instData[o + 1] = s.dy; this.instData[o + 2] = s.dw; this.instData[o + 3] = s.dh;
        this.instData[o + 4] = s.sx / c.w; this.instData[o + 5] = s.sy / c.h;
        this.instData[o + 6] = (s.sx + s.sw) / c.w; this.instData[o + 7] = (s.sy + s.sh) / c.h;
        this.instData[o + 8] = s.flip ? 1 : 0; this.instData[o + 9] = palBase;
        n++;
      }
      groups.push({ cid, first, count: n - first }); gi++;
    }
    if (gi) this.dev.queue.writeBuffer(this.palBuf, 0, this.palData, 0, gi * 256 * 4);
    if (n) this.dev.queue.writeBuffer(this.inst, 0, this.instData, 0, n * 10);

    // hit-spark instances (additive): dest center+size, atlas frame UV, alpha
    let sn = 0;
    if (sparks && sparks.length && this.sparkBg) {
      const nframes = Math.max(1, Math.floor(this.sparkW / this.sparkFrame));
      for (const sp of sparks) {
        if (sn >= 32) break;
        const o = sn * 9, f = Math.min(nframes - 1, sp.frame | 0);
        this.sparkInstData[o] = sp.x; this.sparkInstData[o+1] = sp.y; this.sparkInstData[o+2] = sp.size; this.sparkInstData[o+3] = sp.size;
        this.sparkInstData[o+4] = f * this.sparkFrame / this.sparkW; this.sparkInstData[o+5] = 0;
        this.sparkInstData[o+6] = (f + 1) * this.sparkFrame / this.sparkW; this.sparkInstData[o+7] = 1;
        this.sparkInstData[o+8] = sp.alpha;
        sn++;
      }
      if (sn) this.dev.queue.writeBuffer(this.sparkInst, 0, this.sparkInstData, 0, sn * 9);
    }

    const enc = this.dev.createCommandEncoder();
    const pass = enc.beginRenderPass({
      colorAttachments: [{ view: rt.colorView, clearValue: { r:0,g:0,b:0,a:0 }, loadOp: 'clear', storeOp: 'store' }],
    });
    if (n) {
      pass.setPipeline(this.pipe);
      pass.setVertexBuffer(0, this.inst);
      for (const g of groups) {
        if (!g.count) continue;
        pass.setBindGroup(0, this.chars[g.cid].bg);
        pass.draw(6, g.count, 0, g.first);
      }
    }
    if (sn) {                                    // additive hit-spark pass, over the characters
      pass.setPipeline(this.sparkPipe);
      pass.setVertexBuffer(0, this.sparkInst);
      pass.setBindGroup(0, this.sparkBg);
      pass.draw(6, sn, 0, 0);
    }
    // live TA effect quads (beams / energy / lightning) — additive, per-quad texture
    if (effects && effects.length) {
      let m = 0;
      for (const ef of effects) { if (m >= 48) break; const o = m * 9;
        this.fxInstData[o]=ef.x; this.fxInstData[o+1]=ef.y; this.fxInstData[o+2]=ef.w; this.fxInstData[o+3]=ef.h;
        this.fxInstData[o+4]=0; this.fxInstData[o+5]=0; this.fxInstData[o+6]=1; this.fxInstData[o+7]=1;
        this.fxInstData[o+8]=ef.alpha != null ? ef.alpha : 1; m++; }
      if (m) {
        this.dev.queue.writeBuffer(this.fxInst, 0, this.fxInstData, 0, m * 9);
        pass.setPipeline(this.sparkPipe);
        pass.setVertexBuffer(0, this.fxInst);
        let i = 0;
        for (const ef of effects) { if (i >= 48) break;
          try {
            const bg = this.dev.createBindGroup({ layout: this.sparkBgl, entries: [
              { binding: 0, resource: ef.tex.createView() },
              { binding: 1, resource: ef.samp || this.sampler },
              { binding: 2, resource: { buffer: this.ubuf } } ]});
            pass.setBindGroup(0, bg); pass.draw(6, 1, 0, i);
          } catch (_e) {}
          i++;
        }
      }
    }
    pass.end();
    this.PP.blit(enc, this.ctx.getCurrentTexture().createView(), cw, ch, dbg || {});
    this.dev.queue.submit([enc.finish()]);
  }
}
