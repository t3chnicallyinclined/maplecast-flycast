//! Native controller -> UDP:7100 input.
//!
//! Lifted from the (validated) desktop/ input path. The "PC" 11-byte packet is
//! CONFIRMED accepted by the live server (0xFE ACK proven on the OVH box), and
//! matches the server parser (core/network/maplecast_input_server.cpp:1237) +
//! the native reference sender (core/network/maplecast_input_sink.cpp:111).

use gilrs::{Button, Gilrs};
use std::net::UdpSocket;
use std::time::Duration;

#[derive(Clone, Debug)]
pub struct InputConfig {
    /// :7100 UDP input server host (public — direct to nobd).
    pub host: String,
    pub port: u16,
    /// 0 = P1, 1 = P2.
    pub slot: u8,
}

impl InputConfig {
    pub fn from_env() -> Self {
        Self {
            host: std::env::var("MAPLECAST_INPUT_HOST").unwrap_or_else(|_| "play.nobd.net".into()),
            port: std::env::var("MAPLECAST_INPUT_PORT").ok().and_then(|s| s.parse().ok()).unwrap_or(7100),
            slot: std::env::var("MAPLECAST_SLOT").ok().and_then(|s| s.parse().ok()).unwrap_or(0),
        }
    }
}

pub fn spawn_input_thread(cfg: InputConfig, debug: std::sync::Arc<crate::debug::DebugState>) {
    std::thread::Builder::new()
        .name("maplecast-input".into())
        .spawn(move || run(cfg, debug))
        .expect("spawn input thread");
}

fn run(cfg: InputConfig, debug: std::sync::Arc<crate::debug::DebugState>) {
    *debug.input_host.lock().unwrap() = cfg.host.clone();
    debug.set_input_slot(cfg.slot); // which E2EP-echoed seq is ours
    let sock = match UdpSocket::bind("0.0.0.0:0") {
        Ok(s) => s,
        Err(e) => { log::error!("[input] bind failed: {e}"); return; }
    };
    let target = format!("{}:{}", cfg.host, cfg.port);
    // Connect so we can recv the server's input ACKs ([0xFE][seq_lo][ts]) for RTT.
    let _ = sock.connect(target.as_str());
    let _ = sock.set_nonblocking(true);
    // HARD INVARIANT: input ALWAYS follows the active VIDEO server. You send your
    // inputs to whatever box you are being RENDERED from — they can NEVER split.
    // MAPLECAST_INPUT_HOST only seeds the bootstrap target above (cfg.host) until the
    // first video connection resolves; it can no longer pin input to a different host
    // than the video (that split sent inputs to localhost while the game ran on NYC).
    debug.input_pinned.store(false, std::sync::atomic::Ordering::Relaxed);
    let mut input_server_idx = u64::MAX;
    let mut gilrs = match Gilrs::new() {
        Ok(g) => g,
        Err(e) => { log::error!("[input] gilrs init failed: {e:?}"); return; }
    };
    log::info!("[input] gamepad -> UDP {target} (slot {})", cfg.slot);
    for (id, gp) in gilrs.gamepads() {
        log::info!("[input] pad {id:?}: {} (using the first listed)", gp.name());
    }

    let mut last: (u16, u8, u8) = (0xFFFF, 0, 0);
    let mut seq: u32 = 1; // strictly monotonic; server drops seq <= last-seen
    let mut ticks: u32 = 0;
    let mut send_times: [Option<std::time::Instant>; 256] = [None; 256];
    let mut rtt_ema: Option<f64> = None;
    let mut rx = [0u8; 16];
    let input_dbg = std::env::var("MAPLECAST_INPUT_DEBUG").is_ok();
    // MAPLECAST_PAD_INDEX picks which enumerated gamepad to read (default 0). Use it
    // to skip a phantom/ghost slot (wireless receiver, Steam Input, virtual pad).
    let pad_index: usize = std::env::var("MAPLECAST_PAD_INDEX").ok()
        .and_then(|s| s.trim().parse().ok()).unwrap_or(0);
    let mut pad_logged = false;

    loop {
        // Enumerate EVERY pad the first time any appear, so a phantom/ghost slot is
        // visible. #index = the MAPLECAST_PAD_INDEX to select it.
        if !pad_logged {
            let pads: Vec<_> = gilrs.gamepads().collect();
            if !pads.is_empty() {
                log::info!("[input] {} gamepad(s) (reading #{pad_index}; set MAPLECAST_PAD_INDEX to change):", pads.len());
                for (i, (id, gp)) in pads.iter().enumerate() {
                    let hex: String = gp.uuid().iter().map(|b| format!("{b:02x}")).collect();
                    log::info!("[input]   #{i} [{id:?}] name='{}' connected={} uuid={hex} map={:?}",
                        gp.name(), gp.is_connected(), gp.mapping_source());
                }
                pad_logged = true;
            }
        }
        {
            // Input follows the active video server every tick — the hard invariant
            // that keeps input and render on the same box (never split).
            let idx = debug.active_server_idx();
            if idx != input_server_idx {
                // Only latch the index once the retarget SUCCEEDS — a failed
                // UDP connect (DNS blip, empty host) must retry next tick, not
                // silently leave input pointed at the previous server forever.
                match debug.server_input_host(idx as usize) {
                    Some(host) if !host.is_empty() => {
                        // Directory entries may carry "host:port" (multi-instance
                        // boxes run input servers on non-7100 ports); bare hosts
                        // keep the launcher-configured default port.
                        let t = if host.contains(':') {
                            host.clone()
                        } else {
                            format!("{}:{}", host, cfg.port)
                        };
                        match sock.connect(t.as_str()) {
                            Ok(()) => {
                                input_server_idx = idx;
                                *debug.input_host.lock().unwrap() = host;
                                // fresh link: stale RTT samples/EMA belong to the
                                // old server; force an immediate state packet too
                                send_times = [None; 256];
                                rtt_ema = None;
                                last = (0xFFFF ^ 1, 0, 0); // != any real read -> resend now
                                log::info!("[input] retargeted -> {t}");
                            }
                            Err(e) => log::warn!("[input] retarget {t} failed: {e} (retrying)"),
                        }
                    }
                    _ => {
                        input_server_idx = idx; // no host known for this entry
                        log::warn!("[input] server #{idx} has no input host — input stays on previous target");
                    }
                }
            }
        }
        // Pump gilrs events. Log connect/disconnect/drop so a controller dropping out
        // (the "stuck until I reconnect" symptom) is visible in the log, and so
        // is_connected() in read_pad reflects reality this tick.
        while let Some(ev) = gilrs.next_event() {
            match ev.event {
                gilrs::EventType::Connected =>
                    log::info!("[input] gamepad CONNECTED {:?}", ev.id),
                gilrs::EventType::Disconnected =>
                    log::warn!("[input] gamepad DISCONNECTED {:?} — sending neutral until it returns", ev.id),
                gilrs::EventType::Dropped =>
                    log::warn!("[input] gamepad DROPPED {:?}", ev.id),
                _ => {}
            }
        }
        let cur = read_pad(&gilrs, pad_index);
        // MAPLECAST_INPUT_DEBUG=1: on every input change, dump the RAW gilrs state
        // (which buttons/axes actually fire) so a mis-mapped d-pad/stick is visible.
        if input_dbg && cur != last {
            if let Some((_id, gp)) = gilrs.gamepads().next() {
                log::info!("[input-dbg] mask=0x{:04X} lt={} rt={} | RAW {}", cur.0, cur.1, cur.2, dump_pad(&gp));
            }
        }
        ticks = ticks.wrapping_add(1);
        let heartbeat = ticks % 200 == 0; // ~every 200ms at 1kHz
        if cur != last || heartbeat {
            let pkt = build_input_packet(cfg.slot, cur.1, cur.2, cur.0, seq);
            let _ = sock.send(&pkt);
            send_times[(seq & 0xFF) as usize] = Some(std::time::Instant::now());
            debug.input_sent(seq); // stamp for the E2E press->present probe
            seq = seq.wrapping_add(1);
        }
        // Drain input ACKs ([0xFE][seq_lo][ts]) -> input RTT (native UDP round-trip).
        while let Ok(n) = sock.recv(&mut rx) {
            if n >= 2 && rx[0] == 0xFE {
                if let Some(t) = send_times[rx[1] as usize].take() {
                    let rtt = t.elapsed().as_secs_f64() * 1000.0;
                    let ema = rtt_ema.map_or(rtt, |e| e * 0.85 + rtt * 0.15);
                    rtt_ema = Some(ema);
                    debug.set_rtt(ema);
                }
            }
        }
        last = cur;
        std::thread::sleep(Duration::from_millis(1)); // ~1 kHz
    }
}

/// (active-low 16-bit DreamcastKey mask, lt, rt). Mapping mirrors web/js/gamepad.mjs:149-162.
fn read_pad(gilrs: &Gilrs, pad_index: usize) -> (u16, u8, u8) {
    let mut btn: u16 = 0xFFFF;
    let mut lt: u8 = 0;
    let mut rt: u8 = 0;
    // Only read CONNECTED pads. gilrs keeps disconnected handles in gamepads(), and
    // reading one returns its FROZEN last state — that's the "inputs stuck until I
    // reconnect the controller" bug. Filtering to connected pads means a dropout
    // sends neutral (no stuck buttons) and auto-recovers when the pad returns.
    if let Some((_id, gp)) = gilrs.gamepads().filter(|(_, g)| g.is_connected()).nth(pad_index) {
        if gp.is_pressed(Button::South) { btn &= !0x0004; } // A
        if gp.is_pressed(Button::East)  { btn &= !0x0002; } // B
        if gp.is_pressed(Button::West)  { btn &= !0x0400; } // X
        if gp.is_pressed(Button::North) { btn &= !0x0200; } // Y
        if gp.is_pressed(Button::Start) { btn &= !0x0008; }
        if gp.is_pressed(Button::DPadUp)    { btn &= !0x0010; }
        if gp.is_pressed(Button::DPadDown)  { btn &= !0x0020; }
        if gp.is_pressed(Button::DPadLeft)  { btn &= !0x0040; }
        if gp.is_pressed(Button::DPadRight) { btn &= !0x0080; }
        // Directions ALSO from the left stick and hat-style d-pads: many pads
        // (DualShock/DirectInput, generic sticks) report the d-pad as an
        // axis/hat, so is_pressed(DPad*) never fires while face buttons work.
        // Mirror both onto the same active-low direction bits.
        const DEAD: f32 = 0.5;
        let ax = |a: gilrs::Axis| gp.axis_data(a).map(|d| d.value()).unwrap_or(0.0);
        let x = ax(gilrs::Axis::LeftStickX) + ax(gilrs::Axis::DPadX);
        let y = ax(gilrs::Axis::LeftStickY) + ax(gilrs::Axis::DPadY);
        if y >  DEAD { btn &= !0x0010; } // up
        if y < -DEAD { btn &= !0x0020; } // down
        if x < -DEAD { btn &= !0x0040; } // left
        if x >  DEAD { btn &= !0x0080; } // right
        lt = analog(button_val(&gp, Button::LeftTrigger2));
        rt = analog(button_val(&gp, Button::RightTrigger2));
        if gp.is_pressed(Button::RightTrigger) { lt = 255; } // RB -> LT (Assist 1)
        if gp.is_pressed(Button::LeftTrigger)  { rt = 255; } // LB -> RT (Assist 2)
    }
    (btn, lt, rt)
}

fn analog(v: f32) -> u8 { (v.clamp(0.0, 1.0) * 255.0) as u8 }
fn button_val(gp: &gilrs::Gamepad<'_>, b: Button) -> f32 {
    gp.button_data(b).map(|d| d.value()).unwrap_or(0.0)
}

/// Diagnostic dump: which gilrs buttons/axes actually fire on this pad. Reveals a
/// mis-mapped d-pad/stick (buttons work but directions map to axes we don't check).
fn dump_pad(gp: &gilrs::Gamepad<'_>) -> String {
    use gilrs::{Axis as A, Button as B};
    let mut s = String::new();
    for b in [B::South, B::East, B::North, B::West, B::C, B::Z, B::Start, B::Select, B::Mode,
              B::LeftTrigger, B::LeftTrigger2, B::RightTrigger, B::RightTrigger2,
              B::LeftThumb, B::RightThumb, B::DPadUp, B::DPadDown, B::DPadLeft, B::DPadRight] {
        if gp.is_pressed(b) { s += &format!("{b:?} "); }
    }
    for a in [A::LeftStickX, A::LeftStickY, A::RightStickX, A::RightStickY, A::DPadX, A::DPadY, A::LeftZ, A::RightZ] {
        let v = gp.axis_data(a).map(|d| d.value()).unwrap_or(0.0);
        if v.abs() > 0.15 { s += &format!("{a:?}={v:+.2} "); }
    }
    if s.is_empty() { s.push_str("(nothing fired)"); }
    s
}

/// "PC" 11-byte packet: "PC"(2) slot(1) seq(u32 LE) LT(1) RT(1) btn_hi(1,BE) btn_lo(1,BE)
fn build_input_packet(slot: u8, lt: u8, rt: u8, btn: u16, seq: u32) -> [u8; 11] {
    let s = slot.min(1);
    [
        b'P', b'C', s,
        seq as u8, (seq >> 8) as u8, (seq >> 16) as u8, (seq >> 24) as u8,
        lt, rt,
        (btn >> 8) as u8, (btn & 0xFF) as u8,
    ]
}
