//! Shared debug/telemetry + render-option state, and the egui overlay (toggle: F1).
//!
//! Telemetry fields are written by the net/replica/render paths and read by the UI;
//! render-option fields are written by the UI and read by the render loop. All
//! lock-free atomics so no thread ever blocks on the panel.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering::Relaxed};
use std::sync::Mutex;

pub struct DebugState {
    // --- telemetry ---
    mbps_x100: AtomicU64,
    fps_x10: AtomicU64,
    // Frame-arrival counters (bumped by the net / replica threads); the render loop
    // watches these to render ONLY on a new frame instead of spinning, and they drive
    // the wire/replica fps readouts so we watch the SERVER cadence, not render-loop fps.
    pub wire_frame: AtomicU64,    // net.rs: fetch_add(1) per applied /ws (ZCS2) frame
    pub replica_frame: AtomicU64, // replica.rs: store(vframe) per /replica-live FRMx
    wire_fps_x10: AtomicU64,
    replica_fps_x10: AtomicU64,
    // Render/pacing telemetry — to see HITCHES (uneven frame delivery reads as lag even
    // when RTT is fine). All written by the render loop over a ~0.5s window.
    render_ms_x100: AtomicU64,  // ema of render() CPU time (build + upload + submit)
    render_max_x100: AtomicU64, // worst single render() in the window
    gap_max_x100: AtomicU64,    // worst gap between two presents in the window (the hitch)
    pub dropped: AtomicU64,     // server frames that arrived but were not drawn in time
    pub body_uploads: AtomicU64, // body-texture GPU uploads (cache MISSES) on the last frame
    pub stage_uploads: AtomicU64, // stage/effect-texture decodes+GPU uploads (cache MISSES) last frame
    pub stage_tex: AtomicU64,     // unique textured stage/effect textures resolved last frame
    pub stage_quads: AtomicU64,
    pub body_quads: AtomicU64,
    pub frame_num: AtomicU64,
    pub thin_active: AtomicBool,
    pub seeded: AtomicBool,
    pub regions: AtomicU64,
    pub gpu_name: Mutex<String>,
    // --- connection / latency ---
    pub server: Mutex<String>,
    pub input_host: Mutex<String>,
    rtt_x100: AtomicU64,
    pub rtt_seen: AtomicBool,
    // --- render options ---
    pub show_bodies: AtomicBool,
    pub show_stage: AtomicBool,
    pub bodies_force_color: AtomicBool,
    pub overlay: AtomicBool,
    // Jitter buffer: hold incoming frames and release them on a steady local clock to
    // smooth bursty network delivery (adds ~1 frame of video latency). Toggle to A/B.
    pub jitter_on: AtomicBool,
    pub jitter_depth: AtomicU64, // frames currently buffered (telemetry)
    // E2E press->present probe. The server stamps the E2EP tail with the client input seq it
    // latched into each frame (MAPLECAST_E2E_PROBE=1). We record each seq's send time in OUR
    // clock; on present we look up the echoed seq -> full press->present, zero clock sync.
    e2e_t0: std::time::Instant,
    e2e_send_us: Box<[AtomicU64; 256]>, // send_us[seq & 0xFF] = us since t0 at packet send (0 = none)
    e2e_echo_seq: AtomicU64,            // latest E2EP-echoed client seq for our slot (u64::MAX = none)
    e2e_ms_x100: AtomicU64,             // computed press->present ema
    e2e_slot: AtomicU64,                // our input slot (which of the two E2EP seqs to read)
    e2e_last_seq: AtomicU64,            // last seq we measured — only measure FRESH inputs
}

impl DebugState {
    pub fn new() -> Self {
        Self {
            mbps_x100: AtomicU64::new(0),
            fps_x10: AtomicU64::new(0),
            wire_frame: AtomicU64::new(0),
            replica_frame: AtomicU64::new(0),
            wire_fps_x10: AtomicU64::new(0),
            replica_fps_x10: AtomicU64::new(0),
            render_ms_x100: AtomicU64::new(0),
            render_max_x100: AtomicU64::new(0),
            gap_max_x100: AtomicU64::new(0),
            dropped: AtomicU64::new(0),
            body_uploads: AtomicU64::new(0),
            stage_uploads: AtomicU64::new(0),
            stage_tex: AtomicU64::new(0),
            stage_quads: AtomicU64::new(0),
            body_quads: AtomicU64::new(0),
            frame_num: AtomicU64::new(0),
            thin_active: AtomicBool::new(false),
            seeded: AtomicBool::new(false),
            regions: AtomicU64::new(0),
            gpu_name: Mutex::new(String::new()),
            server: Mutex::new(String::new()),
            input_host: Mutex::new(String::new()),
            rtt_x100: AtomicU64::new(0),
            rtt_seen: AtomicBool::new(false),
            show_bodies: AtomicBool::new(true),
            show_stage: AtomicBool::new(true),
            bodies_force_color: AtomicBool::new(false),
            overlay: AtomicBool::new(true),
            jitter_on: AtomicBool::new(false),
            jitter_depth: AtomicU64::new(0),
            e2e_t0: std::time::Instant::now(),
            e2e_send_us: Box::new(std::array::from_fn(|_| AtomicU64::new(0))),
            e2e_echo_seq: AtomicU64::new(u64::MAX),
            e2e_ms_x100: AtomicU64::new(0),
            e2e_slot: AtomicU64::new(0),
            e2e_last_seq: AtomicU64::new(u64::MAX),
        }
    }

    pub fn set_mbps(&self, v: f64) {
        self.mbps_x100.store((v * 100.0) as u64, Relaxed);
    }
    pub fn set_fps(&self, v: f64) {
        self.fps_x10.store((v * 10.0) as u64, Relaxed);
    }
    /// Frames/sec actually arriving on each socket (computed from the counters over a
    /// window). ~60 each is healthy; render fps below wire fps means we drop frames.
    pub fn set_wire_fps(&self, v: f64) {
        self.wire_fps_x10.store((v * 10.0) as u64, Relaxed);
    }
    pub fn set_replica_fps(&self, v: f64) {
        self.replica_fps_x10.store((v * 10.0) as u64, Relaxed);
    }
    /// Per-window render timing: ema render time, worst render, worst present gap (ms).
    pub fn set_render_stats(&self, ema_ms: f64, max_ms: f64, gap_max_ms: f64) {
        self.render_ms_x100.store((ema_ms * 100.0) as u64, Relaxed);
        self.render_max_x100.store((max_ms * 100.0) as u64, Relaxed);
        self.gap_max_x100.store((gap_max_ms * 100.0) as u64, Relaxed);
    }

    // --- E2E press->present probe ---
    pub fn set_input_slot(&self, slot: u8) {
        self.e2e_slot.store(slot as u64, Relaxed);
    }
    /// Record the send time of input packet `seq` (our clock).
    pub fn input_sent(&self, seq: u32) {
        let us = self.e2e_t0.elapsed().as_micros() as u64;
        self.e2e_send_us[(seq & 0xFF) as usize].store(us.max(1), Relaxed); // 0 = "none"
    }
    /// Server echoed the seq it latched into this frame (per slot) — keep ours.
    pub fn e2e_echo(&self, seq0: u32, seq1: u32) {
        let s = if self.e2e_slot.load(Relaxed) == 1 { seq1 } else { seq0 };
        self.e2e_echo_seq.store(s as u64, Relaxed);
    }
    /// On present: press->present = now - send_time[echoed seq], EMA'd. Zero clock sync.
    pub fn e2e_present(&self) {
        let s = self.e2e_echo_seq.load(Relaxed);
        if s == u64::MAX {
            return;
        }
        // Measure only when a NEW input's frame is presented — otherwise the value inflates as
        // we re-present the same latest input between fresh packets (236fps present vs 60fps wire).
        if s == self.e2e_last_seq.swap(s, Relaxed) {
            return;
        }
        let sent = self.e2e_send_us[(s as usize) & 0xFF].load(Relaxed);
        if sent == 0 {
            return;
        }
        let now = self.e2e_t0.elapsed().as_micros() as u64;
        if now < sent {
            return;
        }
        let e2e = (now - sent) as f64 / 1000.0;
        if e2e > 1000.0 {
            return; // seq& 0xFF wrap collided with a stale send — ignore
        }
        let prev = self.e2e_ms_x100.load(Relaxed) as f64 / 100.0;
        let ema = if prev == 0.0 { e2e } else { prev * 0.9 + e2e * 0.1 };
        self.e2e_ms_x100.store((ema * 100.0) as u64, Relaxed);
    }
    pub fn e2e_ms(&self) -> f64 {
        self.e2e_ms_x100.load(Relaxed) as f64 / 100.0
    }

    // --- getters for the live status file (external monitoring) ---
    pub fn rtt_ms(&self) -> f64 {
        self.rtt_x100.load(Relaxed) as f64 / 100.0
    }
    pub fn fps(&self) -> f64 {
        self.fps_x10.load(Relaxed) as f64 / 10.0
    }
    pub fn wire_fps(&self) -> f64 {
        self.wire_fps_x10.load(Relaxed) as f64 / 10.0
    }
    pub fn replica_fps(&self) -> f64 {
        self.replica_fps_x10.load(Relaxed) as f64 / 10.0
    }
    pub fn render_ms(&self) -> f64 {
        self.render_ms_x100.load(Relaxed) as f64 / 100.0
    }
    pub fn gap_max_ms(&self) -> f64 {
        self.gap_max_x100.load(Relaxed) as f64 / 100.0
    }
    pub fn mbps(&self) -> f64 {
        self.mbps_x100.load(Relaxed) as f64 / 100.0
    }
    /// EMA of the input round-trip time (native UDP -> :7100 -> ACK), in ms.
    pub fn set_rtt(&self, ms: f64) {
        self.rtt_x100.store((ms * 100.0) as u64, Relaxed);
        self.rtt_seen.store(true, Relaxed);
    }
}

/// Build the debug panel. Reads telemetry, writes back option toggles.
pub fn ui(ctx: &egui::Context, d: &DebugState) {
    egui::CentralPanel::default().show(ctx, |ui| {
            ui.heading("MapleCast · debug");
            ui.label(format!("GPU: {}", d.gpu_name.lock().unwrap()));
            ui.separator();

            egui::Grid::new("telemetry").num_columns(2).show(ui, |ui| {
                ui.label("render fps");
                ui.label(format!("{:.1}", d.fps_x10.load(Relaxed) as f64 / 10.0));
                ui.end_row();
                ui.label("wire fps");
                ui.label(format!("{:.1}  (server ~60)", d.wire_fps_x10.load(Relaxed) as f64 / 10.0));
                ui.end_row();
                ui.label("replica fps");
                ui.label(format!("{:.1}  (server ~60)", d.replica_fps_x10.load(Relaxed) as f64 / 10.0));
                ui.end_row();
                ui.label("bandwidth");
                ui.label(format!("{:.2} Mbps", d.mbps_x100.load(Relaxed) as f64 / 100.0));
                ui.end_row();
                ui.label("stage quads");
                ui.label(format!("{}", d.stage_quads.load(Relaxed)));
                ui.end_row();
                ui.label("body quads");
                ui.label(format!("{}", d.body_quads.load(Relaxed)));
                ui.end_row();
                ui.label("body uploads");
                ui.label(format!("{}  (cache miss/frame)", d.body_uploads.load(Relaxed)));
                ui.end_row();
                ui.label("game frame");
                ui.label(format!("{}", d.frame_num.load(Relaxed)));
                ui.end_row();
                ui.label("render ms");
                ui.label(format!(
                    "{:.2}  (max {:.1})",
                    d.render_ms_x100.load(Relaxed) as f64 / 100.0,
                    d.render_max_x100.load(Relaxed) as f64 / 100.0
                ));
                ui.end_row();
                ui.label("frame gap max");
                ui.label(format!("{:.1} ms  (16.7 = 60fps)", d.gap_max_x100.load(Relaxed) as f64 / 100.0));
                ui.end_row();
                ui.label("dropped");
                ui.label(format!("{}", d.dropped.load(Relaxed)));
                ui.end_row();
                ui.label("buffer depth");
                ui.label(format!("{} frames", d.jitter_depth.load(Relaxed)));
                ui.end_row();
            });

            ui.separator();
            ui.strong("Connection");
            ui.label(format!("stream: {}", d.server.lock().unwrap()));
            ui.label(format!("input:  {}:7100", d.input_host.lock().unwrap()));
            if d.rtt_seen.load(Relaxed) {
                ui.label(format!(
                    "input RTT (net): {:.1} ms",
                    d.rtt_x100.load(Relaxed) as f64 / 100.0
                ));
            } else {
                ui.label("input RTT: — (press a button)");
            }
            let e2e = d.e2e_ms();
            if e2e > 0.0 {
                ui.colored_label(
                    egui::Color32::from_rgb(120, 200, 255),
                    format!("press → present: {:.1} ms", e2e),
                );
            } else {
                ui.label("press → present: — (server E2EP + input)");
            }
            status_line(ui, "thin ZCS2 wire", d.thin_active.load(Relaxed));
            status_line(
                ui,
                &format!("replica RAM ({} regions)", d.regions.load(Relaxed)),
                d.seeded.load(Relaxed),
            );

            ui.separator();
            ui.strong("Render");
            checkbox(ui, &d.show_stage, "stage / effects / HUD");
            checkbox(ui, &d.show_bodies, "fighter bodies");
            checkbox(ui, &d.bodies_force_color, "bodies force-color (silhouette)");
            checkbox(ui, &d.jitter_on, "jitter buffer — smooth motion (+~1 frame)");

            ui.separator();
            ui.weak("F1 (on the game window) shows/hides this window");
    });
}

fn status_line(ui: &mut egui::Ui, label: &str, ok: bool) {
    ui.horizontal(|ui| {
        let (dot, col) = if ok {
            ("●", egui::Color32::from_rgb(80, 220, 120))
        } else {
            ("○", egui::Color32::from_rgb(180, 180, 180))
        };
        ui.colored_label(col, dot);
        ui.label(label);
    });
}

fn checkbox(ui: &mut egui::Ui, a: &AtomicBool, label: &str) {
    let mut v = a.load(Relaxed);
    if ui.checkbox(&mut v, label).changed() {
        a.store(v, Relaxed);
    }
}
