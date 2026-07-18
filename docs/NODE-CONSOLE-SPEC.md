# NODE-CONSOLE-SPEC.md — Live Node Map + Match-Aware Midpoint Placement

**Status: DRAFT spec (2026-07-17).** Grounded in a 3-agent recon of the existing
Pillar-5 stack. The headline: **~80% of this is already built.** This spec is an
*enhancement punch-list* on `hub/`, `web/network.html`, the relay, and the native
client — not a greenfield build.

---

## 0. What already exists (do NOT rebuild)

| Piece | Where | State |
|---|---|---|
| Hub service (axum, `/hub/api/*`, systemd, nginx-proxied) | `hub/src/main.rs` | LIVE |
| Node registry + record schema (geo, metrics, capacity, status, stats, **per-node `http` port**) | `hub/src/types.rs:11-76` | LIVE |
| GeoIP (lat/lng/city/country/isp on the node's public IP) | `hub/src/geo.rs:6-42` | LIVE |
| **Min-max fairness matchmaker** (`select_node` minimizes `max(P1,P2)` RTT) + tests | `hub/src/matchmaker.rs:11-53` | LIVE |
| Ping ingest + pairing (`POST /hub/api/matchmake`, FIFO queue) | `hub/src/api.rs:307-424`, `hub/src/queue.rs` | LIVE |
| Dashboard endpoints (`/dashboard/stats`, `/dashboard/nodes`) | `hub/src/api.rs:430-491` | LIVE |
| **Live map** (Leaflet, stats bar, node table, your-latency probes; 5s poll) | `web/network.html` | LIVE |
| Browser pair-midpoint (browsers report pings → hub → fairest node) | `web/js/node-router.mjs` | LIVE |
| State hand-off primitive (`migrate`/`STPU`, place/move a match onto a node) | `core/network/maplecast_ws_server.cpp:1214,2500`; `docs/STATE-HANDOFF-PLAN.md` | LIVE (gated) |
| Rich per-node status JSON (`getStatus()`: per-slot RTT/IP, relay topology, ops, live game) | `core/network/maplecast_ws_server.cpp:325-627` | LIVE (7200 mirror) |
| Node registration + heartbeat (relay → hub) | `relay/src/hub_client.rs:218-363` | LIVE |
| Relay loopback HTTP server (JSON API) | `relay/src/turn.rs:58-460` (`127.0.0.1:7202`) | LIVE |

**So the user's vision — a node map with network info where new nodes pop up live —
is mostly implemented. The gaps below are the actual work.**

---

## 1. Gaps (the ~20% = the deliverable), phased

### Phase A — Make the map *live*, and re-skin it (highest value, uses only existing data)
The map is a **5-second poll** today (`web/network.html:278,540`); nodes appear within
≤5s, not instantly. There is **no push anywhere in the hub** (no WS/SSE — grep confirms 0).

- **A1. Hub live feed.** Add `GET /hub/api/events` (SSE; `text/event-stream`) backed by a
  `tokio::sync::broadcast`. Emit `node_joined` / `node_updated` / `node_left` on:
  register (`api.rs:33-125`), heartbeat status change (`api.rs:136-174`), and the
  **stale-sweeper transitions** (`api.rs:531-566` — the natural emit hook: it already
  computes ready→stale→offline every 10s but emits nothing). SSE over WS: one-way,
  survives nginx trivially, no upgrade handshake to manage.
- **A2. Map consumes the feed.** `web/network.html` subscribes via `EventSource`; markers
  pop in/out on events. Keep the 5s poll as a reconcile/fallback (self-healing if an
  event is missed).
- **A3. Arcade Noir re-skin.** Restyle the map to the play-page identity (`web/play/css/play.css`
  tokens). Neon status pins (ready=cyan, in_match=magenta, draining=amber, stale/offline=dim),
  scanline overlay, chroma headings. Optional: draw the **current match's chosen midpoint node
  highlighted** with RTT spokes to both players.

### Phase B — Per-node localhost serve ("the binary ships a small HTTP server")
The per-node `http` port is already in the schema (`NodePorts.http`, `types.rs:16`) and
registered to the hub, but **no node binary serves anything on it** today.

- **Decision — serve from the RELAY** (recommended). `relay/src/turn.rs:117 handle_http` is
  already a loopback HTTP server (`127.0.0.1:7202`); add a static-file branch that serves the
  map page + proxies `/hub/api/*`. The relay is the per-node, hub-facing component and already
  owns registration — cleanest home. (Alt: bolt `set_http_handler` onto flycast's loopback
  control-WS `7211` — no new port/lib — but `getStatus()` lives in the mirror TU and would need
  relocating.)
- **B1.** Embed the map page in the relay binary (`include_str!`), serve on the node's `http` port (loopback).
- **B2.** Operator opens `http://localhost:<http>` (headless VPS node → SSH tunnel). Every node ships it.
- **B3.** "or built in": the native desktop client can embed the same page in a webview, or keep
  its existing egui **Servers** panel (already shows RTT bars, `native-client-tdw/src/debug.rs:93`).
  Optional third surface — same data feed.

### Phase C — Native players into the midpoint pool
Browser pairs already get fair-node placement. **Native-client players don't** — the native
client probes RTT (UDP `:7100`) but never reports it to the hub, and drops each node's `node_id`.

- **C1.** Add `node_id: Option<String>` to `ServerEntry` (`native-client-tdw/src/debug.rs:10-16`);
  populate from the hub fetch (`main.rs:681` — `node_id` is already in the `/nodes` payload, just discarded).
- **C2.** After each probe sweep (`main.rs:768-777`), `POST` `{node_id → rtt_ms}` to
  `/hub/api/matchmake` (`MatchmakeRequest`, `types.rs:153-158`). **No hub change** — `select_node` eats it.
- **C3.** Metric consistency: native RTT = 1 UDP sample; browser RTT = 5-sample WS ping/pong.
  Mixing them in one `select_node` compares two transports. Standardize (native does N samples +
  p95) or tag the source and normalize. Required for a *fair* cross-transport midpoint.

### Phase D — Hub-driven placement (optional, powerful)
Today `select_node` returns a node to the **client**, which connects. To place/migrate a
**running** match onto the midpoint node, wire the hub's pick to the existing `migrate`/`STPU`
primitive (`maplecast_ws_server.cpp:1214`): the hub instructs the source node to migrate to the
winner. Enables mid-match re-optimization when a better node appears.

### Phase E — Honesty + robustness (do alongside A/B)
- **E1. Real health.** Node `status` is derived *only* from the relay↔flycast socket
  (`upstream_connected`, `hub_client.rs:285-289`) — a node reports `ready` while flycast is
  internally wedged. Add a flycast→relay health beat (frames flowing + SH4 alive); fix
  `total_matches` hardcoded `0` (`hub_client.rs:299`).
- **E2. Persistence.** Registry is an in-memory `HashMap`, wiped on hub restart
  (`types.rs:222-234`). `hub/src/schema.surql` (SurrealDB) is defined but unwired — wire it.
- **E3. Privacy (design decision).** GeoIP resolves the node's **public IP** → ISP + city on a
  *shared* map. Showing every operator's approximate location + ISP is an exposure. Decide the
  public granularity: coarsen `NodePublic` to city/region only, add an opt-out, or jitter pins.

---

## 2. MVP / smallest shippable slice
**Phase A alone** (SSE live feed + Arcade Noir re-skin) is a visible, shippable win on 100%
existing data — the "nodes pop up live on a map" the user asked for. Ship A, then B (per-node
serve), then C (native into the pool), then D/E.

## 3. Out of scope here — "user machines as nodes" (the ambitious version)
Turning home machines into public nodes adds three hard problems tracked separately
(`project-distributed-midpoint-nodes` memory): **NAT/reachability** (home nodes behind NAT;
browsers need WSS+domain so can't be home-hosted), **trust/anti-cheat** (peer-hosted
authoritative sim → deterministic-replay audit, since SH4 is byte-deterministic), and
**capacity** (1 GB nodes can't survive a match-load). Phases A–E all apply to the trusted DC
fleet first and are worth shipping regardless.
