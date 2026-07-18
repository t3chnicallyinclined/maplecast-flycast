//! S0 — the honest same-stage arrival-jitter probe.
//!
//! The "QUIC halves jitter" claim was CONFOUNDED: QUIC was measured at the net
//! thread (frame decode) while the TCP number was `wire_gap`, measured way
//! downstream at the render loop (after the jitter buffer + lock). Different
//! stages -> not comparable.
//!
//! This probe measures the SAME thing for both transports: the inter-arrival time
//! between consecutive DECODED TDW1 frames, at the net-thread receive point,
//! before any jitter buffer. Both quic.rs and net.rs call `on_frame` at that exact
//! stage and log identical `[arrival TCP|QUIC] p50/p95/p99/max + loss` lines, so
//! the two runs are directly comparable. Percentiles (not just max) because jitter
//! is a tail property — p95/p99 are what a player feels as a hitch.
use std::time::Instant;

pub struct ArrivalProbe {
    label: &'static str,
    last: Option<Instant>,
    last_fn: Option<u32>,
    samples: Vec<f32>, // inter-arrival ms since last accepted frame
    lost: u64,         // frames missing (frame-number gaps)
    rx: u64,
    t0: Instant,
}

impl ArrivalProbe {
    pub fn new(label: &'static str) -> Self {
        Self {
            label,
            last: None,
            last_fn: None,
            samples: Vec::with_capacity(2048),
            lost: 0,
            rx: 0,
            t0: Instant::now(),
        }
    }

    /// Call the instant a TDW1 frame is decoded off the socket (before jitter buffer).
    pub fn on_frame(&mut self, frame_num: u32) {
        let now = Instant::now();
        if let Some(l) = self.last {
            self.samples.push((now.duration_since(l).as_secs_f64() * 1000.0) as f32);
        }
        self.last = Some(now);
        self.rx += 1;
        if let Some(pn) = self.last_fn {
            let d = frame_num.wrapping_sub(pn);
            if d > 1 {
                self.lost += (d - 1) as u64; // gap => datagrams/frames lost in between
            }
        }
        self.last_fn = Some(frame_num);
        if self.t0.elapsed().as_secs_f64() >= 5.0 {
            self.report();
        }
    }

    fn pct(sorted: &[f32], p: f64) -> f32 {
        if sorted.is_empty() {
            return 0.0;
        }
        let idx = ((p / 100.0) * (sorted.len() as f64 - 1.0)).round() as usize;
        sorted[idx.min(sorted.len() - 1)]
    }

    fn report(&mut self) {
        let mut s = self.samples.clone();
        s.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
        let mean = if s.is_empty() { 0.0 } else { s.iter().sum::<f32>() / s.len() as f32 };
        let denom = self.rx + self.lost;
        let losspct = if denom > 0 { self.lost as f64 * 100.0 / denom as f64 } else { 0.0 };
        log::info!(
            "[arrival {}] n={} mean={:.1} p50={:.1} p95={:.1} p99={:.1} max={:.1} ms · loss {:.2}% ({} frames)",
            self.label,
            s.len(),
            mean,
            Self::pct(&s, 50.0),
            Self::pct(&s, 95.0),
            Self::pct(&s, 99.0),
            Self::pct(&s, 100.0),
            losspct,
            self.lost,
        );
        self.samples.clear();
        self.lost = 0;
        self.rx = 0;
        self.t0 = Instant::now();
    }
}
