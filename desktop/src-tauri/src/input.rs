//! Native controller -> UDP:7100 input path.
//!
//! This is the reason the desktop client exists. Instead of the browser's
//! rAF-gated Gamepad API (~8ms sampling) sent over a TCP WebSocket, we poll the
//! pad natively at ~1kHz on a dedicated thread and `sendto` a small UDP packet
//! straight to the MapleCast input server, off the webview entirely.

use gilrs::{Button, Gilrs};
use std::net::UdpSocket;
use std::time::Duration;

#[derive(Clone, Debug)]
pub struct InputConfig {
    /// The live client the window loads (default https://play.nobd.net/webgpu-test.html).
    pub client_url: String,
    /// Host of the :7100 UDP input server (defaults to the client's host).
    pub input_host: String,
    /// UDP input port (public per the port map).
    pub input_port: u16,
    /// Player slot this client drives. 0 = P1, 1 = P2, ...
    /// TODO(session): today this is fixed via env / deep-link `slot=`. The real
    /// flow should learn the slot from the server's join handshake so a shared
    /// match link auto-assigns "you are P2". See README "Session / slot binding".
    pub slot: u8,
}

impl InputConfig {
    pub fn from_env() -> Self {
        let client_url = std::env::var("MAPLECAST_URL")
            .unwrap_or_else(|_| "https://play.nobd.net/webgpu-test.html".to_string());
        let input_host = std::env::var("MAPLECAST_INPUT_HOST")
            .ok()
            .filter(|s| !s.is_empty())
            .or_else(|| host_of(&client_url))
            .unwrap_or_else(|| "play.nobd.net".to_string());
        let input_port = std::env::var("MAPLECAST_INPUT_PORT")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(7100);
        let slot = std::env::var("MAPLECAST_SLOT")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(0);
        Self { client_url, input_host, input_port, slot }
    }

    /// URL loaded on a normal (non-deep-link) launch. `native=1` tells the page
    /// to suppress its own browser gamepad->WS sender so input isn't double-sent.
    pub fn boot_url(&self) -> String {
        add_query(&self.client_url, &[("native", "1"), ("slot", &self.slot.to_string())])
    }
}

/// Spawn the polling thread. Never blocks the caller / UI.
pub fn spawn_input_thread(cfg: InputConfig) {
    std::thread::Builder::new()
        .name("maplecast-input".into())
        .spawn(move || run_input_loop(cfg))
        .expect("failed to spawn input thread");
}

fn run_input_loop(cfg: InputConfig) {
    let sock = match UdpSocket::bind("0.0.0.0:0") {
        Ok(s) => s,
        Err(e) => {
            log::error!("[input] could not bind UDP socket: {e}");
            return;
        }
    };
    let target = format!("{}:{}", cfg.input_host, cfg.input_port);
    let mut gilrs = match Gilrs::new() {
        Ok(g) => g,
        Err(e) => {
            log::error!("[input] gilrs init failed: {e:?}");
            return;
        }
    };
    log::info!("[input] polling gamepad -> UDP {target} (slot {})", cfg.slot);

    // active-low: 0xFFFF = nothing pressed
    let mut last: (u16, u8, u8) = (0xFFFF, 0, 0);
    // seq MUST be strictly monotonic per source — the server drops any packet
    // with seq <= last-seen (dedup, input_server.cpp:1266-1296). Start at 1 to
    // match the native reference sender (buildPacket seq = fetch_add(1)+1).
    let mut seq: u32 = 1;
    let mut ticks: u32 = 0;

    loop {
        // Keep gilrs' internal state current (connect/disconnect, button state).
        while gilrs.next_event().is_some() {}

        let cur = read_pad(&gilrs);
        ticks = ticks.wrapping_add(1);

        // Send on any change, plus a periodic heartbeat so the server never
        // idles-out our slot (the browser client does the same, gamepad.mjs).
        let heartbeat = ticks % HEARTBEAT_TICKS == 0;
        if cur != last || heartbeat {
            let pkt = build_input_packet(cfg.slot, cur.1, cur.2, cur.0, seq);
            let _ = sock.send_to(&pkt, &target);
            seq = seq.wrapping_add(1);
        }
        last = cur;

        std::thread::sleep(POLL_INTERVAL); // ~1 kHz
    }
}

const POLL_INTERVAL: Duration = Duration::from_millis(1);
/// ~1kHz poll; heartbeat roughly every 200ms.
const HEARTBEAT_TICKS: u32 = 200;

/// Read the active pad into (active-low 16-bit button mask, lt, rt).
/// Button/trigger mapping mirrors web/js/gamepad.mjs:148-162 exactly.
fn read_pad(gilrs: &Gilrs) -> (u16, u8, u8) {
    let mut btn: u16 = 0xFFFF;
    let mut lt: u8 = 0;
    let mut rt: u8 = 0;

    // First connected pad. TODO: pick most-recently-active for hot-swap parity
    // with the browser client (gamepad.mjs walks all 4 slots by timestamp).
    // Button bits are DreamcastKey (core/input/gamepad.h:22-37); mapping mirrors
    // web/js/gamepad.mjs:149-162 exactly.
    if let Some((_id, gp)) = gilrs.gamepads().next() {
        if gp.is_pressed(Button::South) { btn &= !0x0004; } // A     (gamepad[0])
        if gp.is_pressed(Button::East)  { btn &= !0x0002; } // B     (gamepad[1])
        if gp.is_pressed(Button::West)  { btn &= !0x0400; } // X     (gamepad[2])
        if gp.is_pressed(Button::North) { btn &= !0x0200; } // Y     (gamepad[3])
        if gp.is_pressed(Button::Start) { btn &= !0x0008; } // Start (gamepad[9])
        if gp.is_pressed(Button::DPadUp)    { btn &= !0x0010; }
        if gp.is_pressed(Button::DPadDown)  { btn &= !0x0020; }
        if gp.is_pressed(Button::DPadLeft)  { btn &= !0x0040; }
        if gp.is_pressed(Button::DPadRight) { btn &= !0x0080; }

        // Analog triggers: LeftTrigger2 / RightTrigger2 are the analog pulls.
        lt = analog(button_val(&gp, Button::LeftTrigger2));
        rt = analog(button_val(&gp, Button::RightTrigger2));
        // Bumpers doubled as assists, swapped exactly like gamepad.mjs:161-162:
        //   RB -> LT (Assist 1), LB -> RT (Assist 2).
        if gp.is_pressed(Button::RightTrigger) { lt = 255; }
        if gp.is_pressed(Button::LeftTrigger)  { rt = 255; }
    }

    (btn, lt, rt)
}

fn analog(v: f32) -> u8 {
    (v.clamp(0.0, 1.0) * 255.0) as u8
}

fn button_val(gp: &gilrs::Gamepad<'_>, b: Button) -> f32 {
    gp.button_data(b).map(|d| d.value()).unwrap_or(0.0)
}

// ============================================================================
// WIRE FORMAT — :7100 UDP input packet  ("PC" 11-byte)
// ============================================================================
// CONFIRMED against the server parser (core/network/maplecast_input_server.cpp
// udpThreadLoop :1140, "PC" branch :1237) and the native reference sender
// (core/network/maplecast_input_sink.cpp buildPacket :111-138).
//
//   off sz field    endian   note
//    0   2  "PC"     -        0x50 0x43  (magic; a remote client MUST use this —
//                                         the 5-byte/0x49 forms are loopback-only,
//                                         rejected from a non-loopback source :1225)
//    2   1  slot     -        0 = P1, 1 = P2 (server clamps to <=1; auto-binds
//                                             this slot to our src IP:port, FCFS,
//                                             last-writer-wins, NO AUTH :1372)
//    3   4  seq      LITTLE   strictly monotonic per source; seq<=last => DROPPED
//    7   1  LT       -        0..255 analog trigger (MVC2 assist A1)
//    8   1  RT       -        0..255 analog trigger (MVC2 assist A2)
//    9   1  btn_hi   BIG      high byte of the active-low 16-bit DreamcastKey mask
//   10   1  btn_lo   BIG      low byte
//
// The 19-byte variant appends [client_ts:u64 LE] for input-age telemetry (opt).
// The server ACKs each packet with [0xFE][seq_lo][ts:u48 LE] (unused here).
// ============================================================================
const PKT_LEN: usize = 11;

fn build_input_packet(slot: u8, lt: u8, rt: u8, btn: u16, seq: u32) -> [u8; PKT_LEN] {
    let s = slot.min(1); // server "PC" guard requires slot <= 1
    [
        b'P',
        b'C',
        s,
        seq as u8,
        (seq >> 8) as u8,
        (seq >> 16) as u8,
        (seq >> 24) as u8,
        lt,
        rt,
        (btn >> 8) as u8,
        (btn & 0xFF) as u8,
    ]
}

// ---------------------------------------------------------------------------
// Deep-link + URL helpers
// ---------------------------------------------------------------------------

/// `maplecast://match/<id>?slot=N` -> `<client_url>?native=1&slot=N&match=<id>`
pub fn match_url_from_deep_link(deep: &str, cfg: &InputConfig) -> String {
    let mut match_id = String::new();
    let mut slot = cfg.slot;

    if let Ok(u) = url::Url::parse(deep) {
        if let Some(segs) = u.path_segments() {
            if let Some(last) = segs.filter(|s| !s.is_empty()).last() {
                match_id = last.to_string();
            }
        }
        for (k, v) in u.query_pairs() {
            match k.as_ref() {
                "slot" => {
                    if let Ok(n) = v.parse() {
                        slot = n;
                    }
                }
                "match" if match_id.is_empty() => match_id = v.to_string(),
                _ => {}
            }
        }
        if match_id.is_empty() {
            if let Some(h) = u.host_str() {
                if h != "match" {
                    match_id = h.to_string();
                }
            }
        }
    }

    let slot_s = slot.to_string();
    let mut params: Vec<(&str, &str)> = vec![("native", "1"), ("slot", &slot_s)];
    if !match_id.is_empty() {
        params.push(("match", &match_id));
    }
    add_query(&cfg.client_url, &params)
}

fn host_of(u: &str) -> Option<String> {
    url::Url::parse(u).ok().and_then(|x| x.host_str().map(|h| h.to_string()))
}

fn add_query(base: &str, extra: &[(&str, &str)]) -> String {
    match url::Url::parse(base) {
        Ok(mut u) => {
            {
                let mut qp = u.query_pairs_mut();
                for (k, v) in extra {
                    qp.append_pair(k, v);
                }
            }
            u.to_string()
        }
        Err(_) => base.to_string(),
    }
}
