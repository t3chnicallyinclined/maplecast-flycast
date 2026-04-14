use crate::types::{Node, NodeUrls, PingResult};

/// Min-max fairness node selection for a 2-player fighting game.
///
/// For each candidate node, compute max(P1_rtt, P2_rtt) — the worst-case
/// latency either player will experience. Select the node that minimizes
/// this maximum. This ensures fairness: the disadvantaged player's latency
/// is as good as possible.
///
/// Returns (node_id, NodeUrls, worst_rtt_ms) or None if no valid candidate.
pub fn select_node(
    p1_pings: &[PingResult],
    p2_pings: &[PingResult],
    nodes: &[&Node],
) -> Option<(String, NodeUrls, f64)> {
    let mut best: Option<(String, NodeUrls, f64)> = None;

    for node in nodes {
        // Only consider nodes that are ready for matches
        if node.status != "ready" {
            continue;
        }

        // Both players must have pinged this node
        let p1 = p1_pings.iter().find(|p| p.node_id == node.node_id);
        let p2 = p2_pings.iter().find(|p| p.node_id == node.node_id);

        let (p1_rtt, p2_rtt) = match (p1, p2) {
            (Some(a), Some(b)) => (a.avg_rtt_ms, b.avg_rtt_ms),
            _ => continue,
        };

        let worst = p1_rtt.max(p2_rtt);

        let dominated = match &best {
            Some((_, _, best_worst)) => worst >= *best_worst,
            None => false,
        };

        if !dominated {
            let urls = NodeUrls {
                relay_url: node.relay_url(),
                control_url: node.control_url(),
                audio_url: node.audio_url(),
                input_udp_host: node.public_host.clone(),
                input_udp_port: node.ports.input_udp,
            };
            best = Some((node.node_id.clone(), urls, worst));
        }
    }

    best
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::*;
    use chrono::Utc;

    fn make_node(id: &str, host: &str) -> Node {
        Node {
            node_id: id.to_string(),
            operator_name: "test".to_string(),
            name: id.to_string(),
            region: "us-east".to_string(),
            public_host: host.to_string(),
            ports: NodePorts {
                relay_ws: 7201,
                input_udp: 7100,
                control_ws: 7210,
                http: 7202,
            },
            tls: true,
            capacity: NodeCapacity {
                max_matches: 1,
                max_spectators: 500,
            },
            rom_hash: "abc".to_string(),
            version: "0.1.0".to_string(),
            status: "ready".to_string(),
            geo: None,
            metrics: None,
            stats: NodeStats {
                total_matches: 0,
                total_frames: 0,
                uptime_s: 0,
            },
            last_heartbeat: Utc::now(),
            registered_at: Utc::now(),
            stale_count: 0,
        }
    }

    #[test]
    fn picks_fairest_node() {
        let node_a = make_node("a", "chi1.nobd.net");
        let node_b = make_node("b", "nyc1.nobd.net");
        let nodes: Vec<&Node> = vec![&node_a, &node_b];

        // P1: 10ms to A, 50ms to B
        // P2: 40ms to A, 20ms to B
        // A: max(10,40) = 40ms, B: max(50,20) = 50ms → pick A
        let p1 = vec![
            PingResult { node_id: "a".into(), avg_rtt_ms: 10.0, p95_rtt_ms: 12.0 },
            PingResult { node_id: "b".into(), avg_rtt_ms: 50.0, p95_rtt_ms: 55.0 },
        ];
        let p2 = vec![
            PingResult { node_id: "a".into(), avg_rtt_ms: 40.0, p95_rtt_ms: 44.0 },
            PingResult { node_id: "b".into(), avg_rtt_ms: 20.0, p95_rtt_ms: 22.0 },
        ];

        let result = select_node(&p1, &p2, &nodes);
        assert!(result.is_some());
        let (id, _, worst) = result.unwrap();
        assert_eq!(id, "a");
        assert!((worst - 40.0).abs() < 0.01);
    }

    #[test]
    fn skips_non_ready_nodes() {
        let mut node_a = make_node("a", "chi1.nobd.net");
        node_a.status = "in_match".to_string();
        let node_b = make_node("b", "nyc1.nobd.net");
        let nodes: Vec<&Node> = vec![&node_a, &node_b];

        let p1 = vec![
            PingResult { node_id: "a".into(), avg_rtt_ms: 5.0, p95_rtt_ms: 6.0 },
            PingResult { node_id: "b".into(), avg_rtt_ms: 30.0, p95_rtt_ms: 35.0 },
        ];
        let p2 = vec![
            PingResult { node_id: "a".into(), avg_rtt_ms: 5.0, p95_rtt_ms: 6.0 },
            PingResult { node_id: "b".into(), avg_rtt_ms: 25.0, p95_rtt_ms: 28.0 },
        ];

        let result = select_node(&p1, &p2, &nodes);
        assert!(result.is_some());
        let (id, _, _) = result.unwrap();
        assert_eq!(id, "b"); // A is in_match, so B wins despite higher RTT
    }

    #[test]
    fn returns_none_when_no_overlap() {
        let node_a = make_node("a", "chi1.nobd.net");
        let nodes: Vec<&Node> = vec![&node_a];

        let p1 = vec![PingResult { node_id: "a".into(), avg_rtt_ms: 10.0, p95_rtt_ms: 12.0 }];
        let p2 = vec![PingResult { node_id: "b".into(), avg_rtt_ms: 20.0, p95_rtt_ms: 22.0 }]; // P2 didn't ping A

        let result = select_node(&p1, &p2, &nodes);
        assert!(result.is_none());
    }
}
