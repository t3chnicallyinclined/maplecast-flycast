# MapleCast Matchmaking

A standalone matchmaking experience that pairs strangers and routes
them into MVC2 matches via the existing input-server network. Sits
above [`king.html`](../web/king.html) (which is the per-cabinet lobby)
and the [hub](../hub/) (which knows about servers but not players).

This doc captures the full Phase 1 → 8 design so we don't lose
context across deploys. Phases land in order; each is independently
useful so we can ship and validate before committing to the next.

---

## Topology

```
                  ┌─────────────────────────────────────┐
                  │         play.html (NEW)             │
                  │ "Find Match" → poll → redirect      │
                  └─────────────┬───────────────────────┘
                                │  /hub/api/queue/{join,status,leave}
                                ▼
                  ┌─────────────────────────────────────┐
                  │         hub (Rust)                  │
                  │  - existing matchmake/select        │
                  │  - NEW queue state machine          │
                  │  - 1Hz pair-and-notify task         │
                  └─────────────┬───────────────────────┘
                                │ matched { server, slot, token }
                                ▼
                  ┌─────────────────────────────────────┐
                  │  user dispatched to one of:         │
                  │   - king.html?slot=N&token=...      │
                  │     (browser path)                  │
                  │   - maplecast://join?host=...&slot= │
                  │     (native client via protocol)    │
                  └─────────────────────────────────────┘
```

Existing pieces this builds on:
- **Hub `select_node`** (min-max RTT fairness, `hub/src/matchmaker.rs`)
- **Server registry** (input servers heartbeat to hub)
- **king.html** (per-server arcade lobby)
- **Native client + `maplecast://` protocol handler**
- **Telemetry stack** (Tele-0.x — drives the live dashboard counts)

---

## Phase 1 — bare matchmaker MVP

**Goal:** name → click → matched → playing. No frills.

### Hub API additions

```
POST /hub/api/queue/join
  body: { name: string, prefer_region?: string, client_rtts?: { node_id: ms } }
  resp: { token: uuid, position: int, eta_s: int }

GET /hub/api/queue/status?token=<uuid>
  resp:
    { state: "queueing", position: 3, queue_size: 5 }
    { state: "matched", server: { name, host, port, region }, slot: 0|1, partner: "name" }
    { state: "expired" }   // token unknown / user took too long to dispatch

POST /hub/api/queue/leave
  body: { token }
  resp: { ok: true }
```

### Hub state machine

```rust
struct QueueEntry {
    token: Uuid,
    name: String,
    joined_at: Instant,
    rtts: HashMap<NodeId, u32>,        // optional, from client probe
    state: QueueState,
}
enum QueueState {
    Queueing,
    Matched { server: NodeId, slot: u8, partner: String },
    Cancelled,
    Expired,                           // never claimed within 60s of being matched
}
```

A 1Hz task pairs the head two queueing entries:
1. Pop two entries (head FIFO).
2. Call existing `select_node()` with their probe RTTs (or fall back
   to a default server if neither sent probes).
3. Mark both `Matched { server, slot=0, partner=other.name }` /
   `slot=1`.
4. The entries stay in the registry for ~60s so polls get the
   `Matched` answer; after that they go `Expired` and are GC'd.

### Web `play.html`

Single page. Form fields:
- `name` — text
- (optional) probe button — runs UDP probes to discover RTT to each
  registered input server, sends results in `client_rtts`. For phase
  1 we skip this and use last-known telemetry on the hub side.

Status box renders the current `state`:
- `idle` — show the form
- `queueing` — show position + spinner + cancel button
- `matched` — show partner name + server + two buttons:
  "Play in browser" → `https://<server.host>/king.html?slot=N&token=<uuid>`
  "Launch desktop client" → `maplecast://join?host=<server.host>&port=<server.port>&slot=N&token=<uuid>`

Polls `/hub/api/queue/status` every 1s while `queueing`.

### king.html ingest

When loaded with `?slot=N&token=<uuid>`:
1. Validates the token with the hub (`/hub/api/queue/status`) — if
   matched and slot matches, auto-join with that slot pre-claimed.
2. If token expired / wrong server, falls back to normal "I GOT NEXT"
   behaviour.

### Native client ingest

`winmain.cpp`'s existing `maplecast://` parser already handles
`host` + `port` + `name`. Phase 1 adds `slot` + `token`:
- `MAPLECAST_PRECLAIM_SLOT=N` env var, picked up by the input server
  at registration time.
- `MAPLECAST_QUEUE_TOKEN=<uuid>` env var, sent in the registration
  payload so the server can verify with the hub.

### What MVP does NOT do
- No login / accounts (cookie-anonymous only)
- No ELO / ranked / casual split
- No friend lists
- No spectate / live-match feed
- No recent-match history
- No region picker (auto)
- No custom lobbies
- No tournament

These come in later phases.

---

## Phase 2 — first-time-user flow

**Goal:** anyone landing on `play.html` can play within ~60 seconds
even if they've never installed anything.

### Detection

play.html attempts to detect whether the `maplecast://` protocol
handler is registered:
1. Render an invisible `<a href="maplecast://probe">` and trigger it.
2. Within ~1.5s, listen for `window.blur` (OS hand-off succeeded).
3. If no blur fires, assume handler not registered.

(Imperfect — Chrome and Firefox handle this differently — but good
enough as a hint. Clear enough for users.)

### Install card

If not registered:
- Card appears on play.html with:
  - "Don't have the desktop client yet?"
  - One-click download (GitHub Releases asset, latest)
  - Brief install steps:
    1. Unzip
    2. Run `register-protocol-handler.ps1` (link to in-repo script,
       included in the release zip)
    3. Refresh this page
- Alternative: "Just play in the browser" button stays prominent —
  no friction for casual one-shot players.

### Match-found choice

If client IS registered:
- "Launch Desktop" / "Play in Browser" buttons both rendered, with
  Desktop labelled "lower latency".
- Clicking Desktop fires the `maplecast://` URL (existing).
- Clicking Browser does the king.html redirect.

If NOT registered:
- Only "Play in Browser" rendered (since native isn't an option).
- Background card still nudges install for next time.

---

## Phase 3 — identity (anonymous-but-persistent)

- localStorage `client_id` (UUID, generated on first visit).
- Optional name field; sticks across sessions.
- Hub tracks `(client_id) → { matches_played, wins, losses, last_seen }`.
- No password, no email, no signup wall — keeps fighting-game culture's
  low-friction tradition. People who want a stable handle pick a name;
  everyone else stays "Player 1234".
- Surface: small pill in play.html top-right with current name + W/L.

Backing storage: SurrealDB (already deployed on prod for skin DB).
New table `players` keyed by `client_id`.

---

## Phase 4 — recent matches feed + spectate

Depends on Tele-0.9 being re-routed (currently disabled). Once
match-end events flow into a persistent stream (likely NATS or a
hub-side ring buffer), play.html shows:
- Last 10 matches: characters / winner / duration / server
- "Watch replay" button → opens replay-play.ps1 path or in-browser
  WASM viewer (we already have the foundation)
- "Spectate live" button → opens spectator stream
  (`MAPLECAST_SPECTATE=1`)

Hub additions:
```
GET /hub/api/recent-matches?limit=10  → array of match summaries
GET /hub/api/live-matches             → list of in-progress matches
```

---

## Phase 5 — server browser + RTT badges

Above the queue panel: a list of registered input servers, each row:
- Server name, region, current player count (0/2 or 1/2)
- Live RTT badge from the user's browser (via the hub's existing
  client probe API + a quick UDP echo)
- "Queue here only" filter checkbox — narrows queue to a specific
  server vs auto-pick

Auto-pick (default) uses the existing min-max algorithm. Manual
filter overrides.

---

## Phase 6 — ranked + ELO

- New tab in play.html: Casual / Ranked.
- Ranked queue uses ELO matchmaking instead of pure FIFO: pair within
  ±N ELO, widening over time.
- Hub computes ELO updates on match-end events (Tele-0.9 must be live
  by this phase).
- Per-character ELO optional (huge appeal for FGC players who main
  one character).
- Profile shows current rank tier (Bronze / Silver / Gold / etc.)
  with progress bar to next tier.

---

## Phase 7 — custom lobbies

- "Create lobby" button on play.html.
- Generates a 6-char invite code; shareable URL
  `https://play.nobd.net/play.html?lobby=ABC123`.
- Joining a lobby skips the queue and routes both players to a
  reserved slot on a chosen server.
- Use cases: friend matches, training rooms, run-back sets without
  re-queueing, tournament pools.

---

## Phase 8 — tournament mode

- Brackets (single elim, double elim, round robin).
- Hub manages bracket state, auto-routes winners to next round's
  match.
- Tournament organizer page (admin token gated) with check-in /
  start / pause controls.
- Stream-friendly: spectator overlay shows bracket on screen,
  current match info, commentator names.
- Long-tail; ship only after the casual / ranked flows have real
  usage.

---

## Cross-cutting design notes

### Anti-cheat / abuse
- Single connection per `client_id` (hub-enforced).
- Rate-limit join/leave to once per 5s per IP.
- Auto-cancel queue if a player leaves their browser tab > 30s
  without a heartbeat.
- Match-end winner inferred server-side, not reported by client.
  (Already the case via the in_match RAM probe.)

### Token lifecycle
- Tokens are valid until matched + 60s after, then GC'd.
- After dispatch (player loads king.html / fires maplecast://), the
  destination server reports back to the hub "claimed" so the hub
  flips the entry to `Dispatched`. After 60s without claim, hub
  flips the entry to `Expired` and re-queues the partner.

### Region split
- Phase 1: single global queue, hub picks server by RTT.
- Phase 5+: optional region filter ("US-East only").
- For very-popular regions, may need per-region queues to keep
  pair-up speed reasonable.

### Failure modes
- Hub down: `play.html` should fail clean — show "matchmaking
  unavailable, try again in a moment."
- Server matched but unreachable: hub's `Dispatched`-timeout
  re-queues the partner.
- Player disconnects mid-match: existing in-game flow handles it
  (other player wins by default, queue advances).

### Telemetry hooks
The matchmaking flow itself is a great telemetry surface:
- Time-to-match histogram (queue size 1 → 2 latency)
- Match completion rate (matched but never connected)
- Per-server fan-in (which servers get the most queue assignments)
- Top players (matches, win rate, longest streak)

These eventually feed into the same `/telemetry.html` dashboard.

---

## Out of scope (forever, probably)

- Voice chat — not the architecture's job; users use Discord / etc.
- In-game chat during match — fighting games are short, no time to type.
- Cosmetics shop / monetization — explicitly not the project's goal.
- Cross-game support — this stack is MVC2-shaped from top to bottom.

---

## Build order (concrete next moves)

1. **Hub**: add `queue` module with state machine + 3 endpoints.
2. **Hub**: 1Hz pair-and-notify task.
3. **Web**: `play.html` with name + find-match + status polling.
4. **Web**: install detection + card.
5. **Web**: king.html accepts `?slot=N&token=...`.
6. **Native**: winmain.cpp parses `slot` + `token` params; passes
   through to the input server registration.
7. **Hub**: validates the token on registration, marks `Dispatched`.

Everything after Phase 2 is open queue.
