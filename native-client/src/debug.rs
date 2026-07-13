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
    pub stage_quads: AtomicU64,
    pub body_quads: AtomicU64,
    pub frame_num: AtomicU64,
    pub thin_active: AtomicBool,
    pub seeded: AtomicBool,
    pub regions: AtomicU64,
    pub gpu_name: Mutex<String>,
    // --- render options ---
    pub show_bodies: AtomicBool,
    pub show_stage: AtomicBool,
    pub bodies_force_color: AtomicBool,
    pub overlay: AtomicBool,
}

impl DebugState {
    pub fn new() -> Self {
        Self {
            mbps_x100: AtomicU64::new(0),
            fps_x10: AtomicU64::new(0),
            stage_quads: AtomicU64::new(0),
            body_quads: AtomicU64::new(0),
            frame_num: AtomicU64::new(0),
            thin_active: AtomicBool::new(false),
            seeded: AtomicBool::new(false),
            regions: AtomicU64::new(0),
            gpu_name: Mutex::new(String::new()),
            show_bodies: AtomicBool::new(true),
            show_stage: AtomicBool::new(true),
            bodies_force_color: AtomicBool::new(false),
            overlay: AtomicBool::new(true),
        }
    }

    pub fn set_mbps(&self, v: f64) {
        self.mbps_x100.store((v * 100.0) as u64, Relaxed);
    }
    pub fn set_fps(&self, v: f64) {
        self.fps_x10.store((v * 10.0) as u64, Relaxed);
    }
    pub fn toggle_overlay(&self) {
        self.overlay.store(!self.overlay.load(Relaxed), Relaxed);
    }
}

/// Build the debug panel. Reads telemetry, writes back option toggles.
pub fn ui(ctx: &egui::Context, d: &DebugState) {
    if !d.overlay.load(Relaxed) {
        return;
    }
    egui::Window::new("MapleCast · debug")
        .default_pos([12.0, 12.0])
        .default_width(240.0)
        .resizable(true)
        .show(ctx, |ui| {
            ui.label(format!("GPU: {}", d.gpu_name.lock().unwrap()));
            ui.separator();

            egui::Grid::new("telemetry").num_columns(2).show(ui, |ui| {
                ui.label("fps");
                ui.label(format!("{:.1}", d.fps_x10.load(Relaxed) as f64 / 10.0));
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
                ui.label("game frame");
                ui.label(format!("{}", d.frame_num.load(Relaxed)));
                ui.end_row();
            });

            ui.separator();
            status_line(ui, "thin ZCS2 wire", d.thin_active.load(Relaxed));
            let seeded = d.seeded.load(Relaxed);
            status_line(
                ui,
                &format!("replica RAM ({} regions)", d.regions.load(Relaxed)),
                seeded,
            );

            ui.separator();
            ui.strong("Render");
            checkbox(ui, &d.show_stage, "stage / effects / HUD");
            checkbox(ui, &d.show_bodies, "fighter bodies");
            checkbox(ui, &d.bodies_force_color, "bodies force-color (silhouette)");

            ui.separator();
            ui.weak("F1 toggles this panel");
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
