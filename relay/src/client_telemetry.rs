// ============================================================================
// CLIENT_TELEMETRY — Per-client latency metrics ingest + aggregation
//
// Browsers POST a JSON snapshot every 5s. We keep the latest report from each
// client (keyed by client_id) in a HashMap. Old reports (>30s) are evicted on
// each push so dead clients don't poison the aggregates.
//
// Aggregates exposed via metrics_snapshot():
//   - count of active clients reporting
//   - p50 / p95 / max RTT
//   - min / avg / max FPS
//   - max frame jitter spike
//   - per-browser breakdown (firefox / chrome / safari / other)
// ============================================================================

use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::sync::Arc;
use std::time::{Instant, Duration};
use tokio::sync::RwLock;

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct ClientReport {
    pub client_id: String,
    pub ua: String,
    pub rtt_ms: f64,
    pub fps: f64,
    pub mbps: f64,
    pub frame_jitter_avg_us: u64,
    pub frame_jitter_max_us: u64,
    pub sync_count: u64,
    pub streaming: u32,
}

#[derive(Debug, Clone)]
struct StoredReport {
    report: ClientReport,
    received_at: Instant,
}

#[derive(Clone)]
pub struct ClientTelemetry {
    inner: Arc<RwLock<HashMap<String, StoredReport>>>,
}

const STALE_AFTER: Duration = Duration::from_secs(30);

impl ClientTelemetry {
    pub fn new() -> Self {
        Self { inner: Arc::new(RwLock::new(HashMap::new())) }
    }

    pub async fn ingest(&self, report: ClientReport) {
        let mut map = self.inner.write().await;
        let now = Instant::now();
        // Evict stale entries while we hold the lock — cheap, only ~50 clients
        map.retain(|_, v| now.duration_since(v.received_at) < STALE_AFTER);
        map.insert(report.client_id.clone(), StoredReport {
            report,
            received_at: now,
        });
    }

    pub async fn snapshot(&self) -> TelemetryAggregate {
        let map = self.inner.read().await;
        let now = Instant::now();
        let mut rtts: Vec<f64> = Vec::with_capacity(map.len());
        let mut fpss: Vec<f64> = Vec::with_capacity(map.len());
        let mut max_jitter: u64 = 0;
        let mut total_mbps: f64 = 0.0;
        let mut by_ua: HashMap<String, u64> = HashMap::new();
        let mut streaming_count = 0;

        for stored in map.values() {
            // Skip stale entries (in case retain hasn't run since the last push)
            if now.duration_since(stored.received_at) >= STALE_AFTER { continue; }
            let r = &stored.report;
            if r.rtt_ms > 0.0 { rtts.push(r.rtt_ms); }
            if r.fps > 0.0 { fpss.push(r.fps); }
            if r.frame_jitter_max_us > max_jitter { max_jitter = r.frame_jitter_max_us; }
            total_mbps += r.mbps;
            *by_ua.entry(r.ua.clone()).or_insert(0) += 1;
            if r.streaming > 0 { streaming_count += 1; }
        }

        rtts.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
        fpss.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));

        TelemetryAggregate {
            count: map.len() as u64,
            streaming_count,
            rtt_p50_ms: percentile(&rtts, 0.50),
            rtt_p95_ms: percentile(&rtts, 0.95),
            rtt_max_ms: rtts.last().copied().unwrap_or(0.0),
            fps_min: fpss.first().copied().unwrap_or(0.0),
            fps_avg: if fpss.is_empty() { 0.0 } else { fpss.iter().sum::<f64>() / fpss.len() as f64 },
            fps_max: fpss.last().copied().unwrap_or(0.0),
            jitter_max_us: max_jitter,
            total_mbps,
            by_ua,
        }
    }
}

fn percentile(sorted: &[f64], pct: f64) -> f64 {
    if sorted.is_empty() { return 0.0; }
    let idx = ((sorted.len() as f64 - 1.0) * pct).round() as usize;
    sorted[idx.min(sorted.len() - 1)]
}

pub struct TelemetryAggregate {
    pub count: u64,
    pub streaming_count: u64,
    pub rtt_p50_ms: f64,
    pub rtt_p95_ms: f64,
    pub rtt_max_ms: f64,
    pub fps_min: f64,
    pub fps_avg: f64,
    pub fps_max: f64,
    pub jitter_max_us: u64,
    pub total_mbps: f64,
    pub by_ua: HashMap<String, u64>,
}

impl TelemetryAggregate {
    /// Render as Prometheus 0.0.4 text format. Caller appends to a String.
    pub fn render_prometheus(&self, out: &mut String) {
        out.push_str("# HELP nobd_client_count Number of browser clients reporting telemetry\n");
        out.push_str("# TYPE nobd_client_count gauge\n");
        out.push_str(&format!("nobd_client_count {}\n", self.count));

        out.push_str("# HELP nobd_client_streaming Number of clients actively rendering frames\n");
        out.push_str("# TYPE nobd_client_streaming gauge\n");
        out.push_str(&format!("nobd_client_streaming {}\n", self.streaming_count));

        out.push_str("# HELP nobd_client_rtt_ms WebSocket round-trip time across all clients\n");
        out.push_str("# TYPE nobd_client_rtt_ms gauge\n");
        out.push_str(&format!("nobd_client_rtt_ms{{quantile=\"0.5\"}} {}\n", self.rtt_p50_ms));
        out.push_str(&format!("nobd_client_rtt_ms{{quantile=\"0.95\"}} {}\n", self.rtt_p95_ms));
        out.push_str(&format!("nobd_client_rtt_ms{{quantile=\"max\"}} {}\n", self.rtt_max_ms));

        out.push_str("# HELP nobd_client_fps Rendered frames per second across all clients\n");
        out.push_str("# TYPE nobd_client_fps gauge\n");
        out.push_str(&format!("nobd_client_fps{{stat=\"min\"}} {}\n", self.fps_min));
        out.push_str(&format!("nobd_client_fps{{stat=\"avg\"}} {}\n", self.fps_avg));
        out.push_str(&format!("nobd_client_fps{{stat=\"max\"}} {}\n", self.fps_max));

        out.push_str("# HELP nobd_client_jitter_max_us Worst-case inter-frame interval seen by any client\n");
        out.push_str("# TYPE nobd_client_jitter_max_us gauge\n");
        out.push_str(&format!("nobd_client_jitter_max_us {}\n", self.jitter_max_us));

        out.push_str("# HELP nobd_client_total_mbps Combined inbound bandwidth across all clients\n");
        out.push_str("# TYPE nobd_client_total_mbps gauge\n");
        out.push_str(&format!("nobd_client_total_mbps {}\n", self.total_mbps));

        out.push_str("# HELP nobd_client_by_ua Browser breakdown of connected clients\n");
        out.push_str("# TYPE nobd_client_by_ua gauge\n");
        for (ua, count) in &self.by_ua {
            out.push_str(&format!("nobd_client_by_ua{{ua=\"{}\"}} {}\n", ua, count));
        }
    }
}
