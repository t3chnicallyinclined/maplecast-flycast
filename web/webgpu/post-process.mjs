// post-process.mjs — Resolution scaling + post-processing pipeline
// Renders scene to offscreen texture at Nx resolution, then blits to canvas
// with optional: FXAA, bloom, CRT, color grading, film grain, sharpening

const BLIT_SHADER = /* wgsl */ `
struct PostUniforms {
    resolution: vec2<f32>,   // offscreen texture size
    canvasSize: vec2<f32>,   // output canvas size
    time: f32,
    fxaa: f32,               // 0 or 1
    bloom: f32,              // 0-1 intensity
    crtAdv: f32,             // 0 or 1
    crtCurve: f32,           // 0-1
    grain: f32,              // 0 or 1
    colorTemp: f32,          // -1 to 1
    sat: f32,                // 0-2
    contrast: f32,           // 0.5-2
    bright: f32,             // 0.5-2
    sharp: f32,              // 0-1
    pad: f32,
};

@group(0) @binding(0) var sceneTex: texture_2d<f32>;
@group(0) @binding(1) var sceneSampler: sampler;
@group(0) @binding(2) var<uniform> pu: PostUniforms;

struct VOut { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32> };

@vertex fn vs(@builtin(vertex_index) vi: u32) -> VOut {
    var o: VOut;
    // Fullscreen triangle
    let x = f32((vi & 1u) << 1u) - 1.0;
    let y = f32((vi & 2u)) - 1.0;
    o.pos = vec4<f32>(x, -y, 0.0, 1.0);
    o.uv = vec2<f32>((x + 1.0) * 0.5, (y + 1.0) * 0.5);
    return o;
}

fn hash22(p: vec2<f32>) -> f32 {
    return fract(sin(dot(p, vec2<f32>(12.9898, 78.233))) * 43758.5453);
}

@fragment fn fs(in: VOut) -> @location(0) vec4<f32> {
    var uv = in.uv;
    let px = 1.0 / pu.resolution;

    // === CRT Curvature ===
    if (pu.crtAdv > 0.5) {
        let dc = uv - 0.5;
        let dist = dot(dc, dc);
        uv = uv + dc * dist * pu.crtCurve * 0.5;
        // Vignette
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
            return vec4<f32>(0.0, 0.0, 0.0, 1.0);
        }
    }

    // === Sample scene ===
    var c = textureSampleLevel(sceneTex, sceneSampler, uv, 0.0);

    // === FXAA (simplified luminance-based edge smoothing) ===
    if (pu.fxaa > 0.5) {
        let lC = dot(c.rgb, vec3<f32>(0.299, 0.587, 0.114));
        let lN = dot(textureSampleLevel(sceneTex, sceneSampler, uv + vec2<f32>(0, -px.y), 0.0).rgb, vec3<f32>(0.299, 0.587, 0.114));
        let lS = dot(textureSampleLevel(sceneTex, sceneSampler, uv + vec2<f32>(0, px.y), 0.0).rgb, vec3<f32>(0.299, 0.587, 0.114));
        let lE = dot(textureSampleLevel(sceneTex, sceneSampler, uv + vec2<f32>(px.x, 0), 0.0).rgb, vec3<f32>(0.299, 0.587, 0.114));
        let lW = dot(textureSampleLevel(sceneTex, sceneSampler, uv + vec2<f32>(-px.x, 0), 0.0).rgb, vec3<f32>(0.299, 0.587, 0.114));
        let edge = abs(lN + lS + lE + lW - 4.0 * lC);
        if (edge > 0.05) {
            c = (c + textureSampleLevel(sceneTex, sceneSampler, uv + vec2<f32>(px.x, 0), 0.0)
                   + textureSampleLevel(sceneTex, sceneSampler, uv + vec2<f32>(-px.x, 0), 0.0)
                   + textureSampleLevel(sceneTex, sceneSampler, uv + vec2<f32>(0, px.y), 0.0)
                   + textureSampleLevel(sceneTex, sceneSampler, uv + vec2<f32>(0, -px.y), 0.0)) / 5.0;
        }
    }

    // === Sharpening (unsharp mask) ===
    if (pu.sharp > 0.01) {
        let blur = (textureSampleLevel(sceneTex, sceneSampler, uv + vec2<f32>(px.x, 0), 0.0)
                  + textureSampleLevel(sceneTex, sceneSampler, uv + vec2<f32>(-px.x, 0), 0.0)
                  + textureSampleLevel(sceneTex, sceneSampler, uv + vec2<f32>(0, px.y), 0.0)
                  + textureSampleLevel(sceneTex, sceneSampler, uv + vec2<f32>(0, -px.y), 0.0)) * 0.25;
        c = vec4<f32>(c.rgb + (c.rgb - blur.rgb) * pu.sharp * 2.0, c.a);
    }

    // === Bloom (bright pass + blur approximation) ===
    if (pu.bloom > 0.01) {
        let lum = dot(c.rgb, vec3<f32>(0.299, 0.587, 0.114));
        if (lum > 0.6) {
            var glow = vec3<f32>(0.0);
            for (var i = 1; i <= 4; i++) {
                let r = f32(i) * 2.0;
                glow += textureSampleLevel(sceneTex, sceneSampler, uv + vec2<f32>(r * px.x, 0), 0.0).rgb;
                glow += textureSampleLevel(sceneTex, sceneSampler, uv + vec2<f32>(-r * px.x, 0), 0.0).rgb;
                glow += textureSampleLevel(sceneTex, sceneSampler, uv + vec2<f32>(0, r * px.y), 0.0).rgb;
                glow += textureSampleLevel(sceneTex, sceneSampler, uv + vec2<f32>(0, -r * px.y), 0.0).rgb;
            }
            glow /= 16.0;
            c = vec4<f32>(c.rgb + glow * pu.bloom, c.a);
        }
    }

    // === Advanced CRT (scanlines + shadow mask + phosphor) ===
    if (pu.crtAdv > 0.5) {
        let scanY = sin(uv.y * pu.resolution.y * 3.14159) * 0.5 + 0.5;
        c = vec4<f32>(c.rgb * (0.75 + 0.25 * scanY), c.a);
        // RGB shadow mask
        let maskX = u32(floor(in.uv.x * pu.canvasSize.x)) % 3u;
        if (maskX == 0u) { c.g *= 0.9; c.b *= 0.8; }
        else if (maskX == 1u) { c.r *= 0.8; c.b *= 0.9; }
        else { c.r *= 0.8; c.g *= 0.9; }
        // Vignette
        let vig = 1.0 - dot((in.uv - 0.5) * 1.2, (in.uv - 0.5) * 1.2);
        c = vec4<f32>(c.rgb * max(vig, 0.3), c.a);
    }

    // === Color Temperature ===
    if (abs(pu.colorTemp) > 0.01) {
        c.r = c.r + pu.colorTemp * 0.1;
        c.b = c.b - pu.colorTemp * 0.1;
    }

    // === Saturation ===
    if (abs(pu.sat - 1.0) > 0.01) {
        let gray = dot(c.rgb, vec3<f32>(0.299, 0.587, 0.114));
        c = vec4<f32>(mix(vec3<f32>(gray), c.rgb, pu.sat), c.a);
    }

    // === Contrast + Brightness ===
    c = vec4<f32>((c.rgb - 0.5) * pu.contrast + 0.5, c.a);
    c = vec4<f32>(c.rgb * pu.bright, c.a);

    // === Film Grain ===
    if (pu.grain > 0.5) {
        let n = hash22(in.uv * pu.resolution + vec2<f32>(pu.time * 100.0, pu.time * 73.0));
        c = vec4<f32>(c.rgb + (n - 0.5) * 0.06, c.a);
    }

    return clamp(c, vec4<f32>(0.0), vec4<f32>(1.0));
}
`;

export class PostProcessor {
    constructor() {
        this.dev = null;
        this.pipeline = null;
        this.sampler = null;
        this.uniformBuf = null;
        this.bgl = null;
        this.offscreenTex = null;
        this.offscreenDepth = null;
        this.offscreenView = null;
        this.scale = 1;
        this._w = 0;
        this._h = 0;
    }

    init(device, canvasFmt) {
        this.dev = device;
        this.canvasFmt = canvasFmt;
        this.sampler = device.createSampler({ minFilter: 'linear', magFilter: 'linear' });
        this.uniformBuf = device.createBuffer({ size: 64, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });

        this.bgl = device.createBindGroupLayout({ entries: [
            { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
            { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
            { binding: 2, visibility: GPUShaderStage.FRAGMENT | GPUShaderStage.VERTEX, buffer: { type: 'uniform' } },
        ]});

        const shader = device.createShaderModule({ code: BLIT_SHADER });
        this.pipeline = device.createRenderPipeline({
            layout: device.createPipelineLayout({ bindGroupLayouts: [this.bgl] }),
            vertex: { module: shader, entryPoint: 'vs' },
            fragment: { module: shader, entryPoint: 'fs', targets: [{ format: canvasFmt }] },
            primitive: { topology: 'triangle-list' },
        });
    }

    // Create/resize offscreen render targets
    ensureTargets(baseW, baseH, scale) {
        const w = baseW * scale, h = baseH * scale;
        if (this.offscreenTex && this._w === w && this._h === h) return;
        if (this.offscreenTex) this.offscreenTex.destroy();
        if (this.offscreenDepth) this.offscreenDepth.destroy();

        this._w = w; this._h = h; this.scale = scale;
        this.offscreenTex = this.dev.createTexture({
            size: [w, h], format: 'rgba8unorm',
            usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING,
        });
        this.offscreenDepth = this.dev.createTexture({
            size: [w, h], format: 'depth32float',
            usage: GPUTextureUsage.RENDER_ATTACHMENT,
        });
        this.offscreenView = this.offscreenTex.createView();

        this.bindGroup = this.dev.createBindGroup({ layout: this.bgl, entries: [
            { binding: 0, resource: this.offscreenView },
            { binding: 1, resource: this.sampler },
            { binding: 2, resource: { buffer: this.uniformBuf } },
        ]});
    }

    // Get the offscreen render target for the main renderer to draw into
    getRenderTarget() {
        return {
            colorView: this.offscreenView,
            depthView: this.offscreenDepth.createView(),
            width: this._w,
            height: this._h,
        };
    }

    // Blit offscreen → canvas with post-processing
    blit(encoder, canvasView, canvasW, canvasH, dbg) {
        const uniforms = new Float32Array(16);
        uniforms[0] = this._w;          // resolution.x
        uniforms[1] = this._h;          // resolution.y
        uniforms[2] = canvasW;           // canvasSize.x
        uniforms[3] = canvasH;           // canvasSize.y
        uniforms[4] = (performance.now() / 1000) % 1000; // time
        uniforms[5] = dbg.fxaa ? 1 : 0;
        uniforms[6] = dbg.bloom ? dbg.bloomAmt || 0.3 : 0;
        uniforms[7] = dbg.crtAdv ? 1 : 0;
        uniforms[8] = dbg.crtCurve || 0.2;
        uniforms[9] = dbg.grain ? 1 : 0;
        uniforms[10] = dbg.colorTemp || 0;
        uniforms[11] = dbg.sat ?? 1.0;
        uniforms[12] = dbg.contrast ?? 1.0;
        uniforms[13] = dbg.bright ?? 1.0;
        uniforms[14] = dbg.sharp || 0;
        uniforms[15] = 0; // pad
        this.dev.queue.writeBuffer(this.uniformBuf, 0, uniforms);

        const rp = encoder.beginRenderPass({
            colorAttachments: [{
                view: canvasView,
                clearValue: { r: 0, g: 0, b: 0, a: 1 },
                loadOp: 'clear', storeOp: 'store',
            }],
        });
        rp.setPipeline(this.pipeline);
        rp.setBindGroup(0, this.bindGroup);
        rp.draw(3); // fullscreen triangle
        rp.end();
    }
}
