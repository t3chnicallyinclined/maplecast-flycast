//! pvr2 renderer ported to wgpu. Consumes a parsed TA frame + VRAM + palette and
//! draws it, reusing the WGSL from shaders.wgsl. First-frame scope: opaque ->
//! punch-through -> translucent in submission order (per-triangle translucent
//! sort + tile-clip scissor are deferred). Non-sRGB target, reverse-Z depth.

use crate::ta::{Parsed, PolyParam};
use crate::texture::{self, Wrap};
use std::collections::HashMap;

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

    pub fn render(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        view: &wgpu::TextureView,
        parsed: &Parsed,
        vram: &[u8],
        palette: &[u8],
        pvr_snapshot: &[u32; 16],
        width: u32,
        height: u32,
        bodies: &[crate::ffi::SceneQuad],
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

                draws.push(Draw {
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
                });
                slot += 1;
            }
        }

        // --- body quads: render_frame output, force-colored silhouettes ---
        let mut body_verts: Vec<u8> = Vec::new();
        let mut body_draws: Vec<Draw> = Vec::new();
        for q in bodies {
            if !show_bodies || (slot as u64) >= MAX_SLOTS {
                break;
            }
            let base = (body_verts.len() / 28) as u32; // 0-based within body_vbuf
            for (x, y) in [(q.ax, q.ay), (q.bx, q.by), (q.cx, q.cy), (q.dx, q.dy)] {
                push_body_vert(&mut body_verts, x, y, q.z);
            }
            write_fu(&mut fu_stage, (slot as usize) * SLOT as usize, 0.0, 0, 0, 1, 0, 0, 0);
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
                sb: 4, // src-alpha
                db: 5, // one-minus-src-alpha
                dm: 6, // greater-equal
                dw: false,
                tcw: 0,
                tsp: 0,
                textured: false,
            });
            slot += 1;
        }

        debug.stage_quads.store(draws.len() as u64, Relaxed);
        debug.body_quads.store(body_draws.len() as u64, Relaxed);

        if draws.is_empty() && body_draws.is_empty() {
            // nothing to draw yet — clear only
            self.clear_only(device, queue, view);
            return;
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

        // decode textures -> bind groups (cache per (tcw,tsp) within the frame)
        let mut tex_cache: HashMap<(u32, u32), wgpu::BindGroup> = HashMap::new();
        let mut keep_tex: Vec<wgpu::Texture> = Vec::new();
        for d in &draws {
            if !d.textured {
                continue;
            }
            let key = (d.tcw, d.tsp);
            if tex_cache.contains_key(&key) {
                continue;
            }
            if let Some(t) = texture::decode(d.tcw, d.tsp, vram, palette) {
                let (bg, tex) = make_tex_bg(
                    device,
                    &self.bgl1,
                    &t.rgba,
                    t.w,
                    t.h,
                    t.filter_linear,
                    t.wrap_u,
                    t.wrap_v,
                    Some(queue),
                );
                keep_tex.push(tex);
                tex_cache.insert(key, bg);
            }
        }

        // Warm the pipeline cache (needs &mut self) BEFORE the render pass, so the
        // pass only takes shared borrows.
        for d in draws.iter().chain(body_draws.iter()) {
            self.pipe(device, d.sb, d.db, d.dm, d.dw);
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
            // MvC2 is 640x480 (4:3). Render into a centered 4:3 viewport so the
            // content keeps its aspect at any (e.g. 16:9) window size — pillarbox.
            let (vx, vy, vw, vh) = pillarbox_4x3(width, height);
            rp.set_viewport(vx, vy, vw, vh, 0.0, 1.0);
            rp.set_vertex_buffer(0, self.vbuf.slice(..));
            rp.set_index_buffer(self.ibuf.slice(..), wgpu::IndexFormat::Uint32);

            for d in &draws {
                rp.set_pipeline(&self.pipes[&pipe_key(d.sb, d.db, d.dm, d.dw)]);
                let tbg = if d.textured {
                    tex_cache.get(&(d.tcw, d.tsp)).unwrap_or(&self.white_bg)
                } else {
                    &self.white_bg
                };
                rp.set_bind_group(1, tbg, &[]);
                rp.set_bind_group(0, &self.ubg, &[d.slot * SLOT as u32]);
                rp.draw_indexed(d.idx_first..d.idx_first + d.idx_count, 0, 0..1);
            }

            // bodies: rebind the body vertex buffer (its indices are 0-based)
            if !body_draws.is_empty() {
                rp.set_vertex_buffer(0, self.body_vbuf.slice(..));
                for d in &body_draws {
                    rp.set_pipeline(&self.pipes[&pipe_key(d.sb, d.db, d.dm, d.dw)]);
                    rp.set_bind_group(1, &self.white_bg, &[]);
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

/// One force-colored body vertex (28B): pos + magenta ~70% + zero offset + zero uv.
fn push_body_vert(buf: &mut Vec<u8>, x: f32, y: f32, z: f32) {
    buf.extend_from_slice(&x.to_le_bytes());
    buf.extend_from_slice(&y.to_le_bytes());
    buf.extend_from_slice(&z.to_le_bytes());
    buf.extend_from_slice(&[0xFF, 0x00, 0xFF, 0xB4]); // R,G,B,A
    buf.extend_from_slice(&[0, 0, 0, 0]); // offset color
    buf.extend_from_slice(&0f32.to_le_bytes()); // u
    buf.extend_from_slice(&0f32.to_le_bytes()); // v
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
