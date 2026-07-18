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
mod arrival;
mod frame;
mod gate;
mod input;
mod net;
mod quic;
mod render;
mod replica;
mod hud;
mod settings;
mod stage;
mod ta;
mod tdw;
mod texture;
mod zcs2;

use frame::FrameDecoder;

/// Local stage bake (MC_STAGE=path to .mcstg), loaded once at startup.
static STAGE_BAKE: std::sync::OnceLock<stage::Stage> = std::sync::OnceLock::new();

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
    decode_t: std::time::Instant,     // time of the last game-frame update (wire-jitter metric)
    win_wire_gap_max: f64,            // worst game-frame update interval this window (ms)
    // egui overlay on the GAME window — the fullscreen pillarbox bar HUD
    // (the tabbed panel is a separate window that fullscreen hides).
    window: Arc<Window>,
    egui_ctx: egui::Context,
    egui_state: egui_winit::State,
    egui_renderer: egui_wgpu::Renderer,
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
            // A2 (LATENCY-ANALYSIS / OPTIMIZATION-ROADMAP): one fewer buffered
            // present in the DXGI queue. Safe with the continuous-redraw loop
            // (a frame is always ready, no starvation); worth ~one present of
            // latency vs the default 2. Full A2 (waitable swapchain + wake)
            // is the follow-up.
            desired_maximum_frame_latency: 1,
        };
        surface.configure(&device, &config);
        let renderer = render::Renderer::new(&device, format);
        *debug.gpu_name.lock().unwrap() = adapter.get_info().name;

        // egui overlay for the bar HUD (same plumbing as DebugWin, on the game surface).
        let egui_ctx = egui::Context::default();
        let egui_state = egui_winit::State::new(
            egui_ctx.clone(),
            egui::ViewportId::ROOT,
            &*window,
            Some(window.scale_factor() as f32),
            None,
        );
        let egui_renderer = egui_wgpu::Renderer::new(&device, format, None, 1);

        Self {
            window,
            egui_ctx,
            egui_state,
            egui_renderer,
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
            decode_t: std::time::Instant::now(),
            win_wire_gap_max: 0.0,
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
            debug.set_wire_gap(self.win_wire_gap_max);
            self.win_wire_gap_max = 0.0;
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
            // motion-jitter metric: how long since the last game-frame update?
            // steady ~16.7ms = smooth; a spike = a late/bunched frame = the
            // visible micro-teleport. Ignore the first tick (huge startup gap).
            if self.last_epoch != (u64::MAX, u64::MAX) {
                let g = self.decode_t.elapsed().as_secs_f64() * 1000.0;
                if g > self.win_wire_gap_max {
                    self.win_wire_gap_max = g;
                }
            }
            self.decode_t = std::time::Instant::now();
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
                // MC_TDW=players: geometry comes from the players-only TDW wire
                // (fd.tdw_ta); pages/VRAM/palette still ride the ZCS2 chain.
                let mut parsed = ta::parse(fd.tdw_ta.as_deref().unwrap_or_else(|| fd.ta()));
                // LOCAL STAGE (endgame (a) pillar 2): re-project the bake through
                // the in-band TDW camera and draw it under the players. Only in
                // players mode (fd.tdw_ta set) — otherwise the wire has the stage.
                if fd.tdw_ta.is_some() {
                    if let (Some(st), Some(cam)) = (STAGE_BAKE.get(), fd.tdw_cam.as_ref()) {
                        let (ss, sc) = st.append(&mut parsed, cam);
                        debug.stage_strips.store(ss, Relaxed);
                        debug.stage_culled.store(sc, Relaxed);
                    }
                    // Keep-rule v4: the REAL HUD rides the wire (measured 2.7%
                    // churn — cheaper than a synthesis divergence surface). The
                    // byte-matched synthetic HUD (hud.rs) is retained for a
                    // future ultra-thin spectator tier: MC_HUD=synth re-enables.
                    if std::env::var("MC_HUD").ok().as_deref() == Some("synth") {
                        if let Some(g) = fd.gsta.as_deref() {
                            hud::append(&mut parsed, g);
                        }
                    }
                }
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

        // Bar HUD overlay: connection/server info in the pillarbox side bars.
        // Loads (does not clear) so it composites over the game render. Skipped
        // ENTIRELY (no egui run / no extra submit) unless there are real bars —
        // a plain 4:3 window pays nothing on the low-latency path.
        let has_bars = self.config.width as f32 / self.config.height as f32 > 4.0 / 3.0 + 0.02;
        if has_bars && debug.bars_hud.load(Relaxed) {
            let raw = self.egui_state.take_egui_input(&self.window);
            let out = self.egui_ctx.run(raw, |ctx| bars_ui(ctx, debug));
            let ppp = out.pixels_per_point;
            let tris = self.egui_ctx.tessellate(out.shapes, ppp);
            let sd = egui_wgpu::ScreenDescriptor {
                size_in_pixels: [self.config.width, self.config.height],
                pixels_per_point: ppp,
            };
            for (id, delta) in &out.textures_delta.set {
                self.egui_renderer.update_texture(&self.device, &self.queue, *id, delta);
            }
            let mut enc = self
                .device
                .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("bars-hud") });
            self.egui_renderer.update_buffers(&self.device, &self.queue, &mut enc, &tris, &sd);
            {
                let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("bars-hud"),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &view,
                        resolve_target: None,
                        ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store },
                    })],
                    depth_stencil_attachment: None,
                    timestamp_writes: None,
                    occlusion_query_set: None,
                });
                self.egui_renderer.render(&mut rp, &tris, &sd);
            }
            self.queue.submit(Some(enc.finish()));
            for id in &out.textures_delta.free {
                self.egui_renderer.free_texture(id);
            }
        }

        frame.present();
        debug.e2e_present(); // press->present: now - send_time[latched seq] (E2E probe)
    }
}

/// The fullscreen pillarbox bar HUD: connection + server + perf info drawn into
/// the black side bars of the GAME window (where the separate panel is hidden).
/// No-op when the bars are too narrow (a plain 4:3 window).
fn bars_ui(ctx: &egui::Context, d: &debug::DebugState) {
    use std::sync::atomic::Ordering::Relaxed;
    let sr = ctx.screen_rect();
    let (w, h) = (sr.width(), sr.height());
    let bar = (w - h * 4.0 / 3.0) * 0.5; // pillarbox bar width (logical pts)
    if bar < 96.0 {
        return; // no meaningful bar (windowed / near-4:3) — draw nothing
    }
    let white = egui::Color32::from_gray(235);
    let dim = egui::Color32::from_gray(150);
    let blue = egui::Color32::from_rgb(120, 200, 255);

    // active server name + ping
    let (name, rtt) = {
        let servers = d.servers.lock().unwrap();
        let i = d.active_server.load(Relaxed) as usize;
        let n = servers.get(i).map(|s| s.name.clone()).unwrap_or_default();
        (n, d.server_rtt_x100[i.min(7)].load(Relaxed))
    };
    let (bars, bcol) = if rtt == u64::MAX {
        ("\u{2717}", egui::Color32::from_rgb(255, 90, 90))
    } else {
        let ms = rtt as f64 / 100.0;
        if ms < 15.0 { ("\u{2582}\u{2584}\u{2586}\u{2588}", egui::Color32::from_rgb(80, 220, 120)) }
        else if ms < 35.0 { ("\u{2582}\u{2584}\u{2586}", egui::Color32::from_rgb(170, 220, 80)) }
        else if ms < 70.0 { ("\u{2582}\u{2584}", egui::Color32::from_rgb(255, 180, 60)) }
        else { ("\u{2582}", egui::Color32::from_rgb(255, 110, 90)) }
    };
    let e2e = d.e2e_ms();

    egui::Area::new(egui::Id::new("bar-hud-left"))
        .fixed_pos(egui::pos2(12.0, 16.0))
        .show(ctx, |ui| {
            ui.set_max_width(bar - 24.0);
            ui.label(egui::RichText::new(if name.is_empty() { "—" } else { &name }).color(white).size(15.0).strong());
            ui.horizontal(|ui| {
                ui.colored_label(bcol, egui::RichText::new(bars).monospace().size(15.0));
                let pingtxt = if rtt == u64::MAX { "down".into() } else { format!("{:.1} ms", rtt as f64 / 100.0) };
                ui.colored_label(blue, pingtxt);
            });
            if e2e > 0.0 {
                ui.colored_label(white, format!("press\u{2192}present  {e2e:.0} ms"));
            }
            let mig = d.migrate_status.lock().unwrap().clone();
            if !mig.is_empty() {
                ui.colored_label(dim, egui::RichText::new(mig).size(11.0));
            }
            ui.add_space(6.0);
            ui.colored_label(dim, egui::RichText::new(format!(
                "render {:.0} / wire {:.0} fps", d.fps(), d.wire_fps()
            )).size(12.0));
            // motion smoothness: worst gap between game-frame updates (16.7=perfect)
            let wg = d.wire_gap_max_ms();
            let wgcol = if wg > 33.0 { egui::Color32::from_rgb(255, 110, 90) }
                        else if wg > 22.0 { egui::Color32::from_rgb(255, 180, 60) }
                        else { dim };
            ui.colored_label(wgcol, egui::RichText::new(format!("motion gap {wg:.0} ms")).size(12.0));
            ui.colored_label(dim, egui::RichText::new(format!("{:.2} Mbps", d.mbps())).size(12.0));
            let dropped = d.dropped.load(Relaxed);
            if dropped > 0 {
                ui.colored_label(egui::Color32::from_rgb(255, 180, 60), format!("dropped {dropped}"));
            }
            ui.add_space(4.0);
            ui.colored_label(egui::Color32::from_gray(90),
                egui::RichText::new(format!("in \u{2192} {}", d.input_host.lock().unwrap())).size(10.0));
        });

    // hint in the RIGHT bar, bottom
    egui::Area::new(egui::Id::new("bar-hud-right"))
        .fixed_pos(egui::pos2(w - bar + 12.0, h - 34.0))
        .show(ctx, |ui| {
            ui.set_max_width(bar - 24.0);
            ui.colored_label(egui::Color32::from_gray(90),
                egui::RichText::new("Esc exit \u{00b7} F3 hide \u{00b7} F1/F2 panel").size(10.0));
        });
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

    fn render(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        ui_fn: impl FnOnce(&egui::Context),
    ) {
        let frame = match self.surface.get_current_texture() {
            Ok(f) => f,
            Err(_) => {
                self.surface.configure(device, &self.config);
                return;
            }
        };
        let view = frame.texture.create_view(&wgpu::TextureViewDescriptor::default());
        let raw = self.egui_state.take_egui_input(&self.window);
        let out = self.egui_ctx.run(raw, |ctx| ui_fn(ctx));
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

    // G0 deterministic loss gate (docs/TDW2-DESIGN.md): offline, no GPU/network.
    //   maplecast-native gate <capture> [--drop N] [--drop-at ..] [--recover R]
    {
        let args: Vec<String> = std::env::args().collect();
        if args.get(1).map(String::as_str) == Some("gate") {
            gate::run_gate(&args[2..]);
            return;
        }
    }

    log::info!("[ffi] render_frame linked (nscene at rest = {})", ffi::link_probe());

    // Local stage bake (endgame (a) pillar 2) — MC_STAGE=path\to\STGxx_ta.mcstg.
    if let Ok(p) = std::env::var("MC_STAGE") {
        match stage::Stage::load(&p) {
            Some(s) => { let _ = STAGE_BAKE.set(s); }
            None => log::warn!("[stage] failed to load bake: {p}"),
        }
    }

    // Shared debug/telemetry + render-option state (F1 overlay).
    let debug = Arc::new(debug::DebugState::new());

    // Server directory (settings panel: probe + connect). Entry 0 = the env-
    // configured default (the launcher's local rig); known nodes follow.
    // MC_SERVERS="name|ws_url|input_host;..." appends extras.
    {
        let mut servers = vec![
            debug::ServerEntry {
                name: "local rig".into(),
                ws: std::env::var("MAPLECAST_WS").unwrap_or_else(|_| "ws://127.0.0.1:7200".into()),
                input: "127.0.0.1".into(),
            },
            debug::ServerEntry {
                name: "nobd prod (NYC)".into(),
                ws: "ws://nobd.net:7200".into(),
                input: "nobd.net".into(),
            },
            debug::ServerEntry {
                name: "dev0ps".into(),
                ws: "ws://65.109.77.178:7200".into(),
                input: "65.109.77.178".into(),
            },
        ];
        if let Ok(extra) = std::env::var("MC_SERVERS") {
            for e in extra.split(';') {
                let p: Vec<&str> = e.split('|').collect();
                if p.len() == 3 {
                    servers.push(debug::ServerEntry {
                        name: p[0].trim().into(),
                        ws: p[1].trim().into(),
                        input: p[2].trim().into(),
                    });
                }
            }
        }
        servers.truncate(8);
        *debug.servers.lock().unwrap() = servers;
    }
    // Hub node-registry fetch: the LIVE server directory (the /network nodes
    // tab's data source). Merges registered nodes after the local entry;
    // refreshes every 60s. Direct player wire = ws://{public_host}:7200 (the
    // documented relay-port-minus-1 rule); input host = public_host.
    // MC_HUB overrides the registry URL; failures keep the static list.
    {
        let d = debug.clone();
        // non-registry statics survive refreshes (dev boxes, MC_SERVERS extras)
        let static_extras: Vec<debug::ServerEntry> = {
            let s = debug.servers.lock().unwrap();
            s.iter().skip(2).cloned().collect() // dev0ps + MC_SERVERS entries
        };
        std::thread::Builder::new()
            .name("maplecast-hub".into())
            .spawn(move || {
                let hub = std::env::var("MC_HUB")
                    .unwrap_or_else(|_| "https://nobd.net/hub/api/nodes".into());
                loop {
                    let fetched: Option<Vec<debug::ServerEntry>> = (|| {
                        let resp = ureq::get(&hub).timeout(std::time::Duration::from_secs(6)).call().ok()?;
                        let v: serde_json::Value = resp.into_json().ok()?;
                        let nodes = v.get("nodes")?.as_array()?;
                        Some(
                            nodes
                                .iter()
                                .filter_map(|n| {
                                    let host = n.get("public_host")?.as_str()?.to_string();
                                    let name = format!(
                                        "{} ({})",
                                        n.get("name")?.as_str()?,
                                        n.get("region").and_then(|r| r.as_str()).unwrap_or("?")
                                    );
                                    Some(debug::ServerEntry {
                                        name,
                                        ws: format!("ws://{host}:7200"),
                                        input: host,
                                    })
                                })
                                .collect(),
                        )
                    })();
                    if let Some(nodes) = fetched {
                        let mut servers = d.servers.lock().unwrap();
                        // entry 0 (local rig) + registry + static extras
                        servers.truncate(1);
                        for n in nodes.into_iter().chain(static_extras.iter().cloned()) {
                            if servers.len() >= 8 {
                                break;
                            }
                            servers.push(n);
                        }
                        log::info!("[hub] node registry: {} servers listed", servers.len());
                    }
                    std::thread::sleep(std::time::Duration::from_secs(60));
                }
            })
            .expect("spawn hub thread");
    }
    // RTT probe thread: the SAME 0xFF/0xFE UDP probe node-discovery uses
    // (input server answers on :7100) — per-server reachability + latency.
    {
        let d = debug.clone();
        std::thread::Builder::new()
            .name("maplecast-probe".into())
            .spawn(move || {
                use std::sync::atomic::Ordering::Relaxed;
                let mut seq: u8 = 1;
                let mut sweeps: u32 = 0;
                let mut auto_moves: u32 = 0;      // placement budget: ONE move, ever
                let mut dead_sweeps: u32 = 0;     // active unreachable streak (failover re-arm)
                let mut last_best: Option<usize> = None; // two-sweep confirmation
                loop {
                    let targets: Vec<(usize, String)> = {
                        let s = d.servers.lock().unwrap();
                        s.iter().enumerate().map(|(i, e)| (i, e.input.clone())).collect()
                    };
                    let n_targets = targets.len();
                    // CONCURRENT sweep (user ask 2026-07-16): one thread per
                    // server, joined — the whole fleet resolves in one 600ms
                    // timeout instead of up to 8×600ms sequentially. Distinct
                    // seq per target; each socket is connect()ed so replies
                    // can't cross-talk.
                    let handles: Vec<_> = targets
                        .into_iter()
                        .map(|(i, host)| {
                            let pseq = seq;
                            seq = seq.wrapping_add(1).max(1);
                            std::thread::spawn(move || {
                                let rtt = (|| -> Option<f64> {
                                    let sock = std::net::UdpSocket::bind("0.0.0.0:0").ok()?;
                                    // multi-instance entries carry "host:port"
                                    let addr = if host.contains(':') {
                                        host.clone()
                                    } else {
                                        format!("{}:7100", host)
                                    };
                                    sock.connect(addr).ok()?;
                                    sock.set_read_timeout(Some(std::time::Duration::from_millis(600))).ok()?;
                                    let pkt = [0xFFu8, pseq, 0, 0, 0, 0, 0, 0];
                                    let t0 = std::time::Instant::now();
                                    sock.send(&pkt).ok()?;
                                    let mut rx = [0u8; 16];
                                    loop {
                                        let n = sock.recv(&mut rx).ok()?;
                                        if n >= 2 && rx[0] == 0xFE && rx[1] == pseq {
                                            return Some(t0.elapsed().as_secs_f64() * 1000.0);
                                        }
                                    }
                                })();
                                (i, rtt)
                            })
                        })
                        .collect();
                    for h in handles {
                        if let Ok((i, rtt)) = h.join() {
                            if i < 8 {
                                d.server_rtt_x100[i].store(
                                    rtt.map(|ms| (ms * 100.0) as u64).unwrap_or(u64::MAX),
                                    Relaxed,
                                );
                            }
                        }
                    }
                    // AUTO-CLOSEST, third iteration (each bug cost a live session):
                    // one-shot latched a fleet-restart window (dfw); free re-eval
                    // FLAPPED between ~5ms peers on probe jitter (4 switches/30s,
                    // each a full reconnect = frozen screens). Now: two-sweep
                    // confirmation of the same candidate, a 10ms hysteresis bar,
                    // and a ONE-MOVE budget — place once, then stand down unless
                    // the active server is unreachable 3 sweeps straight
                    // (failover re-arm). Manual picks end auto for the session.
                    sweeps += 1;
                    // auto placement is the DEFAULT again (user decision
                    // 2026-07-16, now that the sweep is concurrent and the
                    // anti-flap rules hold); MC_NO_AUTO=1 opts out. The picker
                    // stays visible — a manual click always wins.
                    if n_targets > 0
                        && !d.manual_server.load(Relaxed)
                        && std::env::var("MC_NO_AUTO").is_err()
                    {
                        // Prefer REMOTE nodes: local rig entries are the dev/
                        // local-play mode, entered explicitly from the panel.
                        let loopback: Vec<bool> = {
                            let s = d.servers.lock().unwrap();
                            s.iter()
                                .map(|e| {
                                    e.input.starts_with("127.")
                                        || e.input.starts_with("localhost")
                                })
                                .collect()
                        };
                        let active = d.active_server.load(Relaxed) as usize;
                        let active_rtt = d.server_rtt_x100[active.min(7)].load(Relaxed);
                        let active_is_local = loopback.get(active).copied().unwrap_or(false);
                        if active_rtt == u64::MAX && !active_is_local {
                            dead_sweeps += 1;
                        } else {
                            dead_sweeps = 0;
                        }
                        // one placement move in the first ~30s; afterwards only a
                        // dead active (3 sweeps unreachable) re-arms a failover.
                        let allowed = (auto_moves == 0 && sweeps <= 15) || dead_sweeps >= 3;
                        let mut best: Option<(usize, u64)> = None;
                        for i in 0..n_targets.min(8) {
                            if loopback.get(i).copied().unwrap_or(false) {
                                continue;
                            }
                            let r = d.server_rtt_x100[i].load(Relaxed);
                            if r != u64::MAX && best.map_or(true, |(_, b)| r < b) {
                                best = Some((i, r));
                            }
                        }
                        // CAPACITY tie-break: within the 10ms band of the best,
                        // prefer big nodes (main, 4GB) over the 1GB edges — RTT
                        // twins are NOT capacity twins (ewr OOM'd a user's match
                        // load at 6.4ms while main sat at 6.6ms with 2.3GB free).
                        if let Some((_, br)) = best {
                            let names: Vec<String> = {
                                let s = d.servers.lock().unwrap();
                                s.iter().map(|e| e.name.clone()).collect()
                            };
                            for i in 0..n_targets.min(8) {
                                if loopback.get(i).copied().unwrap_or(false) {
                                    continue;
                                }
                                let r = d.server_rtt_x100[i].load(Relaxed);
                                if r != u64::MAX
                                    && r <= br + 1000
                                    && names.get(i).map_or(false, |n| n.contains("main") || n.contains("prod"))
                                {
                                    best = Some((i, r));
                                    break;
                                }
                            }
                        }
                        let mut confirmed: Option<(usize, u64)> = None;
                        if let Some((i, r)) = best {
                            let beats = active_is_local
                                || active_rtt == u64::MAX
                                || r + 1000 < active_rtt;   // 10ms hysteresis bar
                            if allowed
                                && i != active
                                && beats
                                && d.switch_server.load(Relaxed) == u64::MAX
                                && d.transfer_server.load(Relaxed) == u64::MAX
                            {
                                // two-sweep confirmation: the same candidate must
                                // win twice in a row (kills single-sample jitter)
                                if last_best == Some(i) {
                                    confirmed = Some((i, r));
                                }
                                last_best = Some(i);
                            } else {
                                last_best = None;
                            }
                        } else {
                            last_best = None;
                        }
                        if let Some((i, r)) = confirmed {
                            last_best = None;
                            auto_moves += 1;
                            dead_sweeps = 0;
                            let name = d
                                .servers
                                .lock()
                                .unwrap()
                                .get(i)
                                .map(|s| s.name.clone())
                                .unwrap_or_default();
                            log::info!(
                                "[probe] auto-closest (move {auto_moves}): {} @ {:.1}ms (active #{active} {})",
                                name,
                                r as f64 / 100.0,
                                if active_rtt == u64::MAX { "down".into() }
                                else { format!("{:.1}ms", active_rtt as f64 / 100.0) }
                            );
                            *d.migrate_status.lock().unwrap() =
                                format!("auto-connected to closest: {name} ({:.1}ms)", r as f64 / 100.0);
                            d.switch_server.store(i as u64, Relaxed);
                        }
                    }
                    std::thread::sleep(std::time::Duration::from_secs(2));
                }
            })
            .expect("spawn probe thread");
    }

    // Local-play manager: spawns/stops a headless server on THIS PC with the
    // panel-chosen ROM (F2 -> Local play). Local play is explicit; startup
    // auto-connect prefers the fleet.
    {
        let d = debug.clone();
        std::thread::Builder::new()
            .name("maplecast-localplay".into())
            .spawn(move || {
                use std::sync::atomic::Ordering::Relaxed;
                let mut child: Option<std::process::Child> = None;
                loop {
                    match d.local_play_cmd.swap(0, Relaxed) {
                        1 if child.is_none() => {
                            let rom = d.local_rom.lock().unwrap().clone();
                            let exe = std::env::var("MC_LOCAL_SERVER_EXE").unwrap_or_else(|_| {
                                r"C:\Users\trist\projects\maplecast-flycast\build-headless-win\flycast.exe".into()
                            });
                            let mut cmd = std::process::Command::new(&exe);
                            cmd.arg(&rom)
                                .env("MAPLECAST", "1")
                                .env("MAPLECAST_MIRROR_SERVER", "1")
                                .env("MAPLECAST_HEADLESS_AUTOLOAD", "1")
                                .env("MAPLECAST_TADICT", "1")
                                .env("MAPLECAST_TDW_ONLY", "1")
                                .env("MAPLECAST_TACANON", "2")
                                .env("MAPLECAST_TADICT_PLAYERS", "1")
                                .env("MAPLECAST_ZSTREAM_CAM", "1")
                                .env("MAPLECAST_E2E_PROBE", "1");
                            if let Ok(k) = std::env::var("MC_FLEET_KEY") {
                                cmd.env("MAPLECAST_FLEET_KEY", k);
                            }
                            if let Ok(f) = std::fs::File::create(
                                r"C:\Users\trist\projects\maplecast-flycast\_srv_localplay.log",
                            ) {
                                if let Ok(f2) = f.try_clone() {
                                    cmd.stdout(f).stderr(f2);
                                }
                            }
                            match cmd.spawn() {
                                Ok(c) => {
                                    log::info!("[localplay] server spawned pid={} rom={rom}", c.id());
                                    child = Some(c);
                                    d.local_srv_on.store(true, Relaxed);
                                    d.manual_server.store(true, Relaxed); // local play = manual: auto stands down
                                    *d.migrate_status.lock().unwrap() =
                                        "local server starting — connecting...".into();
                                    d.switch_server.store(0, Relaxed); // entry 0 = local rig
                                }
                                Err(e) => {
                                    *d.migrate_status.lock().unwrap() =
                                        format!("local server failed to start: {e}");
                                }
                            }
                        }
                        2 => {
                            if let Some(mut c) = child.take() {
                                let _ = c.kill();
                                d.local_srv_on.store(false, Relaxed);
                                *d.migrate_status.lock().unwrap() = "local server stopped".into();
                            }
                        }
                        _ => {}
                    }
                    // reap a server that exited on its own (bad ROM, port busy)
                    if let Some(c) = child.as_mut() {
                        if let Ok(Some(st)) = c.try_wait() {
                            d.local_srv_on.store(false, Relaxed);
                            *d.migrate_status.lock().unwrap() =
                                format!("local server exited ({st}) — see _srv_localplay.log");
                            child = None;
                        }
                    }
                    std::thread::sleep(std::time::Duration::from_millis(250));
                }
            })
            .expect("spawn localplay thread");
    }

    // Shared decoded frame state (TA + VRAM + PVR), written by the net thread.
    let shared = Arc::new(Mutex::new(FrameDecoder::new()));

    // Native controller -> UDP:7100 (direct to nobd).
    input::spawn_input_thread(input::InputConfig::from_env(), debug.clone());

    // Wire -> shared FrameDecoder. MC_QUIC routes TDW over the QUIC bridge
    // (datagrams, no TCP head-of-line) instead of the TCP WS; otherwise the
    // WS path (server directory entry 0 from MAPLECAST_WS) as before.
    if std::env::var("MC_QUIC").is_ok() {
        log::info!("[net] MC_QUIC set — TDW over QUIC bridge (TCP WS disabled)");
        quic::spawn_quic_thread(shared.clone(), debug.clone());
    } else {
        net::spawn_net_thread(shared.clone(), debug.clone());
    }

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
    // The /replica-live body feed belongs to the LEGACY render path — in TDW
    // players mode it would only spin a reconnect loop (gold-standard ledger
    // #9): don't spawn it at all.
    if std::env::var("MC_TDW").ok().as_deref() != Some("players") {
        replica::spawn_replica_thread(
            std::env::var("MAPLECAST_REPLICA").unwrap_or_else(|_| "wss://play.nobd.net/replica-live".into()),
            replica_shared.clone(),
            frame_queue.clone(),
            debug.clone(),
        );
    }

    // Persisted display settings: "w h fullscreen vsync" next to the exe.
    // Priority: MC_WINDOW env > remembered last size > default 640x480.
    let display_persist = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.join("maplecast-display.txt")));
    let persisted: Option<(u32, u32, bool, bool)> = display_persist
        .as_ref()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .and_then(|s| {
            let v: Vec<&str> = s.split_whitespace().collect();
            Some((
                v.first()?.parse().ok()?,
                v.get(1)?.parse().ok()?,
                v.get(2).map(|x| *x == "1").unwrap_or(false),
                v.get(3).map(|x| *x == "1").unwrap_or(false),
            ))
        });
    if let Some((_, _, fs, vs)) = persisted {
        debug.vsync_on.store(vs, std::sync::atomic::Ordering::Relaxed);
        if fs {
            debug
                .req_fullscreen
                .store(1, std::sync::atomic::Ordering::Relaxed);
        }
    }
    // A1 (LATENCY-ANALYSIS / OPTIMIZATION-ROADMAP): MC_FULLSCREEN=1 forces
    // borderless-fullscreen at launch so the surface reaches DXGI
    // independent-flip and BYPASSES the DWM compositor — the single biggest
    // removable chunk of button-to-photon (~12-16ms; windowed AutoNoVsync
    // cannot independent-flip). The F2 display toggle does the same live.
    // VERIFY WITH PresentMon: PresentMode must read HardwareIndependentFlip,
    // not Composed/ComposedFlip.
    if std::env::var("MC_FULLSCREEN").ok().as_deref() == Some("1") {
        debug
            .req_fullscreen
            .store(1, std::sync::atomic::Ordering::Relaxed);
    }
    let init_size = std::env::var("MC_WINDOW")
        .ok()
        .and_then(|s| {
            let (a, b) = s.split_once(['x', 'X'])?;
            Some((a.trim().parse().ok()?, b.trim().parse().ok()?))
        })
        .or(persisted.map(|(w, h, _, _)| (w, h)))
        .unwrap_or((640u32, 480u32));

    let event_loop = EventLoop::new().expect("event loop");
    let window = Arc::new(
        WindowBuilder::new()
            .with_title("MapleCast (native)")
            // 4:3 (MvC2 native aspect). PHYSICAL pixels so Windows display
            // scaling can't inflate it; locked size (F2 panel / MC_WINDOW to change).
            .with_inner_size(winit::dpi::PhysicalSize::new(
                init_size.0.max(320),
                init_size.1.max(240),
            ))
            .with_resizable(false)
            .build(&event_loop)
            .expect("window"),
    );
    let mut gpu = pollster::block_on(Gpu::new(window.clone(), &debug));

    // ONE tabbed side panel (debug / servers / display) — resizable, docked to
    // the right edge of the game window and following it. F1 opens it on the
    // Debug tab, F2 on Servers.
    let panel_window = Arc::new(
        WindowBuilder::new()
            .with_title("MapleCast · panel")
            .with_inner_size(winit::dpi::LogicalSize::new(400.0, 600.0))
            .with_resizable(true)
            .build(&event_loop)
            .expect("panel window"),
    );
    let mut panel_win = DebugWin::new(&gpu.instance, &gpu.adapter, &gpu.device, panel_window.clone());
    // dock helper: snap the panel to the game window's right edge
    let dock_panel = {
        let game = window.clone();
        let panel = panel_window.clone();
        move || {
            if let Ok(pos) = game.outer_position() {
                let size = game.outer_size();
                panel.set_outer_position(winit::dpi::PhysicalPosition::new(
                    pos.x + size.width as i32,
                    pos.y,
                ));
            }
        }
    };
    dock_panel();
    let mut vsync_applied = false;
    let mut cur_size = init_size;
    let game_id = window.id();
    let panel_id = panel_window.id();

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
                        dock_panel(); // keep the panel glued to the right edge
                    }
                    WindowEvent::Moved(_) => dock_panel(),
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
                        if key.state == winit::event::ElementState::Pressed {
                            match key.physical_key {
                                winit::keyboard::PhysicalKey::Code(winit::keyboard::KeyCode::F1) => {
                                    // F1: toggle the panel on the Debug tab (retoggle
                                    // hides only if already showing Debug).
                                    let showing = debug.overlay.load(Relaxed);
                                    let on_tab = debug.active_tab.load(Relaxed) == 0;
                                    let vis = !(showing && on_tab);
                                    debug.active_tab.store(0, Relaxed);
                                    debug.overlay.store(vis, Relaxed);
                                    panel_window.set_visible(vis);
                                    if vis {
                                        dock_panel();
                                    }
                                }
                                winit::keyboard::PhysicalKey::Code(winit::keyboard::KeyCode::F2) => {
                                    // F2: toggle the panel on the Servers tab.
                                    let showing = debug.overlay.load(Relaxed);
                                    let on_tab = debug.active_tab.load(Relaxed) == 1;
                                    let vis = !(showing && on_tab);
                                    debug.active_tab.store(1, Relaxed);
                                    debug.overlay.store(vis, Relaxed);
                                    panel_window.set_visible(vis);
                                    if vis {
                                        dock_panel();
                                    }
                                }
                                winit::keyboard::PhysicalKey::Code(winit::keyboard::KeyCode::Escape) => {
                                    // Escape leaves borderless-fullscreen — the only
                                    // way out (there's no window chrome to click).
                                    if debug.fullscreen_on.load(Relaxed) {
                                        debug.req_fullscreen.store(0, Relaxed);
                                    }
                                }
                                winit::keyboard::PhysicalKey::Code(winit::keyboard::KeyCode::F3) => {
                                    // F3 toggles the in-game pillarbox bar HUD.
                                    let v = !debug.bars_hud.load(Relaxed);
                                    debug.bars_hud.store(v, Relaxed);
                                }
                                _ => {}
                            }
                        }
                    }
                    _ => {}
                },
                Event::WindowEvent { window_id, event: wev } if *window_id == panel_id => {
                    let _ = panel_win.egui_state.on_window_event(&panel_window, wev);
                    match wev {
                        WindowEvent::CloseRequested => {
                            debug.overlay.store(false, Relaxed);
                            panel_window.set_visible(false);
                        }
                        WindowEvent::Resized(size) => panel_win.resize(&gpu.device, size.width, size.height),
                        WindowEvent::RedrawRequested => {
                            if debug.overlay.load(Relaxed) {
                                panel_win.render(&gpu.device, &gpu.queue, |ctx| {
                                    settings::panel(ctx, &debug)
                                });
                            }
                        }
                        _ => {}
                    }
                }
                Event::AboutToWait => {
                    // Apply display-settings requests (settings.rs writes, we act),
                    // persisting "w h fullscreen vsync" so the last setup is
                    // remembered across launches.
                    let mut save_display = false;
                    let rs = debug.req_scale.swap(0, Relaxed);
                    if (1..=3).contains(&rs) {
                        let (w, h) = (640 * rs as u32, 480 * rs as u32);
                        window.set_resizable(true);
                        let _ = window.request_inner_size(winit::dpi::PhysicalSize::new(w, h));
                        window.set_resizable(false);
                        cur_size = (w, h);
                        save_display = true;
                    }
                    match debug.req_fullscreen.swap(2, Relaxed) {
                        1 => {
                            window.set_fullscreen(Some(winit::window::Fullscreen::Borderless(None)));
                            debug.fullscreen_on.store(true, Relaxed);
                            save_display = true;
                        }
                        0 => {
                            window.set_fullscreen(None);
                            debug.fullscreen_on.store(false, Relaxed);
                            save_display = true;
                        }
                        _ => {}
                    }
                    let vs = debug.vsync_on.load(Relaxed);
                    if vs != vsync_applied {
                        vsync_applied = vs;
                        gpu.config.present_mode = if vs {
                            wgpu::PresentMode::AutoVsync
                        } else {
                            wgpu::PresentMode::AutoNoVsync
                        };
                        gpu.surface.configure(&gpu.device, &gpu.config);
                        save_display = true;
                    }
                    if save_display {
                        if let Some(p) = &display_persist {
                            let _ = std::fs::write(
                                p,
                                format!(
                                    "{} {} {} {}",
                                    cur_size.0,
                                    cur_size.1,
                                    debug.fullscreen_on.load(Relaxed) as u8,
                                    vsync_applied as u8
                                ),
                            );
                        }
                    }
                    // Present EVERY iteration — submit() is cheap and the heavy decode is gated
                    // to new server frames inside gpu.render(). Continuous presentation restores
                    // the smoothness the 60fps present-lock had lost (drops are counted at the
                    // decode gate now, not here).
                    window.request_redraw();
                    if debug.overlay.load(Relaxed) {
                        panel_window.request_redraw();
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
                            "press_present_ms={:.1}\ninput_rtt_ms={:.1}\nrender_fps={:.1}\nwire_fps={:.1}\nreplica_fps={:.1}\njitter_on={}\njitter_depth={}\ndropped={}\ngame_frame={}\nrender_ms_ema={:.2}\nframe_gap_max_ms={:.1}\nwire_gap_max_ms={:.1}\nbandwidth_mbps={:.2}\nstage_quads={}\nstage_tex={}\nstage_uploads={}\nbody_uploads={}\nrun_secs={:.0}\n",
                            debug.e2e_ms(), debug.rtt_ms(), debug.fps(), debug.wire_fps(),
                            debug.replica_fps(), debug.jitter_on.load(Relaxed),
                            debug.jitter_depth.load(Relaxed), debug.dropped.load(Relaxed),
                            debug.frame_num.load(Relaxed), debug.render_ms(),
                            debug.gap_max_ms(), debug.wire_gap_max_ms(), debug.mbps(),
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
