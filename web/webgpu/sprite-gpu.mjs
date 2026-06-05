// sprite-gpu.mjs — WebGPU 2D sprite renderer for the ROM-asset client (Option 6).
//
// Draws character sprites as textured quads into the PostProcessor's offscreen
// target, then blits through the effect chain — so the sprite client gets the
// SAME bloom/CRT/scanline/etc. effects as the live TA render, on the GPU.
// Reuses the page's WebGPU device. Canvas2D remains the no-WebGPU fallback.
//
// Per-char RGBA atlas textures (one per loaded character, lazy). Instanced
// quads: one draw per character over its on-screen sprites. Positions come
// pre-extrapolated from the SpriteClient (same velocity prediction as Canvas2D).
//
// (Skin/hit-flash recolor — swapping the live body-16 palette — is the next
// layer; this first version samples the rip's baked palette, which is the
// correct default color. The pipeline + atlas plumbing is the foundation for it.)

import { PostProcessor } from './post-process.mjs?v=2';

const SHADER = `
struct VSOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
struct U { canvas: vec2f, pad: vec2f };
@group(0) @binding(0) var atlasTex: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;
@group(0) @binding(2) var<uniform> u: U;

@vertex
fn vs(@builtin(vertex_index) vi: u32,
      @location(0) dest: vec4f,   // x,y,w,h in canvas pixels
      @location(1) auv: vec4f,    // u0,v0,u1,v1 normalized atlas UV
      @location(2) flip: f32) -> VSOut {
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
  var o: VSOut; o.pos = vec4f(clip, 0., 1.); o.uv = uv; return o;
}
@fragment
fn fs(i: VSOut) -> @location(0) vec4f {
  let col = textureSample(atlasTex, samp, i.uv);
  if (col.a < 0.5) { discard; }
  return col;
}`;

const INST_STRIDE = 9 * 4;   // dest(4) + auv(4) + flip(1), f32

export class SpriteGPU {
  constructor() { this.ok = false; this.chars = {}; this.maxInst = 64; }

  // canvas: the <canvas> to present to; device: shared WebGPU device.
  init(device, canvas) {
    try {
      this.dev = device;
      this.canvas = canvas;
      this.ctx = canvas.getContext('webgpu');
      this.fmt = navigator.gpu.getPreferredCanvasFormat();
      this.ctx.configure({ device, format: this.fmt, alphaMode: 'opaque' });
      this.PP = new PostProcessor();
      this.PP.init(device, this.fmt);
      this.sampler = device.createSampler({ minFilter: 'nearest', magFilter: 'nearest' });
      this.ubuf = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
      this.inst = device.createBuffer({ size: this.maxInst * INST_STRIDE, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
      this.instData = new Float32Array(this.maxInst * 9);
      this.bgl = device.createBindGroupLayout({ entries: [
        { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
        { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
        { binding: 2, visibility: GPUShaderStage.VERTEX, buffer: { type: 'uniform' } },
      ]});
      const mod = device.createShaderModule({ code: SHADER });
      this.pipe = device.createRenderPipeline({
        layout: device.createPipelineLayout({ bindGroupLayouts: [this.bgl] }),
        vertex: { module: mod, entryPoint: 'vs', buffers: [{
          arrayStride: INST_STRIDE, stepMode: 'instance', attributes: [
            { shaderLocation: 0, offset: 0,  format: 'float32x4' },
            { shaderLocation: 1, offset: 16, format: 'float32x4' },
            { shaderLocation: 2, offset: 32, format: 'float32' },
          ]}]},
        fragment: { module: mod, entryPoint: 'fs', targets: [{ format: this.fmt }] },
        primitive: { topology: 'triangle-list' },
      });
      this.ok = true;
    } catch (e) { console.error('[sprite-gpu] init failed, Canvas2D fallback:', e); this.ok = false; }
    return this.ok;
  }

  // Upload (or replace) a character's atlas as a GPU texture + bind group.
  setAtlas(charId, imageBitmap) {
    if (!this.ok || !imageBitmap) return;
    const w = imageBitmap.width, h = imageBitmap.height;
    const tex = this.dev.createTexture({
      size: [w, h], format: 'rgba8unorm',
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT,
    });
    this.dev.queue.copyExternalImageToTexture({ source: imageBitmap }, { texture: tex }, [w, h]);
    const bg = this.dev.createBindGroup({ layout: this.bgl, entries: [
      { binding: 0, resource: tex.createView() },
      { binding: 1, resource: this.sampler },
      { binding: 2, resource: { buffer: this.ubuf } },
    ]});
    this.chars[charId] = { tex, bg, w, h };
  }

  // sprites: [{charId, sx,sy,sw,sh (atlas px), dx,dy,dw,dh (canvas px), flip}]
  // Grouped by charId internally so each char's atlas binds once.
  render(sprites, dbg) {
    if (!this.ok) return;
    const cw = this.canvas.width, ch = this.canvas.height;
    this.dev.queue.writeBuffer(this.ubuf, 0, new Float32Array([cw, ch, 0, 0]));
    this.PP.ensureTargets(cw, ch, (dbg && dbg.resScale) || 1);
    const rt = this.PP.getRenderTarget();

    // group sprites by char (contiguous in the instance buffer)
    const byChar = new Map();
    for (const s of sprites) {
      const c = this.chars[s.charId]; if (!c) continue;
      if (!byChar.has(s.charId)) byChar.set(s.charId, []);
      byChar.get(s.charId).push(s);
    }
    let n = 0; const groups = [];
    for (const [cid, list] of byChar) {
      const first = n;
      for (const s of list) {
        if (n >= this.maxInst) break;
        const c = this.chars[cid];
        const o = n * 9;
        this.instData[o]   = s.dx; this.instData[o+1] = s.dy; this.instData[o+2] = s.dw; this.instData[o+3] = s.dh;
        this.instData[o+4] = s.sx / c.w; this.instData[o+5] = s.sy / c.h;
        this.instData[o+6] = (s.sx + s.sw) / c.w; this.instData[o+7] = (s.sy + s.sh) / c.h;
        this.instData[o+8] = s.flip ? 1 : 0;
        n++;
      }
      groups.push({ cid, first, count: n - first });
    }
    if (n) this.dev.queue.writeBuffer(this.inst, 0, this.instData, 0, n * 9);

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
    pass.end();
    this.PP.blit(enc, this.ctx.getCurrentTexture().createView(), cw, ch, dbg || {});
    this.dev.queue.submit([enc.finish()]);
  }
}
