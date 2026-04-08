# WORKSTREAM: /overlord — Web Admin UI for the Headless Cab

> **One-shot implementation guide.** Read top-to-bottom, execute in order. Don't skip phases. Every file path, endpoint, byte, and gotcha is captured so an agent can land a working admin panel at `https://nobd.net/overlord` in a single pass.

---

## 0. Context You Need Before Touching Code

### What this is

A web admin panel at `https://nobd.net/overlord` that lets an authorized operator:

- **See the live cab state** — is flycast running, current fps, memory, client count, uptime, current savestate slot
- **Manage savestates** — list, download, upload, load a slot into the running emulator **without a restart**, save the current running state to a slot
- **Edit `emu.cfg`** — change `SavestateSlot`, `AutoLoadState`, renderer options, etc., with a diff view and validation
- **Restart the flycast service** — one-click `systemctl restart maplecast-headless` for when config changes need a cold reload
- **Tail logs live** — `journalctl -u maplecast-headless -f` streamed over SSE, color-coded, searchable
- **Preview the game live** — an embedded instance of the existing `king.html` WASM renderer pointed at the same upstream, so you can see what the cab is rendering while you mess with it

The name is `overlord` because it's the admin overlord of the cab. `/admin` was boring. `/control` was taken. `/overlord` was the vibe.

### What this is NOT

- **Not a gameplay interface.** The admin can VIEW the game via the embedded preview but cannot take control of player slots from the admin UI. Slot assignment still goes through the normal NOBD/queue flow. V2 can add "admin-only debug input."
- **Not a replacement for SSH.** It sits alongside SSH access. If the admin UI breaks, you fall back to SSH + systemctl.
- **Not multi-tenant.** V1 is single-cab (this VPS, this flycast instance). No multi-cabinet routing.
- **Not a ROM manager.** Swapping the running ROM is a separate concern (involves savestate namespace changes, config changes, service restart). V1 assumes the ROM is fixed at `/opt/maplecast/roms/mvc2.gdi`. V2 can add ROM swap.
- **Not a flycast rewrite.** We add a **tiny** control WebSocket surface to flycast for hot savestate save/load, and everything else lives in the existing relay and the web frontend.

### The architecture you're extending

```
┌──────────────────────────────────────────────────────────────┐
│ VPS (66.55.128.93)                                           │
│                                                               │
│  ┌──────────────────────┐      ┌──────────────────────┐     │
│  │ maplecast-headless    │      │ maplecast-relay       │     │
│  │ (flycast, compile-out)│      │ (Rust)                │     │
│  │                       │      │                       │     │
│  │ Ports:                │      │ Existing:             │     │
│  │  127.0.0.1:7210/tcp   │ ◄────│  --ws-upstream        │     │
│  │     TA mirror WS      │  WS  │   ws://127.0.0.1:7210 │     │
│  │  0.0.0.0:7100/udp     │      │  0.0.0.0:7201          │     │
│  │     input server      │      │   downstream WS        │     │
│  │                       │      │  127.0.0.1:7202        │     │
│  │ NEW (this workstream):│      │   HTTP /metrics /api/* │     │
│  │  127.0.0.1:7211/tcp   │ ◄────│                        │     │
│  │     control WS        │      │ NEW: /overlord/api/*   │     │
│  │     (in-process       │      │   admin HTTP routes    │     │
│  │      savestate cmds)  │      │                        │     │
│  └──────────────────────┘      └────────┬─────────────┘     │
│                                           │                    │
│                                  nginx /overlord/*            │
│                                  nginx /overlord/api/*        │
│                                           │                    │
└───────────────────────────────────────────┼────────────────────┘
                                            │
                                            │  HTTPS (wss+https)
                                            ▼
                                 ┌────────────────────┐
                                 │  Browser (operator) │
                                 │  /overlord/         │
                                 │                     │
                                 │  Admin dashboard SPA│
                                 │  + embedded         │
                                 │    king.html iframe │
                                 │    (live preview)   │
                                 └────────────────────┘
```

### The three existing subsystems you're plugging into

#### 1. The relay's hand-rolled HTTP dispatcher

**File:** [relay/src/turn.rs:81-220](../relay/src/turn.rs#L81) `handle_http()`

Raw TCP accept, read request, parse by `first_line.starts_with()`, dispatch to handler, write response. No framework. Existing routes:

- `GET /turn-cred` → TURN ICE credentials
- `GET /metrics` → Prometheus metrics
- `GET /health` → health JSON
- `POST /api/telemetry` → browser telemetry ingest
- `POST /api/register` → SurrealDB player registration
- `POST /api/signin` → SurrealDB player login, mints a JWT
- `POST /api/leave` → JWT-gated leave handler

Adding `/overlord/*` routes is a drop-in extension to this dispatcher. No new listener, no new crate dependencies for the Rust side (we already have `tokio`, `reqwest`, `serde_json`, `argon2`).

#### 2. SurrealDB auth + JWT

**File:** [relay/src/auth_api.rs](../relay/src/auth_api.rs)

`handle_register()` and `handle_signin()` already:
- Hash passwords with argon2
- Write to the `player` table in SurrealDB (`maplecast/arcade`)
- Mint a scoped JWT via SurrealDB's `/signin` endpoint, stored client-side as `nobd_token`
- Return it to the browser in the signin response

The admin panel **reuses this login flow verbatim** — operator signs in with their existing nobd.net account at `/overlord/login`. The token contains the SurrealDB scope which we check for a new `admin: bool` field on the player record.

The schema lives at [web/schema.surql:32-72](../web/schema.surql#L32). We add one field:

```sql
DEFINE FIELD admin ON player TYPE bool DEFAULT false;
```

The operator (you) runs one SQL statement on the VPS to set `admin = true` on your own player record. Everyone else's `admin` defaults to false and can't reach `/overlord/*` write endpoints.

#### 3. The existing king.html WASM mirror renderer

**File:** [web/king.html](../web/king.html) + [web/js/](../web/js/)

Already a fully working WASM instance that connects to `wss://nobd.net/ws`, decodes the ZCST-compressed TA mirror stream, and renders MVC2 in a browser. We **embed it** in the admin page via an iframe. Zero new renderer work.

The iframe gets the exact same stream the public spectators see — which is ideal, because the admin wants to see what the public sees.

### Why put admin surface in the relay instead of a new service

The relay already:
- Has an HTTP listener at `:7202`
- Has a full SurrealDB client with auth + JWT minting
- Runs on the same box as flycast (can read/write flycast's config + savestate files directly)
- Runs as a privileged-enough user to `systemctl restart maplecast-headless` (with a polkit rule or a setuid helper — see phase B2)
- Is already a trusted process in the streaming path

Adding a new service would duplicate all of this. The relay growing an admin surface is the minimum-disruption choice.

---

## 1. Success Criteria — How You Know You're Done

Phase-gated. No "mostly works."

| Gate | Acceptance test |
|------|-----------------|
| **G1** | Flycast (headless compile-out) exposes a second WebSocket on `127.0.0.1:7211` ("control WS"). Dev build sends `{"cmd":"ping"}` over the control WS and receives `{"ok":true,"pong":true}`. Existing TA mirror stream on `:7210` is unchanged. |
| **G2** | Flycast control WS accepts `{"cmd":"savestate_save","slot":3}` and `{"cmd":"savestate_load","slot":1}` messages. `save` writes the current running state to the correct file on disk (matching flycast's native `dc_savestate()` path), `load` calls `dc_loadstate()` on the main thread and the emulator state changes in-place without a service restart. Verified by: save slot 3 → change slot 1 → load slot 3 → state matches what was captured. |
| **G3** | Rust relay dispatches `GET /overlord/` to a static file handler serving `/var/www/maplecast/overlord/index.html`. nginx routes `https://nobd.net/overlord/` to the relay unchanged. curl from external box returns HTML. |
| **G4** | Relay has `POST /overlord/api/signin` reusing the existing `auth_api::handle_signin()` flow but also checking the `admin` bool on the player record. Returns 403 if not admin, 200 + JWT if admin. Tested: non-admin account gets 403, admin account gets 200. |
| **G5** | Relay has `GET /overlord/api/status` returning JSON with flycast pid, uptime, memory, current savestate slot, current fps (scraped from flycast's own `/metrics` or the relay's fanout counters), relay client count. JWT gated (admin only). |
| **G6** | Relay has `GET /overlord/api/savestates` listing all `mvc2_*.state` files in `/opt/maplecast/.local/share/flycast/` with size + mtime. JWT gated. |
| **G7** | Relay has `POST /overlord/api/savestates/load` with `{"slot":N}` that proxies through the flycast control WS (G2) and returns the result. Hot-loads the state into the running emulator. Browser verification: admin UI shows the game state change in the embedded preview within ~1 second. |
| **G8** | Relay has `POST /overlord/api/savestates/save` with `{"slot":N}` that proxies through the control WS. Creates a new file in the savestates dir. Refreshing the savestate list shows the new file. |
| **G9** | Relay has `POST /overlord/api/savestates/upload` accepting multipart/form-data with a `.state` file. Writes it to disk with the correct ownership and refreshes the list. File visible in G6's list. |
| **G10** | Relay has `GET /overlord/api/config` returning the contents of `/opt/maplecast/.config/flycast/emu.cfg` as text. `POST /overlord/api/config` accepts new text, validates it's parseable (no CRLF mangling, no truncation), writes it atomically, returns the new content. |
| **G11** | Relay has `POST /overlord/api/service/restart` that shells out to `systemctl restart maplecast-headless`. Returns 200 on success, 500 with stderr on failure. |
| **G12** | Relay has `GET /overlord/api/logs/tail?n=200` returning the last 200 lines from `journalctl -u maplecast-headless`. |
| **G13** | Relay has `GET /overlord/api/logs/stream` as a Server-Sent Events endpoint streaming new log lines as they arrive. Tested: `curl -N` keeps the connection open and prints lines as flycast emits them. |
| **G14** | `web/overlord/index.html` loads in a browser after logging in. Shows: status dashboard (running/stopped pill, fps, mem, uptime, client count), savestate slot table (list + download + load + save + upload), emu.cfg editor (textarea + save button), log tail (virtual-scroll terminal view), restart button, embedded king.html preview iframe. |
| **G15** | End-to-end cabaret: log in to `/overlord` → click "Save to Slot 5" → click "Upload" on a `.state` file from disk → click "Load Slot 5" → see the emulator state change in the embedded preview → edit `emu.cfg` to set `SavestateSlot = 5` → save → click "Restart service" → see status pill cycle through stopped → starting → running → see the new savestate auto-load in the preview. Full flow in under 30 seconds, no SSH required. |

**Until G15 is green, this workstream is not done.** G1–G14 is infrastructure; G15 is the proof you actually built the thing.

---

## 2. Prerequisites — What Must Already Exist

Verify before starting. If any is false, fix that first.

- [ ] `headless-server` branch merged into `wasm-determinism`. Commits `4d7dbac69`, `d93abcac1`, `93ceeff9d`, `2d9c0de92`, `194e557ef`, `d9df25465` present. `git log --oneline wasm-determinism | head -10` shows all of them.
- [ ] `maplecast-headless.service` running on the VPS. `ssh root@66.55.128.93 'systemctl is-active maplecast-headless'` returns `active`.
- [ ] `maplecast-relay.service` running and pointing at `ws://127.0.0.1:7210`. `ssh root@66.55.128.93 'systemctl cat maplecast-relay | grep ExecStart'` shows the `--ws-upstream ws://127.0.0.1:7210` flag.
- [ ] SurrealDB at `127.0.0.1:8000` on the VPS, schema from `web/schema.surql` applied, `player` table exists. Test: `curl -u root:nobd_arcade_2026 http://127.0.0.1:8000/sql -d 'INFO FOR TABLE player;' -H 'NS: maplecast' -H 'DB: arcade'` on the VPS returns field definitions.
- [ ] At least one player account exists in SurrealDB (yours, from registering at `https://nobd.net`). Test: `curl ... 'SELECT username FROM player LIMIT 5'` returns your username.
- [ ] nginx `/ws` routing works (public `wss://nobd.net/ws` returns frames). This tells you nginx + TLS + upstream proxying is healthy before we add `/overlord`.
- [ ] You have a local build of flycast that produces a healthy binary: `cmake --build build-headless -- -j$(nproc)` exits 0 and `ldd build-headless/flycast` has zero GPU libs.

---

## 3. Expert Consultations — Whose Problems You're Inheriting

### The Relay HTTP Dispatcher

**Who:** [relay/src/turn.rs:81-220](../relay/src/turn.rs#L81) (`handle_http`)

**What they know:**
- The dispatcher is a flat `if` / `else if` chain on `first_line.starts_with(...)`. Order matters for prefix collisions — put `/overlord/api/*` branches **before** the generic 404.
- Body parsing is naive: split on `\r\n\r\n`, take `nth(1)`. Works for Content-Length bodies ≤ 64 KB (the hard cap in `handle_http`). For the savestate upload (multipart/form-data, up to ~30 MB decompressed), **you need to bump the cap OR stream the body separately**. See phase B6.
- Responses are hand-written strings. Helper macros or a `write_json_response(code, body)` function would clean this up but isn't required — just follow the existing pattern.
- Authorization extraction: `req.lines().find(|l| l.to_ascii_lowercase().starts_with("authorization:"))`. Same pattern as `/api/leave`.

**Warnings:**
- **Don't block the dispatcher task.** Each request runs in a `tokio::spawn` so long operations are OK as long as they're async. `systemctl restart` is a blocking syscall — wrap in `tokio::task::spawn_blocking` or use `tokio::process::Command`.
- **CRLF handling on file writes.** `emu.cfg` is LF-terminated on Linux. If you read it via JSON → browser → textarea → JSON → file, you must not let the browser insert `\r\n` line endings. Validate + normalize on the server side.
- **Don't leak absolute paths in error messages to the client.** Logs can have full paths; the JSON response should say "savestate not found" not "file not found at /opt/maplecast/...".

### The SurrealDB Auth Layer

**Who:** [relay/src/auth_api.rs:120-180](../relay/src/auth_api.rs#L120) (`handle_register`, `handle_signin`, `mint_browser_token`)

**What they know:**
- argon2 hashing with a static salt? Check the existing `handle_register` code — it should be using a per-account salt. If not, don't regress it.
- The JWT returned by `mint_browser_token` is scoped to the `browser` access level (see the SurrealDB `DEFINE ACCESS browser` in the schema) and contains the player's `id`. Use it as-is for reading the `admin` field.
- Token is stored client-side as `localStorage.nobd_token` by [web/js/auth.mjs](../web/js/auth.mjs). The admin UI reuses this key — if the user is already signed in to nobd.net when they visit `/overlord`, they're automatically signed in here.
- Non-admin users get 403 everywhere. Admin check is: query `SELECT admin FROM player WHERE id = $token_player_id` with the bearer token; if `admin == true`, allow; else 403.

**Warnings:**
- **Do NOT ship SurrealDB credentials to the browser.** The admin panel must never see `NOBD_DB_PASS`. All DB queries go through the relay.
- **The `admin` bool is the ONLY access gate.** Don't add a parallel hardcoded-password "emergency admin" path — that's the kind of thing that gets committed to a public repo by accident. If you lose admin access, you SSH into the box and flip the bool in SurrealDB directly.
- **Bootstrap problem:** how does the first admin get flagged? Answer: by hand. One SQL statement on the VPS after schema migration:
  ```
  UPDATE player SET admin = true WHERE username = 'tris';
  ```
  Document this in VPS-SETUP.md.

### The Flycast WebSocket Server

**Who:** [core/network/maplecast_ws_server.cpp](../core/network/maplecast_ws_server.cpp)

**What they know:**
- Current WS server binds `MAPLECAST_SERVER_PORT` (production: `127.0.0.1:7210`) for the TA mirror stream. It accepts multiple concurrent WS clients (relay + occasional direct dev clients) and broadcasts binary frames to all.
- There's existing text-message handling for JSON lobby messages (`queue_join`, `register_stick`, etc.). New control commands add to this handler.
- The WS server runs on its own thread (or thread pool — verify in code) separate from the render thread. Any command that needs to mutate emulator state (savestate load) must **bounce the call to the main/render thread** via a message queue, not call `dc_loadstate()` directly from the WS thread.

**Warnings:**
- **`dc_loadstate()` is NOT thread-safe from the WS thread.** See [ARCHITECTURE.md bug #8](ARCHITECTURE.md) — after a state load, you MUST also reset the mirror's per-region shadow buffers from live VRAM, otherwise the next frame's diff is computed against a stale base and the wasm clients see corrupt VRAM for seconds. The existing `broadcastFreshSync()` handler has this pattern — **copy it exactly**.
- **Adding a second WS listener is simpler than multiplexing control commands onto the existing TA mirror WS.** The mirror WS is binary-first, high-volume, and sends zero text after the initial handshake; mixing in JSON control messages risks corrupting the relay's parse logic. **Use a separate listener on `MAPLECAST_CONTROL_PORT=7211` for control-plane traffic.**
- **Control WS should be loopback-bound by default** (`127.0.0.1:7211`, NOT `0.0.0.0:7211`). Public exposure of control commands is an instant P0 vulnerability.
- **Flycast's dynarec signal handler intercepts SIGSEGV for fast-mem rewriting.** A crash in the control WS handler will NOT generate a useful stack trace — the fastmem rewriter will try to handle the fault first, then fall through to `die()`. Use `try/catch` around any code path that could dereference unvalidated pointers.

### The Static Asset Pipeline

**Who:** nginx + [web/](../web/)

**What they know:**
- nginx serves static files from `/var/www/maplecast/`. Deploys are `scp web/* root@66.55.128.93:/var/www/maplecast/`. There is NO build step — the web app is hand-written ES modules, no webpack, no bundler, no TypeScript.
- `web/king.html` + `web/js/*.mjs` is the spectator app. The admin UI follows the same conventions: vanilla HTML + ES modules + CSS files, no frameworks.
- Cache busting: each `<script src="foo.mjs?v=N">` has an explicit `?v=` query string. Bump it when you deploy a new version of the JS.
- nginx route table lives in `/etc/nginx/sites-enabled/maplecast`. To add a new route for `/overlord/*`, add a new `location` block pointing at either static files (for the HTML/JS) or `http://127.0.0.1:7202` (for `/overlord/api/*`).

**Warnings:**
- **Don't break king.html.** The admin UI is a separate page; any shared JS module (`auth.mjs`, `surreal.mjs`) is fine to import from `/js/` but don't MODIFY those shared modules from the admin work unless you're fixing a bug that affects both.
- **The embedded preview iframe needs `frame-ancestors` CSP permission.** If king.html's current CSP blocks iframing, add `'self'` to the `frame-ancestors` directive (or equivalent `X-Frame-Options: SAMEORIGIN`).
- **Cache-busting `?v=` params must match the file you just deployed.** If you deploy `admin.mjs` with `?v=2` but the HTML still references `?v=1`, you load stale JS. Use a dated stamp or a session-stable version (e.g. `?v=2026-04-08a`).

### The Live Preview Iframe

**Who:** [web/king.html](../web/king.html)

**What they know:**
- king.html is the existing WASM mirror renderer. It connects to `wss://nobd.net/ws` on load, receives frames, renders via WebGL2.
- It has a full UI (chat, queue, leaderboard, diagnostics) that's NOT wanted in the admin preview. The preview should be **render-only** — no chat panel, no queue buttons, no input.
- The cleanest way to get a stripped-down preview is a URL param: `https://nobd.net/king.html?embed=1` → hide all UI, just show the canvas. Add this flag to `web/king.html` and gate the UI elements behind it.

**Warnings:**
- **Don't spawn a second TA mirror stream connection** — the admin preview shares the same relay upstream as every other browser. No extra load on flycast.
- **The iframe gets its own WebGL context.** If the admin visits `/overlord` with `king.html` already open in another tab, they'll have two WebGL contexts rendering the same stream. That's fine — ~1% extra GPU on the admin's local machine, nothing on the server.

---

## 4. The Phased Plan

Six phases. Each phase is independently verifiable and independently deployable. Don't skip. The dependency graph is:

```
A: Flycast control WS  ──┐
                         ├──► C: Admin HTTP routes in relay
B: SurrealDB admin role ─┘      │
                                ├──► D: Admin web UI
                                │
                                └──► E: nginx + deploy

                    F: Integration test + polish
```

### Phase A — Flycast control WebSocket

**Goal:** flycast exposes a second WS listener on `127.0.0.1:7211` that accepts JSON control commands for savestate save/load and emulator reset. Commands run on the main thread via a message queue.

**Files:**

1. **[core/network/maplecast_ws_server.h](../core/network/maplecast_ws_server.h)** — add a new namespace/function:
   ```cpp
   namespace maplecast_control_ws {
       bool init(int port);
       void shutdown();
       bool active();

       // Queue a command for the render thread to execute.
       // Called from the WS handler thread; non-blocking.
       struct Command {
           enum Type { SavestateSave, SavestateLoad, Reset, Ping } type;
           int slot;              // for savestate cmds
           std::string reply_id;  // client correlation id
       };
       void queueCommand(Command cmd);

       // Called from the render thread once per frame. Drains the
       // queue and executes queued commands.
       void drainCommandQueue();
   }
   ```

2. **[core/network/maplecast_control_ws.cpp](../core/network/maplecast_control_ws.cpp)** (new file) — the control WS server implementation. Copy the skeleton of `maplecast_ws_server.cpp` and strip it to the minimum: one `websocketpp` endpoint, no binary broadcast, just JSON text messages in and out.

3. **[core/emulator.cpp](../core/emulator.cpp)** at the `Emulator::start()` function (around line 1030 where other maplecast services init) — add:
   ```cpp
   #ifndef MAPLECAST_HEADLESS_BUILD_STRIPPED_CONTROL
       int controlPort = 7211;
       const char* cpEnv = std::getenv("MAPLECAST_CONTROL_PORT");
       if (cpEnv) controlPort = std::atoi(cpEnv);
       maplecast_control_ws::init(controlPort);
   #endif
   ```

4. **[core/hw/pvr/Renderer_if.cpp](../core/hw/pvr/Renderer_if.cpp) `PvrMessageQueue::render()`** — drain the control queue once per frame, BEFORE `serverPublish`:
   ```cpp
   maplecast_control_ws::drainCommandQueue();
   if (maplecast_mirror::isServer() && taContext)
       maplecast_mirror::serverPublish(taContext);
   ```

5. **[core/network/CMakeLists.txt](../core/network/CMakeLists.txt)** — add `maplecast_control_ws.cpp` to the target_sources list.

**Command semantics:**

- `{"cmd":"ping","reply_id":"abc"}` → `{"ok":true,"pong":true,"reply_id":"abc"}` — smoke test
- `{"cmd":"savestate_save","slot":3,"reply_id":"..."}` → calls `dc_savestate(3, nullptr, 0)` on the render thread → writes to the path computed by `hostfs::getSavestatePath(3, true)`, returns `{"ok":true,"path":"...","size":N,"reply_id":"..."}`
- `{"cmd":"savestate_load","slot":1,"reply_id":"..."}` → calls `dc_loadstate(1)` on the render thread → returns `{"ok":true,"slot":1,"reply_id":"..."}`
- `{"cmd":"reset","reply_id":"..."}` → calls `dc_reset(true)` → returns `{"ok":true,"reply_id":"..."}`

**Critical invariant (ARCHITECTURE.md bug #8):** after `dc_loadstate()`, you MUST reset the mirror's per-region shadow buffers. The existing `client_request_sync` handler in `maplecast_mirror.cpp` has the pattern — a `for (i...) memcpy(_regions[i].shadow, _regions[i].ptr, _regions[i].size);` loop. Your `savestate_load` handler must do the same, OR trigger a `maplecast_mirror::requestSyncBroadcast()` which will cause the next frame to ship a fresh full SYNC and the shadows to reset. The latter is simpler and is what `broadcastFreshSync` already does internally — **use `requestSyncBroadcast()` after any `dc_loadstate()` call.**

**Verification (Phase A exit):**
- Local build with `-DMAPLECAST_HEADLESS=ON`, binary still has zero GPU libs
- Run locally: `MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1 ./build-headless/flycast mvc2.gdi`
- Check `ss -ltnp | grep 7211` shows flycast listening
- Send a ping via `websocat ws://127.0.0.1:7211` with `{"cmd":"ping","reply_id":"test"}`, receive `{"ok":true,"pong":true,"reply_id":"test"}`
- Send `savestate_save` for slot 9 (an unused slot), verify the file appears on disk
- Send `savestate_load` for an existing slot, verify the emulator state visibly changes via a browser connected to the mirror
- **Determinism rig still green:** `MAPLECAST_DUMP_TA=1` on both server and a mirror client, run for 60 seconds, byte-diff the dumps. 0 differ. This confirms the control WS addition hasn't introduced a race on the main thread.

### Phase B — SurrealDB admin role + VPS bootstrap

**Goal:** the existing SurrealDB schema gains an `admin` bool on `player`, the operator's account is flagged as admin, the relay has a helper function to check admin-ness from a JWT.

**Files:**

1. **[web/schema.surql](../web/schema.surql)** — add near line 43 (after the rating/tier fields):
   ```sql
   -- Admin role for /overlord access. Default false for all users.
   -- Flip to true with: UPDATE player SET admin = true WHERE username = '...';
   DEFINE FIELD admin ON player TYPE bool DEFAULT false;
   ```

2. **[relay/src/auth_api.rs](../relay/src/auth_api.rs)** — add a new helper:
   ```rust
   /// Given a bearer token (JWT), query SurrealDB as that token and
   /// return true iff the player record has `admin = true`. Returns
   /// false on any error (missing token, invalid token, DB unreachable,
   /// admin field absent, admin field false). This is the sole access
   /// gate for /overlord/api/* write endpoints.
   pub async fn check_admin(authorization: Option<&str>) -> bool {
       let token = match authorization
           .and_then(|s| s.strip_prefix("Bearer "))
           .map(str::trim)
       {
           Some(t) if !t.is_empty() => t,
           _ => return false,
       };
       // Query: SELECT admin FROM player WHERE id = $auth.id
       // SurrealDB treats the bearer token as the auth context, so
       // $auth.id is the authenticated player's id.
       let cfg = DbConfig::from_env();
       let query = "SELECT admin FROM player WHERE id = $auth.id LIMIT 1;";
       let resp = match reqwest::Client::new()
           .post(format!("{}/sql", cfg.url))
           .bearer_auth(token)
           .header("NS", &cfg.namespace)
           .header("DB", &cfg.database)
           .body(query.to_string())
           .send()
           .await
       {
           Ok(r) => r,
           Err(_) => return false,
       };
       let json: serde_json::Value = match resp.json().await {
           Ok(j) => j,
           Err(_) => return false,
       };
       // Response shape: [{"result":[{"admin":true}],"status":"OK","time":"..."}]
       json.as_array()
           .and_then(|a| a.first())
           .and_then(|o| o.get("result"))
           .and_then(|r| r.as_array())
           .and_then(|a| a.first())
           .and_then(|o| o.get("admin"))
           .and_then(|v| v.as_bool())
           .unwrap_or(false)
   }
   ```

3. **VPS bootstrap** — document in [docs/VPS-SETUP.md](VPS-SETUP.md) the one-time admin flag:
   ```bash
   ssh root@66.55.128.93
   curl -u root:nobd_arcade_2026 \
        -H 'NS: maplecast' -H 'DB: arcade' \
        -d 'UPDATE player SET admin = true WHERE username = "tris";' \
        http://127.0.0.1:8000/sql
   ```

**Verification (Phase B exit):**
- Schema migration applied on the VPS: `curl ... -d 'INFO FOR TABLE player;'` shows the `admin` field
- Your account flagged: `curl ... -d 'SELECT username, admin FROM player WHERE admin = true;'` returns your record
- `check_admin(Some("Bearer <your_jwt>"))` returns true from a Rust unit test or an ad-hoc debug endpoint
- `check_admin(None)` returns false
- `check_admin(Some("Bearer garbage"))` returns false
- A non-admin test account's JWT returns false

### Phase C — Admin HTTP routes in relay

**Goal:** the relay dispatches `/overlord/api/*` endpoints, each guarded by `check_admin()`, each implementing one of the G4-G13 acceptance tests.

**Files:**

1. **[relay/src/admin_api.rs](../relay/src/admin_api.rs)** (new file) — all the admin endpoint handlers. One function per endpoint, each returns `AdminResponse { ok: bool, data: Value, error: Option<String> }`. Use the same shape as `AuthResponse`.

2. **[relay/src/turn.rs](../relay/src/turn.rs)** — add new `else if` branches in `handle_http()` BEFORE the 404:
   ```rust
   } else if first_line.starts_with("POST /overlord/api/signin") {
       // Reuse auth_api::handle_signin but also require admin
       let auth_resp = auth_api::handle_signin(&body).await;
       if !auth_resp.ok {
           // Invalid creds, pass through
       } else {
           // Valid login — now check admin
           if !auth_api::check_admin(extract_token_from_response(&auth_resp)).await {
               return 403 forbidden;
           }
       }
       // ... response
   } else if first_line.starts_with("GET /overlord/api/status") {
       let auth = req.lines().find(...).and_then(...);
       if !auth_api::check_admin(auth.as_deref()).await { return 403; }
       let resp = admin_api::handle_status(&state).await;
       // ... response
   } else if first_line.starts_with("GET /overlord/api/savestates") {
       // same pattern
   // ... etc
   }
   ```

3. **The endpoint matrix** — each of these is a few dozen lines of Rust:

   | Method | Path | Handler | Notes |
   |---|---|---|---|
   | POST | `/overlord/api/signin` | `admin_api::handle_admin_signin` | Reuses `auth_api::handle_signin` + `check_admin` |
   | GET | `/overlord/api/status` | `admin_api::handle_status` | Reads `/proc/<pid>/status` for flycast memory; shells `systemctl is-active` for uptime; pulls fps/clients from `state.metrics()` |
   | GET | `/overlord/api/savestates` | `admin_api::handle_list_savestates` | `fs::read_dir("/opt/maplecast/.local/share/flycast/")`, filter `mvc2_*.state`, return JSON array of `{slot, size, mtime}` |
   | POST | `/overlord/api/savestates/save` | `admin_api::handle_savestate_save` | Body: `{"slot":N}`. Opens control WS at `ws://127.0.0.1:7211`, sends `{"cmd":"savestate_save","slot":N,"reply_id":"..."}`, awaits reply, returns it |
   | POST | `/overlord/api/savestates/load` | `admin_api::handle_savestate_load` | Same pattern, `{"cmd":"savestate_load",...}` |
   | POST | `/overlord/api/savestates/upload` | `admin_api::handle_savestate_upload` | multipart/form-data, parse body, write to `/opt/maplecast/.local/share/flycast/mvc2_<slot>.state`. **Must bump the 64 KB body cap on the HTTP dispatcher** — see phase B6 gotcha below |
   | GET | `/overlord/api/savestates/download/<slot>` | `admin_api::handle_savestate_download` | Read the file, stream it back with `Content-Disposition: attachment` |
   | GET | `/overlord/api/config` | `admin_api::handle_config_read` | `fs::read_to_string("/opt/maplecast/.config/flycast/emu.cfg")`, return as JSON `{text: "..."}` |
   | POST | `/overlord/api/config` | `admin_api::handle_config_write` | Body: `{text: "..."}`. Normalize line endings to LF. Validate (parseable as INI-ish). Write atomically (tempfile + rename). Return new content. |
   | POST | `/overlord/api/service/restart` | `admin_api::handle_service_restart` | Shell out to `systemctl restart maplecast-headless`. This is the one that needs root — see phase B7 polkit note |
   | GET | `/overlord/api/logs/tail?n=200` | `admin_api::handle_logs_tail` | Shell out to `journalctl -u maplecast-headless -n 200 --no-pager`, return text |
   | GET | `/overlord/api/logs/stream` | `admin_api::handle_logs_stream` | SSE: spawn `journalctl -u maplecast-headless -f` as a child process, stream stdout lines as `data: <line>\n\n` |

**Body size cap bump:**

[relay/src/turn.rs:96](../relay/src/turn.rs#L96) has `if buf.len() > 65536 { break; }`. Savestate uploads can be ~30 MB compressed. Options:

- **Raise the cap for `/overlord/api/savestates/upload` only.** Before the body-read loop, if `first_line.starts_with("POST /overlord/api/savestates/upload")`, use a 64 MB cap. Otherwise keep 64 KB.
- **Stream the upload directly to disk** instead of buffering in memory. This is cleaner but more work. V2.

Go with option 1 for v1: per-route cap.

**Service restart permissions:**

The relay runs as `maplecast-relay` user (check `systemctl cat maplecast-relay | grep User`). That user almost certainly can't `systemctl restart maplecast-headless` without authentication. Two options:

- **Polkit rule** granting `maplecast-relay` the ability to restart `maplecast-headless.service`:
  ```
  # /etc/polkit-1/rules.d/50-maplecast-admin.rules
  polkit.addRule(function(action, subject) {
      if (action.id == "org.freedesktop.systemd1.manage-units" &&
          action.lookup("unit") == "maplecast-headless.service" &&
          subject.user == "maplecast-relay") {
          return polkit.Result.YES;
      }
  });
  ```
- **Setuid helper script** at `/usr/local/sbin/maplecast-restart-headless` that the relay can exec without auth. Simpler than polkit.

Go with polkit for v1 (more explicit, easier to audit).

**Control WS client in Rust:**

Admin routes that proxy to flycast's control WS need a Rust WebSocket client. `tokio-tungstenite` is already a relay dependency (check `Cargo.toml`). Use it.

```rust
async fn send_control_cmd(cmd: serde_json::Value) -> Result<serde_json::Value, String> {
    use tokio_tungstenite::tungstenite::Message;
    use futures_util::{SinkExt, StreamExt};

    let (mut ws, _) = tokio_tungstenite::connect_async("ws://127.0.0.1:7211")
        .await.map_err(|e| e.to_string())?;
    ws.send(Message::Text(cmd.to_string())).await.map_err(|e| e.to_string())?;
    let reply = ws.next().await
        .ok_or("no reply")?
        .map_err(|e| e.to_string())?;
    match reply {
        Message::Text(t) => serde_json::from_str(&t).map_err(|e| e.to_string()),
        _ => Err("non-text reply".into()),
    }
}
```

Connect-per-request is fine for admin ops (low frequency, don't need persistent connection).

**Verification (Phase C exit):**
- All endpoints return correct JSON from `curl` with a valid admin JWT
- All endpoints return 403 with no JWT or a non-admin JWT
- `curl -X POST /overlord/api/savestates/save -d '{"slot":9}'` creates a file and the status endpoint sees it
- `curl -X POST /overlord/api/savestates/load -d '{"slot":1}'` changes the game state visible in a browser connected to the public stream
- `curl -X POST /overlord/api/service/restart` actually restarts the flycast service
- SSE log stream stays open and emits lines

### Phase D — Admin web UI

**Goal:** the static admin app at `/var/www/maplecast/overlord/` works end-to-end against phase C's backend.

**Files:**

1. **[web/overlord/index.html](../web/overlord/index.html)** (new) — the main admin page. Dark theme (we're overlords, not paper-pushers). Layout:
   ```
   ┌──────────────────────────────────────────────────────────┐
   │ [MapleCast Overlord]          tris · admin · Log out     │
   ├──────────────────────────────────────────────────────────┤
   │                                                           │
   │ ┌────────────────────┐  ┌────────────────────────────┐  │
   │ │  Status            │  │  Live Preview              │  │
   │ │  ● running         │  │                            │  │
   │ │  59.7 fps          │  │   [iframe king.html?embed] │  │
   │ │  301 MB / 1 GB     │  │                            │  │
   │ │  3 clients         │  │                            │  │
   │ │  Uptime: 2h 14m    │  │                            │  │
   │ │                    │  │                            │  │
   │ │  [Restart service] │  │                            │  │
   │ └────────────────────┘  └────────────────────────────┘  │
   │                                                           │
   │ ┌──────────────────────────────────────────────────────┐ │
   │ │  Savestates                                          │ │
   │ │  ────────────────────────────────────────────────    │ │
   │ │  Slot 0  .state          6.7 MB  Apr 3 13:26  [Load] │ │
   │ │  Slot 1  _1.state (LIVE) 7.7 MB  Apr 5 17:03  [Load] │ │
   │ │  Slot 3  _3.state        9.1 MB  Apr 6 02:44  [Load] │ │
   │ │  ...                                                  │ │
   │ │                                                        │ │
   │ │  [Save to slot __]  [Upload file__]  [Download all]   │ │
   │ └──────────────────────────────────────────────────────┘ │
   │                                                           │
   │ ┌──────────────────────────────────────────────────────┐ │
   │ │  emu.cfg                              [Save] [Reset] │ │
   │ │  ┌────────────────────────────────────────────────┐  │ │
   │ │  │ Dreamcast.AutoLoadState = yes                  │  │ │
   │ │  │ Dreamcast.SavestateSlot = 1                    │  │ │
   │ │  │ ...                                            │  │ │
   │ │  └────────────────────────────────────────────────┘  │ │
   │ └──────────────────────────────────────────────────────┘ │
   │                                                           │
   │ ┌──────────────────────────────────────────────────────┐ │
   │ │  Logs                    [tail] [follow] [clear]      │ │
   │ │  ┌────────────────────────────────────────────────┐  │ │
   │ │  │ 00:50:49 N[BOOT]  Game ID is [T1212N]          │  │ │
   │ │  │ 00:50:49 N[SAVESTATE]  Loaded state ver 853... │  │ │
   │ │  │ ...                                            │  │ │
   │ │  └────────────────────────────────────────────────┘  │ │
   │ └──────────────────────────────────────────────────────┘ │
   └──────────────────────────────────────────────────────────┘
   ```

2. **[web/overlord/overlord.mjs](../web/overlord/overlord.mjs)** (new) — the main admin JS. Handles:
   - Token check on load (redirect to `/overlord/login.html` if no token)
   - `fetchStatus()` polls `GET /overlord/api/status` every 2 seconds
   - `fetchSavestates()` populates the table
   - Button handlers for load/save/upload/download/restart
   - `openLogStream()` opens the SSE connection and appends lines to the log box
   - Auto-refresh savestate list after any save/upload
   - Optimistic UI (disable button → call → re-enable) with error toast on failure

3. **[web/overlord/overlord.css](../web/overlord/overlord.css)** (new) — dark theme, terminal font, status pill colors, rounded card layout. No frameworks.

4. **[web/overlord/login.html](../web/overlord/login.html)** (new) — simple login form. Reuses [web/js/auth.mjs](../web/js/auth.mjs) for the signin call, but posts to `/overlord/api/signin` instead of `/api/signin` (so non-admin users can't log in here even if they have a valid account). On success, stores token as `localStorage.overlord_token` (separate from `nobd_token` so logging out of the admin doesn't log you out of the spectator site).

5. **[web/overlord/config-editor.mjs](../web/overlord/config-editor.mjs)** (new) — the emu.cfg editor. Three modes:
   - **Raw text mode** — plain textarea, direct edit
   - **Sectioned mode** — renders the INI as collapsible sections with input fields (v1 can skip this, just ship raw text)
   - **Diff preview** — before save, show a text diff (red removed lines, green added lines) so the operator sees exactly what's changing

6. **[web/overlord/log-viewer.mjs](../web/overlord/log-viewer.mjs)** (new) — log tail renderer. Uses an `EventSource` to connect to `/overlord/api/logs/stream`. Virtual-scrolls if the buffer grows past 1000 lines.

7. **[web/king.html](../web/king.html)** — add the `?embed=1` query param handler. When set, add a CSS class to `<body>` that hides chat, queue, leaderboard, diagnostics — leaves only the canvas. ~10 lines of CSS + a single `if (new URLSearchParams(location.search).get('embed'))` check.

**Verification (Phase D exit):**
- Browser loads `/overlord/login.html`, login form renders
- Login with non-admin creds fails with a visible error
- Login with admin creds redirects to `/overlord/`
- Status pill shows "running" and numbers update live
- Savestate list populates with real files
- Clicking "Load" on slot 1 causes the embedded preview to visibly change state
- Clicking "Save to slot 9" creates a new row
- Uploading a file adds it to the list
- Editing emu.cfg, clicking save, confirming the diff, saving — the file on disk changes
- Log tail shows real-time lines as flycast emits them
- Click restart, see the status pill go red → yellow → green over ~5 seconds

### Phase E — nginx + deploy

**Goal:** `/overlord/*` is routable via nginx, static files are served, `/overlord/api/*` is proxied to the relay's HTTP endpoint.

**Files:**

1. **nginx config** at `/etc/nginx/sites-enabled/maplecast` on the VPS — add new location blocks:
   ```nginx
   # Admin panel static files
   location /overlord/ {
       alias /var/www/maplecast/overlord/;
       index index.html;
       try_files $uri $uri/ /overlord/index.html;
   }

   # Admin API → relay HTTP endpoint
   location /overlord/api/ {
       proxy_pass http://127.0.0.1:7202;
       proxy_http_version 1.1;
       proxy_set_header Host $host;
       proxy_set_header X-Real-IP $remote_addr;
       proxy_read_timeout 86400;  # SSE log stream needs long timeouts
       proxy_buffering off;       # SSE needs unbuffered
   }
   ```

2. **Deploy script** at `deploy/scripts/deploy-overlord.sh` (new) — mirror `deploy-headless.sh`:
   ```bash
   #!/bin/bash
   set -euo pipefail
   VPS="${1:?}"
   REPO=$(dirname "$(dirname "$(realpath "$0")")")/..
   cd "$REPO"

   # 1. Build relay with the new admin_api module
   cd relay && cargo build --release --target x86_64-unknown-linux-musl
   cd ..

   # 2. Ship relay binary
   scp relay/target/x86_64-unknown-linux-musl/release/maplecast-relay \
       "$VPS:/tmp/maplecast-relay.new"
   ssh "$VPS" '
       systemctl stop maplecast-relay
       install -m 0755 /tmp/maplecast-relay.new /opt/maplecast-relay
       rm /tmp/maplecast-relay.new
       systemctl start maplecast-relay
   '

   # 3. Ship web/overlord/
   ssh "$VPS" 'mkdir -p /var/www/maplecast/overlord'
   scp -r web/overlord/* "$VPS:/var/www/maplecast/overlord/"

   # 4. Bump king.html if embed mode changed
   scp web/king.html "$VPS:/var/www/maplecast/king.html"

   # 5. Verify
   ssh "$VPS" '
       systemctl is-active maplecast-relay
       curl -fsS http://127.0.0.1:7202/health | head -1
       ls /var/www/maplecast/overlord/
   '
   ```

3. **Polkit rule** installed once:
   ```bash
   ssh root@66.55.128.93 'cat > /etc/polkit-1/rules.d/50-maplecast-admin.rules' <<'EOF'
   polkit.addRule(function(action, subject) {
       if (action.id == "org.freedesktop.systemd1.manage-units" &&
           action.lookup("unit") == "maplecast-headless.service" &&
           subject.user == "maplecast-relay") {
           return polkit.Result.YES;
       }
   });
   EOF
   ssh root@66.55.128.93 'systemctl restart polkit'
   ```

**Verification (Phase E exit):**
- `curl -I https://nobd.net/overlord/` returns 200 + serves the login HTML
- `curl -I https://nobd.net/overlord/api/health` returns 200
- Deploy script works end-to-end on a clean retry
- Polkit rule persists across reboots (test: `systemctl reboot` the VPS, then verify `maplecast-relay` can still restart `maplecast-headless`)

### Phase F — Integration test + polish

**Goal:** run the full G15 cabaret. Fix any rough edges. Ship.

**Checklist:**

- [ ] Full G15 cabaret run: login → save to slot 5 → upload file → load slot 5 → edit config → save → restart → see new slot auto-load
- [ ] Status pill auto-updates during the restart (stopped → starting → running)
- [ ] Savestate load is smooth — the embedded preview doesn't freeze or show a black screen for more than ~500ms
- [ ] Log tail doesn't drop lines under load
- [ ] Config editor diff view correctly highlights changes
- [ ] Upload works for files up to 30 MB, bails loudly on files larger than the cap
- [ ] All error paths return useful messages (no 500s with empty bodies)
- [ ] Dark theme is actually dark
- [ ] Mobile-responsive? (v1 can ship desktop-only)
- [ ] Browser tab title shows "Overlord — MapleCast"
- [ ] Favicon?

---

## 5. Pitfalls & Tripwires

### "dc_loadstate crashed the emulator"

Almost certainly the shadow-reset invariant. After `dc_loadstate()` you MUST trigger a fresh SYNC so the mirror's per-region shadows realign. Call `maplecast_mirror::requestSyncBroadcast()` right after the load. If the crash is a SIGSEGV inside `serverPublish()`, check that the mirror is actually getting the reset — put a printf at the top of `requestSyncBroadcast` and watch it fire.

### "The admin token works but the browser says 403"

The browser is sending `Authorization: Bearer <nobd_token>` but the relay is looking for `<overlord_token>`. You have two separate tokens because the admin login is a separate flow. The browser-side code must pick the right one based on which section of the site it's in. Check `localStorage` in the devtools: if `overlord_token` is missing but `nobd_token` is present, the admin code is reading the wrong key.

### "Uploaded savestate is corrupt"

Body parsing is tricky with multipart. Log the raw bytes of the uploaded body at the point of write and compare to the local file. Common causes: (a) the HTTP dispatcher truncated the body at 64 KB because you forgot to bump the cap for the upload route, (b) multipart boundary parsing dropped the last chunk, (c) line-ending normalization ran on binary data.

### "Log stream stops after 60 seconds"

nginx's `proxy_read_timeout` defaults to 60 seconds. SSE connections are held open indefinitely. Set `proxy_read_timeout 86400;` in the `/overlord/api/` location block. Also set `proxy_buffering off;` — nginx will buffer the SSE stream otherwise.

### "Config save succeeded but flycast ignored the new value"

Flycast reads `emu.cfg` **at startup**, not continuously. After writing the config, the operator must click "Restart service" for the changes to take effect. Make this explicit in the UI: after a successful save, show a banner "⚠️ Config saved. Click **Restart service** to apply." Don't auto-restart — some config changes are non-critical and the operator might not want a service interruption.

### "Polkit rule doesn't work"

Polkit rules require the `polkit-1` daemon to be running and the rule file to have a `.rules` extension in `/etc/polkit-1/rules.d/`. Test with `pkcheck --action-id=org.freedesktop.systemd1.manage-units --process=<relay-pid>`. If polkit is too much hassle, fall back to a setuid helper binary at `/usr/local/sbin/maplecast-restart-headless`.

### "Logged-in session silently expires"

The SurrealDB JWT has a TTL (check the `DEFINE TOKEN` definition in the schema). When it expires, every admin API call returns 403. The admin UI should handle 403 responses by redirecting to the login page. Don't show a silent "nothing's updating" UI.

### "The embedded preview iframe is blank"

Check the browser console for Content Security Policy violations. The king.html page may be served with `Content-Security-Policy: frame-ancestors 'none'` or `X-Frame-Options: DENY`, which blocks iframe embedding. Add `'self'` to the `frame-ancestors` directive, or use `X-Frame-Options: SAMEORIGIN`.

### "Two admin sessions fight over the savestate"

V1 is single-operator. If two operators try to load different slots at the same time, whichever one hit the control WS last wins. No locking. This is fine — the operator is expected to coordinate out of band. V2 can add session locking if multi-admin becomes a thing.

### "The restart button does nothing"

Check the polkit rule is active (`pkcheck` as above). Check the relay's user is `maplecast-relay`, not `root` or `nobody`. Check `systemctl status maplecast-relay` doesn't show permission errors in the journal. If the rule is right but it still fails, try a setuid helper instead.

---

## 6. Out of Scope (Do Not Do These In This Workstream)

- **Multi-user admin audit log.** "Who did what when" is a real feature for a real team, but we're a single-user cab.
- **Rollback / undo.** Save-state edits are destructive. If you load slot 1 over a mid-match state, that match is gone. V2.
- **Config schema enforcement.** The config editor accepts any text. Validation is "does it parse as INI?" not "is this a valid flycast option with a sane value." V3.
- **ROM manager.** Uploading and switching ROMs. Requires savestate namespace changes and a full service restart. V2.
- **Remote input control.** Taking over a player slot from the admin UI for debug/maintenance. Cool but scope-creepy.
- **Visual input mapper.** The `[input]` section of emu.cfg is gnarly. A visual mapper is v3.
- **Alerts / notifications.** "Notify me when the service crashes" → use Prometheus alertmanager (already have /metrics).
- **Historical metrics.** Graphs of fps/memory/clients over time → use Grafana with the existing Prometheus exporter.
- **Admin API rate limiting beyond nginx defaults.** The admin is one trusted user. V2 if we multi-user.

---

## 7. Definition of Done

All 15 gates green. The `/overlord` admin panel:

1. Builds from the existing repo with no new top-level dependencies (reuses `tokio`, `reqwest`, `serde_json`, `argon2`, `tokio-tungstenite` that the relay already has)
2. Deploys via `./deploy/scripts/deploy-overlord.sh root@66.55.128.93` in one command
3. Runs on the existing VPS with no additional RAM cost (the relay grows by maybe 5 MB; no new service)
4. Is gated by SurrealDB admin role, enforced server-side on every write
5. Can fully manage savestates (list, load, save, upload, download) without an SSH session
6. Can edit `emu.cfg` with diff preview and a restart-required banner
7. Has a live log tail
8. Has a live preview of the game via the existing WASM renderer
9. Does not regress the determinism rig (headless flycast wire bytes still 460/460 match)
10. Does not regress the spectator site (nobd.net viewers still see the same stream, the home bypass from 2026-04-08 is still live)

**Ship the whole thing or ship nothing.** Don't leave the half-built admin API dangling on the relay — if phase C gates fail, revert phase A/B too until the whole workstream is green again.

---

## Appendix A — Control WS protocol reference

Text-only JSON WebSocket at `127.0.0.1:7211`. Messages are one JSON object per WS frame. Bidirectional.

### Client → server (commands)

| `cmd` | Fields | Description |
|---|---|---|
| `ping` | `reply_id` | Heartbeat / smoke test |
| `savestate_save` | `slot` (int 0-99), `reply_id` | Save current state to the given slot |
| `savestate_load` | `slot` (int 0-99), `reply_id` | Load the state from the given slot |
| `reset` | `reply_id` | Soft-reset the emulator (equivalent to `dc_reset(true)`) |
| `status` | `reply_id` | Query current state: frame count, fps, loaded slot, uptime |

### Server → client (replies)

Every reply echoes the `reply_id` from the originating command. Shape:
```json
{
    "ok": true,
    "reply_id": "...",
    "cmd": "savestate_save",
    "data": { ... command-specific ... }
}
```

On error:
```json
{
    "ok": false,
    "reply_id": "...",
    "cmd": "...",
    "error": "human-readable message"
}
```

### Execution model

Commands are queued by the WS handler thread into a thread-safe `std::deque<Command>` inside `maplecast_control_ws.cpp`. The render thread drains this queue once per frame at the top of `PvrMessageQueue::render()` (before `serverPublish`). Each command is executed synchronously on the render thread, and the reply is pushed back to the originating client via a stored `connection_hdl` map.

The queue is bounded (max 16 pending commands). If full, new commands are rejected with `{"ok":false,"error":"command queue full"}`. 16 is arbitrary and we don't expect contention.

---

## Appendix B — File layout after this workstream

```
core/network/
├── maplecast_control_ws.cpp      ← NEW: control WS server
├── maplecast_control_ws.h        ← NEW: control WS header
├── maplecast_ws_server.cpp       ← unchanged
├── maplecast_mirror.cpp          ← unchanged (but drainCommandQueue called from Renderer_if)
└── CMakeLists.txt                ← updated to include control_ws

core/hw/pvr/
└── Renderer_if.cpp               ← one-line addition: drainCommandQueue() before serverPublish

relay/src/
├── admin_api.rs                  ← NEW: all /overlord/api/* handlers
├── auth_api.rs                   ← updated: add check_admin()
├── turn.rs                       ← updated: dispatch /overlord/api/* routes
└── Cargo.toml                    ← possibly add futures-util if not present

web/
├── king.html                     ← updated: ?embed=1 flag hides spectator UI
├── overlord/
│   ├── index.html                ← NEW
│   ├── login.html                ← NEW
│   ├── overlord.mjs              ← NEW
│   ├── overlord.css              ← NEW
│   ├── config-editor.mjs         ← NEW
│   └── log-viewer.mjs            ← NEW
└── schema.surql                  ← updated: add admin field

deploy/
└── scripts/
    ├── deploy-headless.sh        ← unchanged
    └── deploy-overlord.sh        ← NEW: builds relay + ships web/overlord/

docs/
├── ARCHITECTURE.md               ← updated: mention the control WS + admin panel
├── VPS-SETUP.md                  ← updated: admin role bootstrap + /overlord section
└── WORKSTREAM-OVERLORD.md        ← this doc
```

No changes to: `maplecast_mirror.cpp`, `maplecast_ws_server.cpp`, `maplecast_input_server.cpp`, `maplecast_wasm_bridge.cpp`, or any of the headless / wire-format infrastructure. The existing TA mirror path stays byte-perfect and the determinism rig still passes.
