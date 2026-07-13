//! MapleCast native client — fresh wgpu port of the working WebGPU renderer.
//!
//! Consumes exactly the thin ZCS2/GSTA wire that webgpu-test.html renders when
//! "ZCS2" is checked: direct wss:// to nobd, native controller -> UDP:7100 input,
//! no webview. The render (TA parse + wgpu pipeline) drops in on the shared
//! FrameDecoder as the M2 modules land.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::sync::{Arc, Mutex};
use winit::{
    event::{Event, WindowEvent},
    event_loop::EventLoop,
    window::{Window, WindowBuilder},
};

mod body_tex;
mod debug;
mod ffi;
mod frame;
mod input;
mod net;
mod render;
mod replica;
mod ta;
mod texture;
mod zcs2;

use frame::FrameDecoder;

struct Gpu {
    instance: wgpu::Instance,
    adapter: wgpu::Adapter,
    surface: wgpu::Surface<'static>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    config: wgpu::SurfaceConfiguration,
    renderer: render::Renderer,
    frames: u32,
    fps_t0: std::time::Instant,
}

impl Gpu {
    async fn new(window: Arc<Window>, debug: &debug::DebugState) -> Self {
        let size = window.inner_size();
        let instance = wgpu::Instance::default();
        let surface = instance.create_surface(window.clone()).expect("create surface");
        let adapter = instance
            .request_adapter(&wgpu::RequestAdapterOptions {
                power_preference: wgpu::PowerPreference::HighPerformance,
                compatible_surface: Some(&surface),
                force_fallback_adapter: false,
            })
            .await
            .expect("no suitable GPU adapter");
        log::info!("[gpu] {:?}", adapter.get_info().name);

        let (device, queue) = adapter
            .request_device(&wgpu::DeviceDescriptor::default(), None)
            .await
            .expect("request device");

        let caps = surface.get_capabilities(&adapter);
        // NON-sRGB surface — the renderer writes final color with no gamma encode.
        let format = caps
            .formats
            .iter()
            .copied()
            .find(|f| !f.is_srgb())
            .unwrap_or(caps.formats[0]);
        let config = wgpu::SurfaceConfiguration {
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
            format,
            width: size.width.max(1),
            height: size.height.max(1),
            present_mode: wgpu::PresentMode::AutoNoVsync, // low-latency present
            alpha_mode: caps.alpha_modes[0],
            view_formats: vec![],
            desired_maximum_frame_latency: 2,
        };
        surface.configure(&device, &config);
        let renderer = render::Renderer::new(&device, format);
        *debug.gpu_name.lock().unwrap() = adapter.get_info().name;

        Self {
            instance,
            adapter,
            surface,
            device,
            queue,
            config,
            renderer,
            frames: 0,
            fps_t0: std::time::Instant::now(),
        }
    }

    fn resize(&mut self, w: u32, h: u32) {
        if w > 0 && h > 0 {
            self.config.width = w;
            self.config.height = h;
            self.surface.configure(&self.device, &self.config);
        }
    }

    fn render(
        &mut self,
        shared: &Arc<Mutex<FrameDecoder>>,
        replica: &Arc<Mutex<replica::ReplicaState>>,
        debug: &debug::DebugState,
    ) {
        self.frames += 1;
        let el = self.fps_t0.elapsed().as_secs_f64();
        if el >= 0.5 {
            debug.set_fps(self.frames as f64 / el);
            self.frames = 0;
            self.fps_t0 = std::time::Instant::now();
        }

        let frame = match self.surface.get_current_texture() {
            Ok(f) => f,
            Err(_) => {
                self.surface.configure(&self.device, &self.config);
                return;
            }
        };
        let view = frame.texture.create_view(&wgpu::TextureViewDescriptor::default());
        {
            // Hold the decoder lock across parse+draw so we never render a frame
            // whose VRAM is mid-update (the flicker race).
            let fd = shared.lock().unwrap();
            debug
                .frame_num
                .store(fd.frame_num as u64, std::sync::atomic::Ordering::Relaxed);
            if fd.renderable {
                let parsed = ta::parse(fd.ta());
                let palette = texture::bake_palette(&fd.pvr_regs);
                let bodies = body_quads(replica, debug);
                self.renderer.render(
                    &self.device,
                    &self.queue,
                    &view,
                    &parsed,
                    &fd.vram,
                    &palette,
                    &fd.pvr_snapshot,
                    self.config.width,
                    self.config.height,
                    &bodies,
                    debug,
                );
            } else {
                self.renderer.clear_only(&self.device, &self.queue, &view);
            }
        }
        frame.present();
    }
}

/// A separate OS window hosting the egui debug panel (device/queue shared with Gpu).
struct DebugWin {
    window: Arc<Window>,
    surface: wgpu::Surface<'static>,
    config: wgpu::SurfaceConfiguration,
    egui_ctx: egui::Context,
    egui_state: egui_winit::State,
    egui_renderer: egui_wgpu::Renderer,
}

impl DebugWin {
    fn new(
        instance: &wgpu::Instance,
        adapter: &wgpu::Adapter,
        device: &wgpu::Device,
        window: Arc<Window>,
    ) -> Self {
        let size = window.inner_size();
        let surface = instance.create_surface(window.clone()).expect("debug surface");
        let caps = surface.get_capabilities(adapter);
        let format = caps
            .formats
            .iter()
            .copied()
            .find(|f| !f.is_srgb())
            .unwrap_or(caps.formats[0]);
        let config = wgpu::SurfaceConfiguration {
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
            format,
            width: size.width.max(1),
            height: size.height.max(1),
            present_mode: wgpu::PresentMode::AutoVsync,
            alpha_mode: caps.alpha_modes[0],
            view_formats: vec![],
            desired_maximum_frame_latency: 2,
        };
        surface.configure(device, &config);
        let egui_ctx = egui::Context::default();
        let egui_state = egui_winit::State::new(
            egui_ctx.clone(),
            egui::ViewportId::ROOT,
            &*window,
            Some(window.scale_factor() as f32),
            None,
        );
        let egui_renderer = egui_wgpu::Renderer::new(device, format, None, 1);
        Self { window, surface, config, egui_ctx, egui_state, egui_renderer }
    }

    fn resize(&mut self, device: &wgpu::Device, w: u32, h: u32) {
        if w > 0 && h > 0 {
            self.config.width = w;
            self.config.height = h;
            self.surface.configure(device, &self.config);
        }
    }

    fn render(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, debug: &debug::DebugState) {
        let frame = match self.surface.get_current_texture() {
            Ok(f) => f,
            Err(_) => {
                self.surface.configure(device, &self.config);
                return;
            }
        };
        let view = frame.texture.create_view(&wgpu::TextureViewDescriptor::default());
        let raw = self.egui_state.take_egui_input(&self.window);
        let out = self.egui_ctx.run(raw, |ctx| debug::ui(ctx, debug));
        self.egui_state.handle_platform_output(&self.window, out.platform_output);
        let ppp = out.pixels_per_point;
        let tris = self.egui_ctx.tessellate(out.shapes, ppp);
        let sd = egui_wgpu::ScreenDescriptor {
            size_in_pixels: [self.config.width, self.config.height],
            pixels_per_point: ppp,
        };
        for (id, delta) in &out.textures_delta.set {
            self.egui_renderer.update_texture(device, queue, *id, delta);
        }
        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("debug") });
        self.egui_renderer.update_buffers(device, queue, &mut enc, &tris, &sd);
        {
            let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("debug"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &view,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color { r: 0.06, g: 0.06, b: 0.07, a: 1.0 }),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            self.egui_renderer.render(&mut rp, &tris, &sd);
        }
        queue.submit(Some(enc.finish()));
        for id in &out.textures_delta.free {
            self.egui_renderer.free_texture(id);
        }
        frame.present();
    }
}

/// A reconstructed body quad + its decoded texture (None = force-color silhouette).
pub struct BodyItem {
    pub quad: ffi::SceneQuad,
    pub tex: Option<body_tex::BodyTex>,
}

/// Run render_frame over the /replica-live RAM image, and (unless force-color) decode
/// each emitted body quad's texture from the RAM while it's still locked.
fn body_quads(
    replica: &Arc<Mutex<replica::ReplicaState>>,
    debug: &debug::DebugState,
) -> Vec<BodyItem> {
    use std::sync::atomic::Ordering::Relaxed;
    if !debug.show_bodies.load(Relaxed) {
        return Vec::new();
    }
    let mut rep = replica.lock().unwrap();
    if !rep.seeded {
        return Vec::new();
    }
    let mut ctx = ffi::GstaSh4Ctx::zeroed();
    ctx.ram = rep.ram.as_mut_ptr();
    // SAFETY: ctx.ram is the locked 16MB RAM (valid for the call). render_frame fills
    // the static g_scene; the returned slice is valid until the next render_frame call.
    let (quads, srcdescs, effects) = unsafe {
        ffi::render_frame(&mut ctx);
        let n = ffi::render_frame_nscene().max(0) as usize;
        let s = ffi::render_frame_scene();
        if s.is_null() || n == 0 {
            return Vec::new();
        }
        static L: std::sync::Once = std::sync::Once::new();
        L.call_once(|| log::info!("[bodies] render_frame emitting {n} quads"));
        (
            std::slice::from_raw_parts(s, n).to_vec(),
            ffi::quad_srcdesc(n),
            ffi::quad_is_effect(n),
        )
    };
    let force_color = debug.bodies_force_color.load(Relaxed);
    quads
        .iter()
        .enumerate()
        .map(|(i, q)| BodyItem {
            quad: *q,
            tex: if force_color {
                None
            } else {
                body_tex::decode_body(q, srcdescs[i], effects[i], &rep.ram)
            },
        })
        .collect()
}

fn main() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or(
        "info,wgpu_core=warn,wgpu_hal=warn,naga=warn,gilrs=error",
    ))
    .init();

    log::info!("[ffi] render_frame linked (nscene at rest = {})", ffi::link_probe());

    // Shared debug/telemetry + render-option state (F1 overlay).
    let debug = Arc::new(debug::DebugState::new());

    // Shared decoded frame state (TA + VRAM + PVR), written by the net thread.
    let shared = Arc::new(Mutex::new(FrameDecoder::new()));

    // Native controller -> UDP:7100 (direct to nobd).
    input::spawn_input_thread(input::InputConfig::from_env(), debug.clone());

    // Thin ZCS2 wire -> shared FrameDecoder.
    net::spawn_net_thread(
        std::env::var("MAPLECAST_WS").unwrap_or_else(|_| "wss://nobd.net/ws".into()),
        shared.clone(),
        debug.clone(),
    );

    // Second socket: /replica-live seeds + maintains the 16MB SH4 RAM image that
    // render_frame walks to reconstruct the char-stripped fighter bodies.
    let replica_shared = Arc::new(Mutex::new(replica::ReplicaState::new()));
    replica::spawn_replica_thread(
        std::env::var("MAPLECAST_REPLICA").unwrap_or_else(|_| "wss://nobd.net/replica-live".into()),
        replica_shared.clone(),
        debug.clone(),
    );

    let event_loop = EventLoop::new().expect("event loop");
    let window = Arc::new(
        WindowBuilder::new()
            .with_title("MapleCast (native)")
            // 4:3 (MvC2 native aspect) at higher-than-native res. Maximize for max
            // quality — the 4:3 pillarbox viewport renders at the full surface size.
            .with_inner_size(winit::dpi::LogicalSize::new(1280.0, 960.0))
            .build(&event_loop)
            .expect("window"),
    );
    let mut gpu = pollster::block_on(Gpu::new(window.clone(), &debug));

    // Separate debug/telemetry window (F1 on the game window shows/hides it).
    let debug_window = Arc::new(
        WindowBuilder::new()
            .with_title("MapleCast · debug")
            .with_inner_size(winit::dpi::LogicalSize::new(360.0, 580.0))
            .build(&event_loop)
            .expect("debug window"),
    );
    let mut debug_win = DebugWin::new(&gpu.instance, &gpu.adapter, &gpu.device, debug_window.clone());
    let game_id = window.id();
    let debug_id = debug_window.id();

    event_loop
        .run(move |event, elwt| {
            use std::sync::atomic::Ordering::Relaxed;
            match &event {
                Event::WindowEvent { window_id, event: wev } if *window_id == game_id => match wev {
                    WindowEvent::CloseRequested => elwt.exit(),
                    WindowEvent::Resized(size) => gpu.resize(size.width, size.height),
                    WindowEvent::RedrawRequested => gpu.render(&shared, &replica_shared, &debug),
                    WindowEvent::KeyboardInput { event: key, .. } => {
                        if key.state == winit::event::ElementState::Pressed
                            && key.physical_key
                                == winit::keyboard::PhysicalKey::Code(winit::keyboard::KeyCode::F1)
                        {
                            let vis = !debug.overlay.load(Relaxed);
                            debug.overlay.store(vis, Relaxed);
                            debug_window.set_visible(vis);
                        }
                    }
                    _ => {}
                },
                Event::WindowEvent { window_id, event: wev } if *window_id == debug_id => {
                    let _ = debug_win.egui_state.on_window_event(&debug_window, wev);
                    match wev {
                        WindowEvent::CloseRequested => {
                            debug.overlay.store(false, Relaxed);
                            debug_window.set_visible(false);
                        }
                        WindowEvent::Resized(size) => debug_win.resize(&gpu.device, size.width, size.height),
                        WindowEvent::RedrawRequested => {
                            if debug.overlay.load(Relaxed) {
                                debug_win.render(&gpu.device, &gpu.queue, &debug);
                            }
                        }
                        _ => {}
                    }
                }
                Event::AboutToWait => {
                    window.request_redraw();
                    debug_window.request_redraw();
                }
                _ => {}
            }
        })
        .expect("event loop run");
}
