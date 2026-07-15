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
    last_epoch: (u64, u64), // last DECODED (wire_frame, replica_frame) — decode gate + drops
    next_release: std::time::Instant, // jitter buffer: when to release the next buffered frame
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
            last_epoch: (u64::MAX, u64::MAX),
            next_release: std::time::Instant::now(),
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
        queue: &Arc<Mutex<std::collections::VecDeque<Vec<u8>>>>,
        debug: &debug::DebugState,
    ) {
        use std::sync::atomic::Ordering::Relaxed;
        self.frames += 1;
        let el = self.fps_t0.elapsed().as_secs_f64();
        if el >= 0.5 {
            debug.set_fps(self.frames as f64 / el);
            self.frames = 0;
            self.fps_t0 = std::time::Instant::now();
        }

        // JITTER BUFFER: the replica thread queues every incoming frame; we are the single
        // consumer. ON -> release ONE frame per ~16.67ms local tick (a PLL keeps ~1 frame
        // buffered) to smooth bursty network delivery. OFF -> drain immediately (no added
        // latency). Applying a frame bumps replica_frame, which the decode gate reacts to.
        {
            let now = std::time::Instant::now();
            let depth = queue.lock().unwrap().len();
            debug.jitter_depth.store(depth as u64, Relaxed);
            if !debug.jitter_on.load(Relaxed) {
                if depth > 0 {
                    let drained: Vec<Vec<u8>> = queue.lock().unwrap().drain(..).collect();
                    let mut rep = replica.lock().unwrap();
                    for f in &drained {
                        if rep.seeded {
                            rep.apply_frame(f);
                        }
                    }
                    if rep.seeded {
                        debug.replica_frame.store(rep.vframe as u64, Relaxed);
                    }
                }
                self.next_release = now;
            } else if now >= self.next_release && depth > 0 {
                const TARGET: i64 = 1;
                const MAX: usize = 4;
                let frame = {
                    let mut q = queue.lock().unwrap();
                    while q.len() > MAX {
                        q.pop_front(); // drop stale frames to bound latency (catch up)
                    }
                    q.pop_front()
                };
                if let Some(f) = frame {
                    let mut rep = replica.lock().unwrap();
                    if rep.seeded {
                        rep.apply_frame(&f);
                        debug.replica_frame.store(rep.vframe as u64, Relaxed);
                    }
                }
                // PLL: release faster when the buffer builds, slower when it drains — locks the
                // release rate to the average arrival while holding ~TARGET frames buffered.
                let err = depth as i64 - TARGET;
                let interval = (16_667 - err * 800).clamp(13_000, 20_000);
                self.next_release = now + std::time::Duration::from_micros(interval as u64);
            }
        }

        // DECODE only when the server frame changed (heavy: TA parse + render_frame + body
        // decode); PRESENT every call (cheap: re-draw the cached frame). Continuous present
        // keeps motion smooth while the expensive work runs only at the server's 60fps.
        let ep = (debug.wire_frame.load(Relaxed), debug.replica_frame.load(Relaxed));
        if ep != self.last_epoch {
            // server frames skipped since the last decode = we couldn't keep up (a drop).
            let prev_rf = self.last_epoch.1;
            if prev_rf != u64::MAX && ep.1 > prev_rf.wrapping_add(1) {
                debug.dropped.fetch_add(ep.1 - prev_rf - 1, Relaxed);
            }
            self.last_epoch = ep;
            // Hold the decoder lock across parse+decode so we never read VRAM mid-update.
            let fd = shared.lock().unwrap();
            debug.frame_num.store(fd.frame_num as u64, Relaxed);
            if fd.renderable {
                let parsed = ta::parse(fd.ta());
                let palette = texture::bake_palette(&fd.pvr_regs);
                // Stage textures use the /ws palette; body palette is phase-locked to
                // /replica-live inside body_quads (assist-flash fix).
                let bodies = body_quads(replica, debug);
                self.renderer.rebuild(
                    &self.device,
                    &self.queue,
                    &parsed,
                    &fd.vram,
                    &palette,
                    &fd.pvr_snapshot,
                    &bodies,
                    debug,
                );
            } else {
                self.renderer.set_empty();
            }
        }

        let frame = match self.surface.get_current_texture() {
            Ok(f) => f,
            Err(_) => {
                self.surface.configure(&self.device, &self.config);
                return;
            }
        };
        let view = frame.texture.create_view(&wgpu::TextureViewDescriptor::default());
        self.renderer
            .submit(&self.device, &self.queue, &view, self.config.width, self.config.height);
        frame.present();
        debug.e2e_present(); // press->present: now - send_time[latched seq] (E2E probe)
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
    let (quads, srcdescs, effects, colrows) = unsafe {
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
            ffi::quad_colrow(n),
        )
    };
    let force_color = debug.bodies_force_color.load(Relaxed);
    // Phase-lock the body palette to the SAME /replica-live snapshot render_frame just
    // walked (rep.pvr_regs — updated by the PALETTE tail in the same FRMx that splats the
    // char regions), NOT the async /ws socket (fd.pvr_regs). Kills the 1-frame default-skin
    // flash on assist-spawn (the cross-socket lag). Verified vs the web ground truth.
    let pvr_pal = texture::bake_palette(&rep.pvr_regs);

    quads
        .iter()
        .enumerate()
        .filter(|(i, _)| !effects[*i]) // bit15 effect quads carry no body art -> skip (no magenta)
        .map(|(i, q)| BodyItem {
            quad: *q,
            tex: if force_color {
                None
            } else {
                body_tex::decode_body(q, srcdescs[i], effects[i], &rep.ram, &pvr_pal, colrows[i])
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
        std::env::var("MAPLECAST_WS").unwrap_or_else(|_| "wss://play.nobd.net/ws".into()),
        shared.clone(),
        debug.clone(),
    );

    // Second socket: /replica-live seeds + maintains the 16MB SH4 RAM image that
    // render_frame walks to reconstruct the char-stripped fighter bodies.
    let replica_shared = Arc::new(Mutex::new(replica::ReplicaState::new()));
    // Incoming /replica-live frames land here; the render loop is the SINGLE consumer
    // (jitter buffer: paced release when on, immediate drain when off).
    let frame_queue = Arc::new(Mutex::new(std::collections::VecDeque::<Vec<u8>>::new()));
    // MAPLECAST_JITTER=1 starts with the jitter buffer on (the debug checkbox toggles it live).
    if std::env::var("MAPLECAST_JITTER").ok().as_deref() == Some("1") {
        debug.jitter_on.store(true, std::sync::atomic::Ordering::Relaxed);
    }
    replica::spawn_replica_thread(
        std::env::var("MAPLECAST_REPLICA").unwrap_or_else(|_| "wss://play.nobd.net/replica-live".into()),
        replica_shared.clone(),
        frame_queue.clone(),
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

    // Telemetry state (see AboutToWait): wire/replica fps + render-timing over a window.
    let mut fps_t = std::time::Instant::now();
    let mut fps_wire0: u64 = 0;
    let mut fps_rep0: u64 = 0;
    let mut last_present: Option<std::time::Instant> = None;
    let mut rt_ema: f64 = 0.0; // ema render() ms
    let mut win_rt_max: f64 = 0.0; // worst render() ms this window
    let mut win_gap_max: f64 = 0.0; // worst present gap this window (the hitch)
    // Per-frame CSV log (set MAPLECAST_TELEMETRY_LOG=path) — for offline hitch analysis.
    let mut telem = std::env::var("MAPLECAST_TELEMETRY_LOG")
        .ok()
        .and_then(|p| std::fs::File::create(p).ok());
    if let Some(f) = telem.as_mut() {
        use std::io::Write;
        let _ = writeln!(f, "t_ms,wf,rf,render_ms,gap_ms,body_quads,body_uploads,dropped,buf,e2e_ms,rtt_ms");
    }
    let telem_t0 = std::time::Instant::now();
    // Live status file (ALWAYS on): the full current stat snapshot, overwritten ~2x/sec, next
    // to the exe so external tooling can read the live numbers without the debug window.
    let status_path = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.join("maplecast-status.txt")))
        .unwrap_or_else(|| std::env::temp_dir().join("maplecast-status.txt"));
    log::info!("[status] live stats file -> {}", status_path.display());

    event_loop
        .run(move |event, elwt| {
            use std::sync::atomic::Ordering::Relaxed;
            match &event {
                Event::WindowEvent { window_id, event: wev } if *window_id == game_id => match wev {
                    WindowEvent::CloseRequested => elwt.exit(),
                    WindowEvent::Resized(size) => {
                        gpu.resize(size.width, size.height);
                        window.request_redraw(); // re-render at the new size despite the frame gate
                    }
                    WindowEvent::RedrawRequested => {
                        let t0 = std::time::Instant::now();
                        let gap = last_present
                            .map(|p| t0.duration_since(p).as_secs_f64() * 1000.0)
                            .unwrap_or(0.0);
                        if gap > win_gap_max {
                            win_gap_max = gap;
                        }
                        last_present = Some(t0);
                        gpu.render(&shared, &replica_shared, &frame_queue, &debug);
                        let rt = t0.elapsed().as_secs_f64() * 1000.0;
                        rt_ema = if rt_ema == 0.0 { rt } else { rt_ema * 0.9 + rt * 0.1 };
                        if rt > win_rt_max {
                            win_rt_max = rt;
                        }
                        if let Some(f) = telem.as_mut() {
                            use std::io::Write;
                            let _ = writeln!(
                                f,
                                "{:.1},{},{},{:.3},{:.3},{},{},{},{},{:.1},{:.1}",
                                telem_t0.elapsed().as_secs_f64() * 1000.0,
                                debug.wire_frame.load(Relaxed),
                                debug.replica_frame.load(Relaxed),
                                rt,
                                gap,
                                debug.body_quads.load(Relaxed),
                                debug.body_uploads.load(Relaxed),
                                debug.dropped.load(Relaxed),
                                debug.jitter_depth.load(Relaxed),
                                debug.e2e_ms(),
                                debug.rtt_ms(),
                            );
                        }
                    }
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
                    // Present EVERY iteration — submit() is cheap and the heavy decode is gated
                    // to new server frames inside gpu.render(). Continuous presentation restores
                    // the smoothness the 60fps present-lock had lost (drops are counted at the
                    // decode gate now, not here).
                    window.request_redraw();
                    if debug.overlay.load(Relaxed) {
                        debug_window.request_redraw();
                    }
                    // fps + render-timing telemetry over a ~0.5s window.
                    let wf = debug.wire_frame.load(Relaxed);
                    let rf = debug.replica_frame.load(Relaxed);
                    let el = fps_t.elapsed().as_secs_f64();
                    if el >= 0.5 {
                        debug.set_wire_fps(wf.wrapping_sub(fps_wire0) as f64 / el);
                        debug.set_replica_fps(rf.wrapping_sub(fps_rep0) as f64 / el);
                        debug.set_render_stats(rt_ema, win_rt_max, win_gap_max);
                        // Live status snapshot (key=value; overwritten each window) — read this
                        // file to see the client's live numbers from outside.
                        let status = format!(
                            "press_present_ms={:.1}\ninput_rtt_ms={:.1}\nrender_fps={:.1}\nwire_fps={:.1}\nreplica_fps={:.1}\njitter_on={}\njitter_depth={}\ndropped={}\ngame_frame={}\nrender_ms_ema={:.2}\nframe_gap_max_ms={:.1}\nbandwidth_mbps={:.2}\nstage_quads={}\nstage_tex={}\nstage_uploads={}\nbody_uploads={}\nrun_secs={:.0}\n",
                            debug.e2e_ms(), debug.rtt_ms(), debug.fps(), debug.wire_fps(),
                            debug.replica_fps(), debug.jitter_on.load(Relaxed),
                            debug.jitter_depth.load(Relaxed), debug.dropped.load(Relaxed),
                            debug.frame_num.load(Relaxed), debug.render_ms(),
                            debug.gap_max_ms(), debug.mbps(),
                            debug.stage_quads.load(Relaxed), debug.stage_tex.load(Relaxed),
                            debug.stage_uploads.load(Relaxed),
                            debug.body_uploads.load(Relaxed), telem_t0.elapsed().as_secs_f64(),
                        );
                        let _ = std::fs::write(&status_path, status);
                        win_rt_max = 0.0;
                        win_gap_max = 0.0;
                        fps_wire0 = wf;
                        fps_rep0 = rf;
                        fps_t = std::time::Instant::now();
                    }
                }
                _ => {}
            }
        })
        .expect("event loop run");
}
