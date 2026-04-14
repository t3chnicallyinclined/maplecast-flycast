// ============================================================================
// NODE-ROUTER.MJS — Distributed node discovery + ping probing
//
// Discovers available game server nodes from the MapleCast hub, probes
// each with WebSocket ping/pong (the SAME protocol as gameplay), and
// reports results for matchmaking.
//
// The hub is NEVER in the gameplay hot path. Once a match is assigned,
// the browser talks directly to the selected node.
//
// OVERKILL IS NECESSARY.
// ============================================================================

// ── Exported state ──────────────────────────────────────────────────
export const nodeState = {
  /** @type {Array<{node_id:string, name:string, public_host:string, ports:object, tls:boolean, relay_url:string, geo:object|null, status:string}>} */
  nodes: [],

  /** @type {Array<{node_id:string, avg_rtt_ms:number, p95_rtt_ms:number, samples:number[]}>} */
  pingResults: [],

  /** @type {{node_id:string, relay_url:string, control_url:string, audio_url:string}|null} */
  assignedNode: null,

  /** True while probing is in progress */
  probing: false,

  /** Hub API base URL (null = hub disabled, use origin server) */
  hubUrl: null,
};

// Auto-detect hub URL — production uses /hub/api on same origin
const isProd = location.port === '' || location.port === '80' || location.port === '443';
if (isProd) {
  nodeState.hubUrl = `${location.protocol}//${location.hostname}/hub/api`;
}

// ── Discover nodes from hub ─────────────────────────────────────────

/**
 * Fetch the list of nearby nodes from the hub.
 * Uses geographic pre-filtering (hub does GeoIP on our request IP).
 * @param {number} [limit=5] Max nodes to return
 * @returns {Promise<Array>} Node list
 */
export async function discoverNodes(limit = 5) {
  if (!nodeState.hubUrl) return [];

  try {
    const resp = await fetch(`${nodeState.hubUrl}/nodes/nearby?limit=${limit}`);
    if (!resp.ok) {
      console.warn('[input-server] Hub /nodes/nearby failed:', resp.status);
      return [];
    }
    const data = await resp.json();
    nodeState.nodes = data.nodes || [];
    console.log(`[input-server] Discovered ${nodeState.nodes.length} input servers`);
    return nodeState.nodes;
  } catch (e) {
    console.warn('[input-server] Hub unreachable:', e.message);
    return [];
  }
}

// ── Ping probe via WebSocket ────────────────────────────────────────

/**
 * Probe a single node's relay WebSocket with 5 ping/pong round-trips.
 * Uses the SAME {"type":"ping","t":...} protocol the relay already handles
 * (fanout.rs line ~538). This measures the TRUE gameplay path.
 *
 * @param {{relay_url:string, node_id:string}} node
 * @returns {Promise<{node_id:string, avg_rtt_ms:number, p95_rtt_ms:number, samples:number[]}>}
 */
function probeNode(node) {
  return new Promise((resolve) => {
    const PROBE_COUNT = 5;
    const PROBE_INTERVAL_MS = 200;
    const TIMEOUT_MS = 3000;
    const samples = [];
    let pending = null;
    let probeIdx = 0;
    let ws;

    const cleanup = () => {
      if (ws && ws.readyState <= WebSocket.OPEN) ws.close();
    };

    const timeout = setTimeout(() => {
      console.warn(`[input-server] Probe timeout for ${node.node_id}`);
      cleanup();
      resolve(buildResult());
    }, TIMEOUT_MS);

    function buildResult() {
      clearTimeout(timeout);
      // Discard first sample (TCP handshake cold start)
      const warm = samples.length > 1 ? samples.slice(1) : samples;
      const avg = warm.length > 0
        ? warm.reduce((a, b) => a + b, 0) / warm.length
        : Infinity;
      const sorted = [...warm].sort((a, b) => a - b);
      const p95 = sorted.length > 0
        ? sorted[Math.floor(sorted.length * 0.95)]
        : Infinity;
      return { node_id: node.node_id, avg_rtt_ms: avg, p95_rtt_ms: p95, samples: warm };
    }

    try {
      ws = new WebSocket(node.relay_url);
    } catch (e) {
      clearTimeout(timeout);
      resolve({ node_id: node.node_id, avg_rtt_ms: Infinity, p95_rtt_ms: Infinity, samples: [] });
      return;
    }

    ws.onopen = () => {
      sendProbe();
    };

    ws.onmessage = (ev) => {
      if (typeof ev.data !== 'string') return; // skip binary TA frames
      try {
        const msg = JSON.parse(ev.data);
        if (msg.type === 'pong' && pending !== null) {
          const rtt = performance.now() - pending;
          samples.push(rtt);
          pending = null;
          probeIdx++;
          if (probeIdx < PROBE_COUNT) {
            setTimeout(sendProbe, PROBE_INTERVAL_MS);
          } else {
            cleanup();
            resolve(buildResult());
          }
        }
      } catch { /* ignore non-JSON */ }
    };

    ws.onerror = () => {
      cleanup();
      resolve(buildResult());
    };

    ws.onclose = () => {
      if (probeIdx < PROBE_COUNT) {
        resolve(buildResult());
      }
    };

    function sendProbe() {
      if (ws.readyState !== WebSocket.OPEN) return;
      pending = performance.now();
      ws.send(JSON.stringify({ type: 'ping', t: pending }));
    }
  });
}

/**
 * Probe all discovered nodes in parallel.
 * @param {Array} [nodes] Nodes to probe (defaults to nodeState.nodes)
 * @returns {Promise<Array>} Ping results sorted by avg_rtt_ms
 */
export async function probeNodes(nodes) {
  const targets = nodes || nodeState.nodes;
  if (targets.length === 0) return [];

  nodeState.probing = true;
  console.log(`[input-server] Probing ${targets.length} input servers...`);

  const results = await Promise.all(targets.map(n => probeNode(n)));

  // Sort by average RTT (best first)
  results.sort((a, b) => a.avg_rtt_ms - b.avg_rtt_ms);
  nodeState.pingResults = results;
  nodeState.probing = false;

  for (const r of results) {
    console.log(
      `[input-server]   ${r.node_id}: ${r.avg_rtt_ms.toFixed(1)}ms avg (${r.samples.length} samples)`
    );
  }

  return results;
}

// ── Report pings to hub for matchmaking ─────────────────────────────

/**
 * Submit ping results to the hub's matchmake endpoint.
 * @param {string} playerId
 * @param {string} sessionId
 * @returns {Promise<{status:string, node_id?:string, node_urls?:object}>}
 */
export async function reportPings(playerId, sessionId) {
  if (!nodeState.hubUrl) return { status: 'disabled' };
  if (nodeState.pingResults.length === 0) return { status: 'no_results' };

  try {
    const resp = await fetch(`${nodeState.hubUrl}/matchmake`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        player_id: playerId,
        session_id: sessionId,
        ping_results: nodeState.pingResults.map(r => ({
          node_id: r.node_id,
          avg_rtt_ms: r.avg_rtt_ms,
          p95_rtt_ms: r.p95_rtt_ms,
        })),
      }),
    });

    if (!resp.ok) {
      console.warn('[input-server] Matchmake report failed:', resp.status);
      return { status: 'error' };
    }

    const data = await resp.json();
    console.log('[input-server] Matchmake response:', data.status);
    return data;
  } catch (e) {
    console.warn('[input-server] Matchmake report error:', e.message);
    return { status: 'error' };
  }
}

// ── Assign a node (called when matchmaker selects one) ──────────────

/**
 * Set the assigned node for this browser session. All subsequent WS
 * connections (relay, control, audio) will target this node instead of
 * the page origin.
 *
 * @param {{node_id:string, relay_url:string, control_url:string, audio_url:string}|null} node
 */
export function assignNode(node) {
  nodeState.assignedNode = node;
  if (node) {
    console.log(`[input-server] Assigned to input server ${node.node_id} → ${node.relay_url}`);
  } else {
    console.log('[input-server] Cleared input server assignment — using origin server');
  }
}

/**
 * Clear the node assignment (after match ends, disconnect, etc.)
 */
export function clearAssignment() {
  nodeState.assignedNode = null;
  nodeState.pingResults = [];
  console.log('[input-server] Assignment cleared');
}

// ── Auto-discover on load (non-blocking) ────────────────────────────

if (nodeState.hubUrl) {
  // Fire-and-forget: discover nodes on page load so they're ready
  // when the player clicks "I GOT NEXT"
  discoverNodes().catch(() => {});
}
