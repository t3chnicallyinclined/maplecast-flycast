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

mod frame;
mod input;
mod net;
mod render;
mod ta;
mod texture;
mod zcs2;

use frame::FrameDecoder;

struct Gpu {
    surface: wgpu::Surface<'static>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    config: wgpu::SurfaceConfiguration,
    renderer: render::Renderer,
}

impl Gpu {
    async fn new(window: Arc<Window>) -> Self {
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
        Self { surface, device, queue, config, renderer }
    }

    fn resize(&mut self, w: u32, h: u32) {
        if w > 0 && h > 0 {
            self.config.width = w;
            self.config.height = h;
            self.surface.configure(&self.device, &self.config);
        }
    }

    fn render(&mut self, shared: &Arc<Mutex<FrameDecoder>>) {
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
            if fd.renderable {
                let parsed = ta::parse(fd.ta());
                let palette = texture::bake_palette(&fd.pvr_regs);
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
                );
            } else {
                self.renderer.clear_only(&self.device, &self.queue, &view);
            }
        }
        frame.present();
    }
}

fn main() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or(
        "info,wgpu_core=warn,wgpu_hal=warn,naga=warn,gilrs=error",
    ))
    .init();

    // Shared decoded frame state (TA + VRAM + PVR), written by the net thread.
    let shared = Arc::new(Mutex::new(FrameDecoder::new()));

    // Native controller -> UDP:7100 (direct to nobd).
    input::spawn_input_thread(input::InputConfig::from_env());

    // Thin ZCS2 wire -> shared FrameDecoder.
    net::spawn_net_thread(
        std::env::var("MAPLECAST_WS").unwrap_or_else(|_| "wss://nobd.net/ws".into()),
        shared.clone(),
    );

    let event_loop = EventLoop::new().expect("event loop");
    let window = Arc::new(
        WindowBuilder::new()
            .with_title("MapleCast (native)")
            .with_inner_size(winit::dpi::LogicalSize::new(1280.0, 720.0))
            .build(&event_loop)
            .expect("window"),
    );
    let mut gpu = pollster::block_on(Gpu::new(window.clone()));

    event_loop
        .run(move |event, elwt| match event {
            Event::WindowEvent { event, .. } => match event {
                WindowEvent::CloseRequested => elwt.exit(),
                WindowEvent::Resized(size) => gpu.resize(size.width, size.height),
                WindowEvent::RedrawRequested => gpu.render(&shared),
                _ => {}
            },
            Event::AboutToWait => window.request_redraw(),
            _ => {}
        })
        .expect("event loop run");
}
