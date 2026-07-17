//! Shared debug/telemetry + render-option state, and the egui overlay (toggle: F1).
//!
//! Telemetry fields are written by the net/replica/render paths and read by the UI;
//! render-option fields are written by the UI and read by the render loop. All
//! lock-free atomics so no thread ever blocks on the panel.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering::Relaxed};
use std::sync::Mutex;

/// A connectable server (settings panel: probed + switchable at runtime).
#[derive(Clone)]
pub struct ServerEntry {
    pub name: String,
    pub ws: String,    // wire websocket url
    pub input: String, // input-server host (:7100/udp, also the RTT probe target)
}

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
    wire_gap_max_x100: AtomicU64, // worst interval between two GAME-frame updates (motion jitter)
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
    // --- per-leg wire breakdown (rates over the net thread's 2s window, ×100) ---
    // Locally there is NO relay shedding: the socket carries EVERY leg at once
    // (legacy ZCST + ZCS2 + TDW + side channels), so the total "bandwidth" number
    // is NOT what any one wire costs. This decomposes it.
    leg_zcst_x100: AtomicU64,
    leg_zcs2_x100: AtomicU64,
    leg_tdw_x100: AtomicU64,  // TDW1 + TDWS combined
    leg_side_x100: AtomicU64, // GSTA/OBJS/PALF/audio/unknown
    // --- TDW dict-wire state (test clone) ---
    pub tdw_mode: Mutex<String>,   // "", "gate", "render"
    pub tdw_eq: AtomicU64,
    pub tdw_ne: AtomicU64,
    pub tdw_dict_blocks: AtomicU64,
    pub tdw_dict_kb: AtomicU64,
    pub tdw_epoch: AtomicU64,      // u64::MAX until a TDWS arrives
    pub tdw_synced: AtomicBool,
    // --- display settings (settings.rs window, F2; main loop APPLIES these) ---
    pub settings_open: AtomicBool,
    pub req_scale: AtomicU64,      // 0 = none; 1/2/3 = request 640/1280/1920-wide (4:3)
    pub req_fullscreen: AtomicU64, // 2 = none; 1 = enter borderless; 0 = exit
    pub fullscreen_on: AtomicBool, // current state (panel display)
    pub vsync_on: AtomicBool,      // desired present mode; loop reconfigures on change
    // --- server directory (settings panel probe/connect) ---
    pub servers: Mutex<Vec<ServerEntry>>,
    pub server_rtt_x100: [AtomicU64; 8], // per-index UDP probe RTT (u64::MAX = down)
    pub active_server: AtomicU64,
    pub switch_server: AtomicU64,        // u64::MAX = none; else requested index
    pub input_pinned: AtomicBool,        // MAPLECAST_INPUT_HOST set: panel switches don't move input
    // --- live state migration (server transfer, docs/STATE-HANDOFF-PLAN.md) ---
    pub transfer_server: AtomicU64,      // u64::MAX = none; else index to hand the GAME to
    pub migrate_status: Mutex<String>,   // human status line for the panel
    // --- local play (client-managed headless server on this PC) ---
    pub local_rom: Mutex<String>,        // GDI path for the local server
    pub local_play_cmd: AtomicU64,       // 0 none, 1 start, 2 stop
    pub local_srv_on: AtomicBool,        // child process alive
    // --- tabbed side panel ---
    pub active_tab: AtomicU64,           // 0 debug, 1 servers, 2 display
    // user picked a server by hand — the auto-closest probe stops overriding
    pub manual_server: AtomicBool,
    // fullscreen pillarbox HUD: connection/server info in the side bars (F3)
    pub bars_hud: AtomicBool,
    // --- one-protocol vitals ---
    pub tdw_pending_depth: AtomicU64, // TDW1 frames awaiting their ZCS2 sibling (pairing health)
    pub tdw_pages: AtomicU64,         // dirty pages applied from the TDW1 in-band section (last frame)
    pub stage_strips: AtomicU64,      // local-stage strips drawn last frame
    pub stage_culled: AtomicU64,      // local-stage vertices culled last frame (per-tri rule)
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
            wire_gap_max_x100: AtomicU64::new(0),
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
            leg_zcst_x100: AtomicU64::new(0),
            leg_zcs2_x100: AtomicU64::new(0),
            leg_tdw_x100: AtomicU64::new(0),
            leg_side_x100: AtomicU64::new(0),
            tdw_mode: Mutex::new(String::new()),
            tdw_eq: AtomicU64::new(0),
            tdw_ne: AtomicU64::new(0),
            tdw_dict_blocks: AtomicU64::new(0),
            tdw_dict_kb: AtomicU64::new(0),
            tdw_epoch: AtomicU64::new(u64::MAX),
            tdw_synced: AtomicBool::new(false),
            settings_open: AtomicBool::new(false),
            req_scale: AtomicU64::new(0),
            req_fullscreen: AtomicU64::new(2),
            fullscreen_on: AtomicBool::new(false),
            vsync_on: AtomicBool::new(false),
            servers: Mutex::new(Vec::new()),
            server_rtt_x100: std::array::from_fn(|_| AtomicU64::new(u64::MAX)),
            active_server: AtomicU64::new(0),
            switch_server: AtomicU64::new(u64::MAX),
            input_pinned: AtomicBool::new(false),
            transfer_server: AtomicU64::new(u64::MAX),
            migrate_status: Mutex::new(String::new()),
            local_rom: Mutex::new(
                std::env::var("MAPLECAST_ROM")
                    .unwrap_or_else(|_| r"C:\roms\roms\mvc2.gdi".into()),
            ),
            local_play_cmd: AtomicU64::new(0),
            local_srv_on: AtomicBool::new(false),
            active_tab: AtomicU64::new(1), // start on Servers = the connect picker
            manual_server: AtomicBool::new(false),
            bars_hud: AtomicBool::new(true),
            tdw_pending_depth: AtomicU64::new(0),
            tdw_pages: AtomicU64::new(0),
            stage_strips: AtomicU64::new(0),
            stage_culled: AtomicU64::new(0),
        }
    }

    pub fn active_server_idx(&self) -> u64 {
        self.active_server.load(Relaxed)
    }
    pub fn server_input_host(&self, idx: usize) -> Option<String> {
        self.servers.lock().unwrap().get(idx).map(|s| s.input.clone())
    }

    /// Per-leg wire rates (Mbps) computed by the net thread over its 2s window.
    pub fn set_leg_mbps(&self, zcst: f64, zcs2: f64, tdw: f64, side: f64) {
        self.leg_zcst_x100.store((zcst * 100.0) as u64, Relaxed);
        self.leg_zcs2_x100.store((zcs2 * 100.0) as u64, Relaxed);
        self.leg_tdw_x100.store((tdw * 100.0) as u64, Relaxed);
        self.leg_side_x100.store((side * 100.0) as u64, Relaxed);
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
    /// Worst interval (ms) between two GAME-frame content updates in the window
    /// — the motion-smoothness metric. ~16.7 = perfectly paced 60fps; a spike
    /// means a frame arrived late/bunched (the visible micro-teleport).
    pub fn set_wire_gap(&self, ms: f64) {
        self.wire_gap_max_x100.store((ms * 100.0) as u64, Relaxed);
    }
    pub fn wire_gap_max_ms(&self) -> f64 {
        self.wire_gap_max_x100.load(Relaxed) as f64 / 100.0
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

/// Debug/telemetry tab body (rendered inside the tabbed side panel).
pub fn body(ui: &mut egui::Ui, d: &DebugState) {
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
                ui.label("bandwidth (ALL legs)");
                ui.label(format!("{:.2} Mbps", d.mbps_x100.load(Relaxed) as f64 / 100.0));
                ui.end_row();
                // Per-leg decomposition — locally the socket carries every shadow
                // leg at once (no relay shedding); only ONE of these is what the
                // client actually renders from.
                ui.label("  ├ legacy ZCST");
                ui.label(format!("{:.2} Mbps  (full mirror; SYNC-only here)", d.leg_zcst_x100.load(Relaxed) as f64 / 100.0));
                ui.end_row();
                ui.label("  ├ ZCS2");
                ui.label(format!("{:.2} Mbps  (thin wire: pages + TA delta)", d.leg_zcs2_x100.load(Relaxed) as f64 / 100.0));
                ui.end_row();
                ui.label("  ├ TDW dict-wire");
                ui.label(format!("{:.2} Mbps  (geometry as dict refs)", d.leg_tdw_x100.load(Relaxed) as f64 / 100.0));
                ui.end_row();
                ui.label("  └ side channels");
                ui.label(format!("{:.2} Mbps  (GSTA/OBJS/PALF/…)", d.leg_side_x100.load(Relaxed) as f64 / 100.0));
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

            let tdw_mode = d.tdw_mode.lock().unwrap().clone();
            if !tdw_mode.is_empty() {
                ui.separator();
                ui.strong("TDW dict-wire");
                let (eq, ne) = (d.tdw_eq.load(Relaxed), d.tdw_ne.load(Relaxed));
                let epoch = d.tdw_epoch.load(Relaxed);
                let synced = d.tdw_synced.load(Relaxed);
                status_line(
                    ui,
                    &format!(
                        "{} — {}",
                        match tdw_mode.as_str() {
                            "render" => "RENDERING FROM DICT WIRE",
                            "players" => "PLAYERS-ONLY DICT WIRE (0.35 Mbps class)",
                            _ => "gate (compare only)",
                        },
                        if synced { "synced" } else { "waiting for TDWS+streamStart" }
                    ),
                    synced,
                );
                if epoch != u64::MAX {
                    ui.label(format!(
                        "dict: {} blocks / {:.1} MB  (epoch {})",
                        d.tdw_dict_blocks.load(Relaxed),
                        d.tdw_dict_kb.load(Relaxed) as f64 / 1024.0,
                        epoch
                    ));
                }
                let col = if ne == 0 {
                    egui::Color32::from_rgb(80, 220, 120)
                } else {
                    egui::Color32::from_rgb(255, 90, 90)
                };
                ui.colored_label(col, format!("TA byte-equality: eq={eq} ne={ne}"));
                ui.label(format!(
                    "pairing buffer: {}   pages via TDW: {}/frame",
                    d.tdw_pending_depth.load(Relaxed),
                    d.tdw_pages.load(Relaxed)
                ));
                ui.label(format!(
                    "local stage: {} strips drawn, {} verts culled",
                    d.stage_strips.load(Relaxed),
                    d.stage_culled.load(Relaxed)
                ));
            }

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
