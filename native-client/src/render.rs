//! pvr2 renderer ported to wgpu. Consumes a parsed TA frame + VRAM + palette and
//! draws it, reusing the WGSL from shaders.wgsl. First-frame scope: opaque ->
//! punch-through -> translucent in submission order (per-triangle translucent
//! sort + tile-clip scissor are deferred). Non-sRGB target, reverse-Z depth.

use crate::ta::{Parsed, PolyParam};
use crate::texture::{self, Wrap};
use std::collections::HashMap;
use std::sync::Arc;

const DEPTH_FMT: wgpu::TextureFormat = wgpu::TextureFormat::Depth32Float;
const SLOT: u64 = 256; // dynamic-uniform stride (>= min alignment)
const MAX_SLOTS: u64 = 8192;

pub struct Renderer {
    format: wgpu::TextureFormat,
    shader: wgpu::ShaderModule,
    bgl1: wgpu::BindGroupLayout,
    layout: wgpu::PipelineLayout,
    pipes: HashMap<u64, wgpu::RenderPipeline>,
    ubuf: wgpu::Buffer,   // 64B ndcMat
    dynbuf: wgpu::Buffer, // FU, SLOT * MAX_SLOTS
    ubg: wgpu::BindGroup,
    vbuf: wgpu::Buffer,
    ibuf: wgpu::Buffer,
    vcap: u64,
    icap: u64,
    body_vbuf: wgpu::Buffer,
    body_vcap: u64,
    depth: Option<(wgpu::TextureView, u32, u32)>,
    white_bg: wgpu::BindGroup,
    _white_tex: wgpu::Texture,
    // Cross-frame body-tile cache: content-hash(rgba) -> GPU texture+bindgroup. A decoded
    // body part is uploaded ONCE and reused while unchanged, instead of re-creating a GPU
    // texture per part every frame (the per-frame churn that spiked render load).
    body_cache: HashMap<u64, CachedTex>,
    // Cross-frame STAGE/EFFECT texture cache: source-fingerprint(vram+palette bank) -> GPU
    // texture+bindgroup. Same idea as body_cache but for the wire TA's stage/effect
    // textures, which previously re-decoded + re-created a GPU texture for EVERY (tcw,tsp)
    // EVERY server frame — the per-frame churn that spiked client render load on 480KB
    // super frames (static backgrounds re-decoded needlessly; super particle textures
    // re-decoded every frame for the whole super). Keyed by SOURCE content so a stage
    // animation / texture stream / skin-palette swap still re-decodes correctly.
    stage_cache: HashMap<u64, CachedTex>,
    body_frame_ctr: u64,
    frame_uploads: u32,
    frame_stage_uploads: u32,
    // Persisted draw state: an UNCHANGED server frame re-submits these without rebuilding,
    // so presentation stays continuous+smooth while the heavy decode runs only on a new
    // frame. Filled by rebuild(), consumed by submit().
    frame_draws: Vec<Draw>,
    frame_body_draws: Vec<Draw>,
    frame_tr_draws: Vec<Draw>,
    // Per-frame stage/effect bindgroups, indexed by Draw.tcw (like frame_body_bgs). Each
    // Arc is held for the frame so submit() re-presents without touching the cache.
    frame_stage_bgs: Vec<Arc<wgpu::BindGroup>>,
    frame_body_bgs: Vec<Arc<wgpu::BindGroup>>,
    // Keeps uncached (MAPLECAST_STAGECACHE=0) stage textures alive for the frame — the
    // A/B-baseline path that re-decodes every frame like the pre-cache code did.
    frame_keep_tex: Vec<wgpu::Texture>,
    // Cross-frame stage cache on (default) vs off (MAPLECAST_STAGECACHE=0 = re-decode every
    // frame, the pre-fix behavior) — lets us A/B render_ms in one binary on the same scene.
    stage_cache_on: bool,
    has_content: bool,
}

struct CachedTex {
    bg: Arc<wgpu::BindGroup>, // Arc: wgpu 0.20 BindGroup isn't Clone; Arc clone is cheap
    _tex: wgpu::Texture,      // keeps the texture alive while the bindgroup references it
    last: u64,                // body_frame_ctr at last use — for age-based eviction
}

struct Draw {
    slot: u32,
    idx_first: u32,
    idx_count: u32,
    sb: u32,
    db: u32,
    dm: u32,
    dw: bool,
    tcw: u32,
    tsp: u32,
    textured: bool,
}

impl Renderer {
    pub fn new(device: &wgpu::Device, format: wgpu::TextureFormat) -> Self {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("pvr2"),
            source: wgpu::ShaderSource::Wgsl(include_str!("shaders.wgsl").into()),
        });

        let bgl0 = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("bgl0"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::VERTEX,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: true,
                        min_binding_size: std::num::NonZeroU64::new(32),
                    },
                    count: None,
                },
            ],
        });
        let bgl1 = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("bgl1"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
            ],
        });
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("pvr2-layout"),
            bind_group_layouts: &[&bgl0, &bgl1],
            push_constant_ranges: &[],
        });

        let ubuf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("ndc"),
            size: 64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let dynbuf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("fu"),
            size: SLOT * MAX_SLOTS,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let ubg = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("ubg"),
            layout: &bgl0,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: ubuf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                        buffer: &dynbuf,
                        offset: 0,
                        size: std::num::NonZeroU64::new(32),
                    }),
                },
            ],
        });

        let vbuf = mkbuf(device, 1 << 20, wgpu::BufferUsages::VERTEX);
        let ibuf = mkbuf(device, 1 << 20, wgpu::BufferUsages::INDEX);
        let body_vbuf = mkbuf(device, 1 << 16, wgpu::BufferUsages::VERTEX);

        // 1x1 white fallback texture for untextured polys.
        let (white_bg, white_tex) = make_tex_bg(
            device,
            &bgl1,
            &[255, 255, 255, 255],
            1,
            1,
            false,
            Wrap::Clamp,
            Wrap::Clamp,
            None,
        );

        Self {
            format,
            shader,
            bgl1,
            layout,
            pipes: HashMap::new(),
            ubuf,
            dynbuf,
            ubg,
            vbuf,
            ibuf,
            vcap: 1 << 20,
            icap: 1 << 20,
            body_vbuf,
            body_vcap: 1 << 16,
            depth: None,
            white_bg,
            _white_tex: white_tex,
            body_cache: HashMap::new(),
            stage_cache: HashMap::new(),
            body_frame_ctr: 0,
            frame_uploads: 0,
            frame_stage_uploads: 0,
            frame_draws: Vec::new(),
            frame_body_draws: Vec::new(),
            frame_tr_draws: Vec::new(),
            frame_stage_bgs: Vec::new(),
            frame_body_bgs: Vec::new(),
            frame_keep_tex: Vec::new(),
            stage_cache_on: std::env::var("MAPLECAST_STAGECACHE").ok().as_deref() != Some("0"),
            has_content: false,
        }
    }

    fn pipe(&mut self, device: &wgpu::Device, sb: u32, db: u32, dm: u32, dw: bool) -> &wgpu::RenderPipeline {
        let key = pipe_key(sb, db, dm, dw);
        let format = self.format;
        let shader = &self.shader;
        let layout = &self.layout;
        self.pipes.entry(key).or_insert_with(|| {
            let blend = wgpu::BlendState {
                color: wgpu::BlendComponent { src_factor: sbm(sb), dst_factor: dbm(db), operation: wgpu::BlendOperation::Add },
                alpha: wgpu::BlendComponent { src_factor: sbm(sb), dst_factor: dbm(db), operation: wgpu::BlendOperation::Add },
            };
            let attrs = [
                wgpu::VertexAttribute { shader_location: 0, offset: 0, format: wgpu::VertexFormat::Float32x3 },
                wgpu::VertexAttribute { shader_location: 1, offset: 12, format: wgpu::VertexFormat::Unorm8x4 },
                wgpu::VertexAttribute { shader_location: 2, offset: 16, format: wgpu::VertexFormat::Unorm8x4 },
                wgpu::VertexAttribute { shader_location: 3, offset: 20, format: wgpu::VertexFormat::Float32x2 },
            ];
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: None,
                layout: Some(layout),
                vertex: wgpu::VertexState {
                    module: shader,
                    entry_point: "vs_main",
                    buffers: &[wgpu::VertexBufferLayout {
                        array_stride: 28,
                        step_mode: wgpu::VertexStepMode::Vertex,
                        attributes: &attrs,
                    }],
                    compilation_options: Default::default(),
                },
                fragment: Some(wgpu::FragmentState {
                    module: shader,
                    entry_point: "fs_main",
                    targets: &[Some(wgpu::ColorTargetState {
                        format,
                        blend: Some(blend),
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: Default::default(),
                }),
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    front_face: wgpu::FrontFace::Cw,
                    cull_mode: None,
                    ..Default::default()
                },
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: DEPTH_FMT,
                    depth_write_enabled: dw,
                    depth_compare: dcm(dm),
                    stencil: Default::default(),
                    bias: Default::default(),
                }),
                multisample: wgpu::MultisampleState::default(),
                multiview: None,
            })
        })
    }

    /// Decode + upload ONE server frame into persistent GPU state (draw lists, buffers,
    /// bind groups). Call only when the server frame changed; submit() then presents it —
    /// and cheaply re-presents it on unchanged frames — so presentation stays smooth.
    pub fn rebuild(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        parsed: &Parsed,
        vram: &[u8],
        palette: &[u8],
        pvr_snapshot: &[u32; 16],
        bodies: &[crate::BodyItem],
        debug: &crate::debug::DebugState,
    ) {
        use std::sync::atomic::Ordering::Relaxed;
        let show_stage = debug.show_stage.load(Relaxed);
        let show_bodies = debug.show_bodies.load(Relaxed);
        // ndcMat from the PVR tile-count register.
        let g = pvr_snapshot[0];
        let w = ((g & 0x3F) + 1) * 32;
        let h = (((g >> 16) & 0x3F) + 1) * 32;
        let (fw, fh) = (w.max(1) as f32, h.max(1) as f32);
        let mut m = [0f32; 16];
        m[0] = 2.0 / fw;
        m[5] = -2.0 / fh;
        m[10] = 1.0;
        m[12] = -1.0;
        m[13] = 1.0;
        m[15] = 1.0;
        queue.write_buffer(&self.ubuf, 0, bytemuck::cast_slice(&m));

        // vertex buffer
        let vd = &parsed.vertex_data;
        if vd.len() as u64 > self.vcap {
            self.vcap = (vd.len() as u64).next_power_of_two();
            self.vbuf = mkbuf(device, self.vcap, wgpu::BufferUsages::VERTEX);
        }
        if !vd.is_empty() {
            queue.write_buffer(&self.vbuf, 0, vd);
        }

        // stage FU + build indices + draw list
        let mut fu_stage = vec![0u8; (SLOT * MAX_SLOTS) as usize];
        let mut indices: Vec<u32> = Vec::new();
        let mut draws: Vec<Draw> = Vec::new();
        // Wire TRANSLUCENT effects (kind 2) are held separately and drawn LAST — after the
        // render_frame bodies — so an effect/assist/kept-wire-satellite can't force-write
        // depth ahead of the bodies and discard their tiles (the "effect draws over
        // everything" root: arm-through-body + missing-tiles-on-hit). Matches the engine/web
        // op -> pt -> (bodies) -> tr order; the shared depth buffer still occludes an effect
        // that is genuinely behind a body.
        let mut tr_draws: Vec<Draw> = Vec::new();
        let mut slot: u32 = 0;

        let lists: [(&Vec<PolyParam>, u8); 3] = [
            (&parsed.opaque, 0),
            (&parsed.punch_through, 1),
            (&parsed.translucent, 2),
        ];
        for (list, kind) in lists {
            if !show_stage {
                break;
            }
            for pp in list {
                if pp.count < 3 || (slot as u64) >= MAX_SLOTS {
                    continue;
                }
                let textured = (pp.pcw >> 3) & 1 == 1;
                // per-list blend/depth (browser stage() rules)
                let (sb, db, dm, dw, at);
                match kind {
                    0 => {
                        // opaque: forced ONE/ZERO; skip dm==0
                        let d = (pp.isp >> 29) & 7;
                        if d == 0 {
                            continue;
                        }
                        sb = 1;
                        db = 0;
                        dm = d;
                        dw = ((pp.isp >> 26) & 1) == 0;
                        at = 0u32;
                    }
                    1 => {
                        sb = (pp.tsp >> 29) & 7;
                        db = (pp.tsp >> 26) & 7;
                        dm = 6;
                        dw = true;
                        at = 1;
                    }
                    _ => {
                        sb = (pp.tsp >> 29) & 7;
                        db = (pp.tsp >> 26) & 7;
                        dm = 6;
                        dw = true;
                        at = 0;
                    }
                }

                // FU uniform (32 bytes)
                let o = (slot as usize) * SLOT as usize;
                let atv: f32 = if at == 1 { 1.0 } else { 0.0 };
                fu_stage[o..o + 4].copy_from_slice(&atv.to_le_bytes());
                let put = |buf: &mut [u8], off: usize, v: u32| buf[off..off + 4].copy_from_slice(&v.to_le_bytes());
                put(&mut fu_stage, o + 4, (pp.tsp >> 6) & 3); // si
                put(&mut fu_stage, o + 8, (pp.pcw >> 3) & 1); // ht
                put(&mut fu_stage, o + 12, (pp.tsp >> 20) & 1); // ua
                put(&mut fu_stage, o + 16, (pp.tsp >> 19) & 1); // ita
                put(&mut fu_stage, o + 20, (pp.pcw >> 2) & 1); // ho
                put(&mut fu_stage, o + 24, at); // at
                put(&mut fu_stage, o + 28, 0); // packed

                // strip -> triangle-list indices
                let idx_first = indices.len() as u32;
                let base = pp.first;
                for i in 0..(pp.count - 2) {
                    let (a, b, c) = if i & 1 == 1 {
                        (base + i + 1, base + i, base + i + 2)
                    } else {
                        (base + i, base + i + 1, base + i + 2)
                    };
                    indices.push(a);
                    indices.push(b);
                    indices.push(c);
                }
                let idx_count = indices.len() as u32 - idx_first;

                let d = Draw {
                    slot,
                    idx_first,
                    idx_count,
                    sb,
                    db,
                    dm,
                    dw,
                    tcw: pp.tcw,
                    tsp: pp.tsp,
                    textured,
                };
                // kind 2 = wire translucent -> deferred past the body pass; op/pt stay first.
                if kind == 2 {
                    tr_draws.push(d);
                } else {
                    draws.push(d);
                }
                slot += 1;
            }
        }

        // --- body quads: render_frame output, textured (or force-colored) ---
        self.body_frame_ctr += 1;
        self.frame_uploads = 0;
        self.frame_stage_uploads = 0;
        {
            // Evict tiles unused for ~3s (bounds VRAM as animations cycle art).
            let f = self.body_frame_ctr;
            self.body_cache.retain(|_, c| c.last + 180 >= f);
            // Same age-based eviction for the stage/effect texture cache: static
            // backgrounds are touched every frame (never evict); transient super/effect
            // textures and superseded skin-palette bakes age out after ~3s.
            self.stage_cache.retain(|_, c| c.last + 180 >= f);
        }
        let mut body_verts: Vec<u8> = Vec::new();
        let mut body_draws: Vec<Draw> = Vec::new();
        let mut body_tex_bgs: Vec<Arc<wgpu::BindGroup>> = Vec::new();
        for item in bodies {
            if !show_bodies || (slot as u64) >= MAX_SLOTS {
                break;
            }
            let q = &item.quad;
            let base = (body_verts.len() / 28) as u32; // 0-based within body_vbuf
            let textured = item.tex.is_some();
            // texU mirror (emitter :4807); the decoded tile IS the sprite, so UV span [0,1].
            let (ulo, uhi) = if q.mirror != 0 { (1.0, 0.0) } else { (0.0, 1.0) };
            // corners A,B,C,D -> UVs A(ulo,1) B(uhi,1) C(uhi,0) D(ulo,0)  (emitter :4801-4812)
            let uvs = [(ulo, 1.0f32), (uhi, 1.0), (uhi, 0.0), (ulo, 0.0)];
            let corners = [(q.ax, q.ay), (q.bx, q.by), (q.cx, q.cy), (q.dx, q.dy)];
            let col: u32 = if textured { 0xFFFF_FFFF } else { 0xB4FF_00FF };
            for k in 0..4 {
                push_body_vert(&mut body_verts, corners[k].0, corners[k].1, q.z, col, uvs[k].0, uvs[k].1);
            }
            // textured -> ht=1, si=1 (modulate the white base with the texture).
            let (si, ht) = if textured { (1u32, 1u32) } else { (0, 0) };
            write_fu(&mut fu_stage, (slot as usize) * SLOT as usize, 0.0, si, ht, 1, 0, 0, 0);
            let tex_ref: u32 = if let Some(bt) = &item.tex {
                // Reuse the cached GPU tile if this exact content was seen recently.
                let bg = self.body_bg(device, queue, &bt.rgba, bt.w, bt.h);
                body_tex_bgs.push(bg);
                (body_tex_bgs.len() - 1) as u32
            } else {
                u32::MAX
            };
            let idx_first = indices.len() as u32;
            for &(a, b, c) in &[(0u32, 1, 2), (0, 2, 3)] {
                indices.push(base + a);
                indices.push(base + b);
                indices.push(base + c);
            }
            let idx_count = indices.len() as u32 - idx_first;
            body_draws.push(Draw {
                slot,
                idx_first,
                idx_count,
                sb: 4,
                db: 5,
                dm: 6,
                // depth-WRITE ON: the engine's body/cape/satellite list occludes by a
                // written depth buffer (ISP DepthMode=4/Greater, ZWriteDisable=0), not
                // paint order. q.z = 1/(W+0.001·k) (render_frame.c:623) then orders parts
                // so the cape/satellite sit BEHIND the body. Matches the web path
                // (pvr2-renderer.mjs:10 "write=ON, func=greater-equal"). No sort (that
                // flickers). alpha-0 texels are already discarded (shaders.wgsl:81) so
                // transparent regions write no depth.
                dw: true,
                tcw: tex_ref, // body: index into body_tex_bgs (u32::MAX = force-color)
                tsp: 0,
                textured,
            });
            slot += 1;
        }

        debug.stage_quads.store((draws.len() + tr_draws.len()) as u64, Relaxed);
        debug.body_quads.store(body_draws.len() as u64, Relaxed);
        debug.body_uploads.store(self.frame_uploads as u64, Relaxed);
        // NOTE: stage_uploads/stage_tex are stored AFTER the resolve loop below (that loop
        // is what populates frame_stage_uploads + frame_stage_bgs).

        self.has_content = !(draws.is_empty() && body_draws.is_empty() && tr_draws.is_empty());
        if !self.has_content {
            self.frame_draws.clear();
            self.frame_body_draws.clear();
            self.frame_tr_draws.clear();
            return; // submit() will clear the surface
        }

        queue.write_buffer(&self.dynbuf, 0, &fu_stage[..(slot as usize) * SLOT as usize]);

        let ib: &[u8] = bytemuck::cast_slice(&indices);
        if ib.len() as u64 > self.icap {
            self.icap = (ib.len() as u64).next_power_of_two();
            self.ibuf = mkbuf(device, self.icap, wgpu::BufferUsages::INDEX);
        }
        queue.write_buffer(&self.ibuf, 0, ib);

        if !body_verts.is_empty() {
            if body_verts.len() as u64 > self.body_vcap {
                self.body_vcap = (body_verts.len() as u64).next_power_of_two();
                self.body_vbuf = mkbuf(device, self.body_vcap, wgpu::BufferUsages::VERTEX);
            }
            queue.write_buffer(&self.body_vbuf, 0, &body_verts);
        }

        // Resolve stage/effect textures to GPU bindgroups. Each textured Draw.tcw is
        // rewritten to an index into frame_stage_bgs (mirroring the body path); untextured
        // or decode-failed draws fall back to white.
        //
        // Dedup by (tcw,tsp) FIRST (within one frame a given address+format is one texture),
        // so the source-fingerprint hash + cache lookup runs ONCE per unique texture, not
        // once per draw. On a cache hit (steady state) this skips BOTH the CPU decode and
        // the GPU texture/bindgroup creation that previously ran for every texture every
        // frame. The cross-frame cache is keyed by SOURCE fingerprint (NOT (tcw,tsp) — an
        // address can point at different content over time), so a stage anim / texture
        // stream / skin-palette swap re-decodes correctly.
        let cache_on = self.stage_cache_on;
        self.frame_keep_tex.clear();
        let mut frame_stage_bgs: Vec<Arc<wgpu::BindGroup>> = Vec::new();
        let mut txkey2idx: HashMap<(u32, u32), u32> = HashMap::new();
        for d in draws.iter_mut().chain(tr_draws.iter_mut()) {
            if !d.textured {
                d.tcw = u32::MAX;
                continue;
            }
            let tk = (d.tcw, d.tsp);
            let idx = if let Some(&i) = txkey2idx.get(&tk) {
                i
            } else {
                let bg = if cache_on {
                    let fp = texture::source_fingerprint(d.tcw, d.tsp, vram, palette);
                    self.stage_bg(device, queue, d.tcw, d.tsp, vram, palette, fp)
                } else {
                    self.stage_decode_uncached(device, queue, d.tcw, d.tsp, vram, palette)
                };
                let i = match bg {
                    Some(bg) => {
                        let i = frame_stage_bgs.len() as u32;
                        frame_stage_bgs.push(bg);
                        i
                    }
                    None => u32::MAX,
                };
                txkey2idx.insert(tk, i);
                i
            };
            if idx == u32::MAX {
                d.textured = false;
                d.tcw = u32::MAX;
            } else {
                d.tcw = idx;
            }
        }

        // Warm the pipeline cache (needs &mut self) BEFORE the render pass, so the
        // pass only takes shared borrows.
        for d in draws.iter().chain(body_draws.iter()).chain(tr_draws.iter()) {
            self.pipe(device, d.sb, d.db, d.dm, d.dw);
        }

        // Stage texture stats — stored HERE (after the resolve loop populated them).
        debug.stage_uploads.store(self.frame_stage_uploads as u64, Relaxed);
        debug.stage_tex.store(frame_stage_bgs.len() as u64, Relaxed);

        // Persist the built frame so submit() (every present) re-draws it without rebuilding.
        self.frame_draws = draws;
        self.frame_body_draws = body_draws;
        self.frame_tr_draws = tr_draws;
        self.frame_stage_bgs = frame_stage_bgs;
        self.frame_body_bgs = body_tex_bgs;
    }

    /// Mark the last frame as empty (nothing renderable) so submit() clears the surface.
    pub fn set_empty(&mut self) {
        self.has_content = false;
    }

    /// Present the last rebuilt frame into `view`. Cheap (no decode) — call every present so
    /// presentation stays continuous+smooth while rebuild() runs only on a new server frame.
    pub fn submit(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        view: &wgpu::TextureView,
        width: u32,
        height: u32,
    ) {
        if !self.has_content {
            self.clear_only(device, queue, view);
            return;
        }
        self.ensure_depth(device, width, height);
        let depth_view = &self.depth.as_ref().unwrap().0;

        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("frame") });
        {
            let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("pvr2"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color { r: 0.0, g: 0.0, b: 0.0, a: 1.0 }),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: depth_view,
                    depth_ops: Some(wgpu::Operations {
                        load: wgpu::LoadOp::Clear(0.0),
                        store: wgpu::StoreOp::Store,
                    }),
                    stencil_ops: None,
                }),
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            // MvC2 is 640x480 (4:3) — centered pillarbox viewport at any window size.
            let (vx, vy, vw, vh) = pillarbox_4x3(width, height);
            rp.set_viewport(vx, vy, vw, vh, 0.0, 1.0);
            rp.set_vertex_buffer(0, self.vbuf.slice(..));
            rp.set_index_buffer(self.ibuf.slice(..), wgpu::IndexFormat::Uint32);

            for d in &self.frame_draws {
                rp.set_pipeline(&self.pipes[&pipe_key(d.sb, d.db, d.dm, d.dw)]);
                let tbg: &wgpu::BindGroup = if d.textured {
                    self.frame_stage_bgs[d.tcw as usize].as_ref()
                } else {
                    &self.white_bg
                };
                rp.set_bind_group(1, tbg, &[]);
                rp.set_bind_group(0, &self.ubg, &[d.slot * SLOT as u32]);
                rp.draw_indexed(d.idx_first..d.idx_first + d.idx_count, 0, 0..1);
            }

            // bodies: rebind the body vertex buffer (its indices are 0-based)
            if !self.frame_body_draws.is_empty() {
                rp.set_vertex_buffer(0, self.body_vbuf.slice(..));
                for d in &self.frame_body_draws {
                    rp.set_pipeline(&self.pipes[&pipe_key(d.sb, d.db, d.dm, d.dw)]);
                    let tbg: &wgpu::BindGroup = if d.textured {
                        self.frame_body_bgs[d.tcw as usize].as_ref()
                    } else {
                        &self.white_bg
                    };
                    rp.set_bind_group(1, tbg, &[]);
                    rp.set_bind_group(0, &self.ubg, &[d.slot * SLOT as u32]);
                    rp.draw_indexed(d.idx_first..d.idx_first + d.idx_count, 0, 0..1);
                }
            }

            // wire TRANSLUCENT effects LAST (over bodies, still depth-tested).
            if !self.frame_tr_draws.is_empty() {
                rp.set_vertex_buffer(0, self.vbuf.slice(..));
                for d in &self.frame_tr_draws {
                    rp.set_pipeline(&self.pipes[&pipe_key(d.sb, d.db, d.dm, d.dw)]);
                    let tbg: &wgpu::BindGroup = if d.textured {
                        self.frame_stage_bgs[d.tcw as usize].as_ref()
                    } else {
                        &self.white_bg
                    };
                    rp.set_bind_group(1, tbg, &[]);
                    rp.set_bind_group(0, &self.ubg, &[d.slot * SLOT as u32]);
                    rp.draw_indexed(d.idx_first..d.idx_first + d.idx_count, 0, 0..1);
                }
            }
        }
        queue.submit(Some(enc.finish()));
    }

    pub fn clear_only(&self, device: &wgpu::Device, queue: &wgpu::Queue, view: &wgpu::TextureView) {
        let mut enc = device.create_command_encoder(&Default::default());
        {
            let _rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("clear"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color { r: 0.02, g: 0.03, b: 0.05, a: 1.0 }),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            });
        }
        queue.submit(Some(enc.finish()));
    }

    /// Get-or-create the GPU texture+bindgroup for a decoded body tile, cached across
    /// frames by content hash so an unchanged part skips the upload each frame.
    fn body_bg(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        rgba: &[u8],
        w: u32,
        h: u32,
    ) -> Arc<wgpu::BindGroup> {
        let key = fnv1a64(rgba);
        let f = self.body_frame_ctr;
        if let Some(c) = self.body_cache.get_mut(&key) {
            c.last = f;
            return c.bg.clone();
        }
        let (bg, tex) =
            make_tex_bg(device, &self.bgl1, rgba, w, h, false, Wrap::Clamp, Wrap::Clamp, Some(queue));
        let bg = Arc::new(bg);
        self.frame_uploads += 1;
        self.body_cache.insert(key, CachedTex { bg: bg.clone(), _tex: tex, last: f });
        bg
    }

    /// Get-or-create the GPU texture+bindgroup for a stage/effect texture, cached across
    /// frames by its source fingerprint so an unchanged texture skips BOTH the CPU decode
    /// and the GPU upload. Returns None for unsupported formats (caller uses white).
    #[allow(clippy::too_many_arguments)]
    fn stage_bg(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        tcw: u32,
        tsp: u32,
        vram: &[u8],
        palette: &[u8],
        fp: u64,
    ) -> Option<Arc<wgpu::BindGroup>> {
        let f = self.body_frame_ctr;
        if let Some(c) = self.stage_cache.get_mut(&fp) {
            c.last = f;
            return Some(c.bg.clone());
        }
        let t = texture::decode(tcw, tsp, vram, palette)?;
        let (bg, tex) = make_tex_bg(
            device, &self.bgl1, &t.rgba, t.w, t.h, t.filter_linear, t.wrap_u, t.wrap_v, Some(queue),
        );
        let bg = Arc::new(bg);
        self.frame_stage_uploads += 1;
        self.stage_cache.insert(fp, CachedTex { bg: bg.clone(), _tex: tex, last: f });
        Some(bg)
    }

    /// A/B baseline (MAPLECAST_STAGECACHE=0): decode + upload the stage texture EVERY frame,
    /// no cross-frame cache — the pre-fix behavior, for measuring what the cache saves. The
    /// texture is parked in frame_keep_tex so it survives until submit().
    fn stage_decode_uncached(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        tcw: u32,
        tsp: u32,
        vram: &[u8],
        palette: &[u8],
    ) -> Option<Arc<wgpu::BindGroup>> {
        let t = texture::decode(tcw, tsp, vram, palette)?;
        let (bg, tex) = make_tex_bg(
            device, &self.bgl1, &t.rgba, t.w, t.h, t.filter_linear, t.wrap_u, t.wrap_v, Some(queue),
        );
        self.frame_keep_tex.push(tex);
        self.frame_stage_uploads += 1;
        Some(Arc::new(bg))
    }

    fn ensure_depth(&mut self, device: &wgpu::Device, w: u32, h: u32) {
        let ok = matches!(self.depth, Some((_, dw, dh)) if dw == w && dh == h);
        if !ok {
            let t = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("depth"),
                size: wgpu::Extent3d { width: w.max(1), height: h.max(1), depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: DEPTH_FMT,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
                view_formats: &[],
            });
            let v = t.create_view(&Default::default());
            self.depth = Some((v, w, h));
        }
    }
}

fn mkbuf(device: &wgpu::Device, size: u64, usage: wgpu::BufferUsages) -> wgpu::Buffer {
    device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size,
        usage: usage | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    })
}

#[allow(clippy::too_many_arguments)]
fn make_tex_bg(
    device: &wgpu::Device,
    bgl1: &wgpu::BindGroupLayout,
    rgba: &[u8],
    w: u32,
    h: u32,
    filter_linear: bool,
    wrap_u: Wrap,
    wrap_v: Wrap,
    queue: Option<&wgpu::Queue>,
) -> (wgpu::BindGroup, wgpu::Texture) {
    let tex = device.create_texture(&wgpu::TextureDescriptor {
        label: None,
        size: wgpu::Extent3d { width: w.max(1), height: h.max(1), depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba8Unorm,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    });
    if let Some(q) = queue {
        q.write_texture(
            wgpu::ImageCopyTexture {
                texture: &tex,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            rgba,
            wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(w * 4), rows_per_image: Some(h) },
            wgpu::Extent3d { width: w.max(1), height: h.max(1), depth_or_array_layers: 1 },
        );
    }
    let vv = tex.create_view(&Default::default());
    let f = if filter_linear { wgpu::FilterMode::Linear } else { wgpu::FilterMode::Nearest };
    let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
        address_mode_u: wrap(wrap_u),
        address_mode_v: wrap(wrap_v),
        mag_filter: f,
        min_filter: f,
        ..Default::default()
    });
    let bg = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: None,
        layout: bgl1,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&vv) },
            wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&sampler) },
        ],
    });
    (bg, tex)
}

fn wrap(w: Wrap) -> wgpu::AddressMode {
    match w {
        Wrap::Repeat => wgpu::AddressMode::Repeat,
        Wrap::Clamp => wgpu::AddressMode::ClampToEdge,
        Wrap::Mirror => wgpu::AddressMode::MirrorRepeat,
    }
}

/// Centered 4:3 viewport (MvC2 native aspect) within the surface — pillar/letterbox.
fn pillarbox_4x3(w: u32, h: u32) -> (f32, f32, f32, f32) {
    let (w, h) = (w as f32, h as f32);
    let target = 4.0 / 3.0;
    if w / h > target {
        let vw = h * target;
        ((w - vw) * 0.5, 0.0, vw, h)
    } else {
        let vh = w / target;
        (0.0, (h - vh) * 0.5, w, vh)
    }
}

/// One body vertex (28B): pos (z=1/w) + ARGB color as bytes R,G,B,A + zero offset + uv.
fn push_body_vert(buf: &mut Vec<u8>, x: f32, y: f32, z: f32, col: u32, u: f32, v: f32) {
    buf.extend_from_slice(&x.to_le_bytes());
    buf.extend_from_slice(&y.to_le_bytes());
    buf.extend_from_slice(&z.to_le_bytes());
    buf.push(((col >> 16) & 0xFF) as u8);
    buf.push(((col >> 8) & 0xFF) as u8);
    buf.push((col & 0xFF) as u8);
    buf.push(((col >> 24) & 0xFF) as u8);
    buf.extend_from_slice(&[0, 0, 0, 0]); // offset color
    buf.extend_from_slice(&u.to_le_bytes());
    buf.extend_from_slice(&v.to_le_bytes());
}

/// Write a 32-byte FU uniform at `o`.
#[allow(clippy::too_many_arguments)]
fn write_fu(buf: &mut [u8], o: usize, atv: f32, si: u32, ht: u32, ua: u32, ita: u32, ho: u32, at: u32) {
    buf[o..o + 4].copy_from_slice(&atv.to_le_bytes());
    let put = |b: &mut [u8], off: usize, v: u32| b[off..off + 4].copy_from_slice(&v.to_le_bytes());
    put(buf, o + 4, si);
    put(buf, o + 8, ht);
    put(buf, o + 12, ua);
    put(buf, o + 16, ita);
    put(buf, o + 20, ho);
    put(buf, o + 24, at);
    put(buf, o + 28, 0);
}

/// FNV-1a 64 over a body tile's RGBA bytes — the body-texture cache key.
fn fnv1a64(data: &[u8]) -> u64 {
    let mut h = 0xcbf2_9ce4_8422_2325u64;
    for &b in data {
        h ^= b as u64;
        h = h.wrapping_mul(0x0000_0100_0000_01b3);
    }
    h
}

fn pipe_key(sb: u32, db: u32, dm: u32, dw: bool) -> u64 {
    (sb as u64) | (db as u64) << 3 | (dm as u64) << 6 | (dw as u64) << 9
}

fn sbm(i: u32) -> wgpu::BlendFactor {
    use wgpu::BlendFactor::*;
    [Zero, One, Dst, OneMinusDst, SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha][(i & 7) as usize]
}
fn dbm(i: u32) -> wgpu::BlendFactor {
    use wgpu::BlendFactor::*;
    [Zero, One, Src, OneMinusSrc, SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha][(i & 7) as usize]
}
fn dcm(i: u32) -> wgpu::CompareFunction {
    use wgpu::CompareFunction::*;
    [Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always][(i & 7) as usize]
}
