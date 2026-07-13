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
            host: std::env::var("MAPLECAST_INPUT_HOST").unwrap_or_else(|_| "nobd.net".into()),
            port: std::env::var("MAPLECAST_INPUT_PORT").ok().and_then(|s| s.parse().ok()).unwrap_or(7100),
            slot: std::env::var("MAPLECAST_SLOT").ok().and_then(|s| s.parse().ok()).unwrap_or(0),
        }
    }
}

pub fn spawn_input_thread(cfg: InputConfig) {
    std::thread::Builder::new()
        .name("maplecast-input".into())
        .spawn(move || run(cfg))
        .expect("spawn input thread");
}

fn run(cfg: InputConfig) {
    let sock = match UdpSocket::bind("0.0.0.0:0") {
        Ok(s) => s,
        Err(e) => { log::error!("[input] bind failed: {e}"); return; }
    };
    let target = format!("{}:{}", cfg.host, cfg.port);
    let mut gilrs = match Gilrs::new() {
        Ok(g) => g,
        Err(e) => { log::error!("[input] gilrs init failed: {e:?}"); return; }
    };
    log::info!("[input] gamepad -> UDP {target} (slot {})", cfg.slot);

    let mut last: (u16, u8, u8) = (0xFFFF, 0, 0);
    let mut seq: u32 = 1; // strictly monotonic; server drops seq <= last-seen
    let mut ticks: u32 = 0;

    loop {
        while gilrs.next_event().is_some() {}
        let cur = read_pad(&gilrs);
        ticks = ticks.wrapping_add(1);
        let heartbeat = ticks % 200 == 0; // ~every 200ms at 1kHz
        if cur != last || heartbeat {
            let pkt = build_input_packet(cfg.slot, cur.1, cur.2, cur.0, seq);
            let _ = sock.send_to(&pkt, &target);
            seq = seq.wrapping_add(1);
        }
        last = cur;
        std::thread::sleep(Duration::from_millis(1)); // ~1 kHz
    }
}

/// (active-low 16-bit DreamcastKey mask, lt, rt). Mapping mirrors web/js/gamepad.mjs:149-162.
fn read_pad(gilrs: &Gilrs) -> (u16, u8, u8) {
    let mut btn: u16 = 0xFFFF;
    let mut lt: u8 = 0;
    let mut rt: u8 = 0;
    if let Some((_id, gp)) = gilrs.gamepads().next() {
        if gp.is_pressed(Button::South) { btn &= !0x0004; } // A
        if gp.is_pressed(Button::East)  { btn &= !0x0002; } // B
        if gp.is_pressed(Button::West)  { btn &= !0x0400; } // X
        if gp.is_pressed(Button::North) { btn &= !0x0200; } // Y
        if gp.is_pressed(Button::Start) { btn &= !0x0008; }
        if gp.is_pressed(Button::DPadUp)    { btn &= !0x0010; }
        if gp.is_pressed(Button::DPadDown)  { btn &= !0x0020; }
        if gp.is_pressed(Button::DPadLeft)  { btn &= !0x0040; }
        if gp.is_pressed(Button::DPadRight) { btn &= !0x0080; }
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
