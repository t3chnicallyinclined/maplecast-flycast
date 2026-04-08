# WORKSTREAM: NOBD NATIVE CLIENT — "Pro Mode"

> **⚠️ DESIGN PREMISE CHANGED — 2026-04-08**
>
> This doc was written when flycast ran on a **home box** and the VPS
> relay fanned out to browsers over the internet. The native client's
> killer feature was "skip the VPS entirely — connect directly to home
> flycast for 6-15ms lower latency than browsers."
>
> **That topology no longer exists.** Since 2026-04-08 flycast runs on
> the VPS (headless compile-out, no GPU), and the home box is out of
> the nobd.net production path. See `docs/ARCHITECTURE.md` "System
> Topology" and `docs/VPS-SETUP.md` §9.
>
> **What this means for the native client:**
>
> 1. "Direct to home" is no longer a shortcut — home isn't the
>    authoritative instance anymore. The native client now has the
>    same target as the browser: `wss://nobd.net/ws` (or
>    `ws://66.55.128.93:7210` if you want to bypass nginx/relay, but
>    `:7210` is loopback-only by default).
>
> 2. The latency win over the browser shrinks. It was "save the
>    VPS relay hop + browser compositor overhead." Now it's just
>    "save the browser compositor overhead" — still real (the Gamepad
>    API vsync-aligned polling alone adds ~8ms), but smaller.
>
> 3. The biggest remaining native-client win is **input directness**:
>    raw HID → UDP packet without Gamepad API delay, and output
>    directly to a full-screen SDL window without browser compositor.
>    Both still worth building, but the "direct to home" framing below
>    is stale and should be reframed as "direct to VPS flycast".
>
> 4. The doc below is left intact as a historical record of the
>    original design. Before executing any phase, reconcile the
>    endpoint assumptions (references to "home flycast" / "home IP" /
>    `74.101.20.197`) with the new VPS-resident topology.
>
> **If you're picking up this workstream fresh**: read this section,
> then go read `docs/ARCHITECTURE.md` "System Topology" to understand
> where flycast actually lives in production, then come back and
> re-plan the endpoint resolution strategy in §9 accordingly.

> **One-shot implementation guide.** Read top-to-bottom, execute in order. Do not skip phases. Every decision, file path, protocol byte, and edge case is captured here so an agent can land a working Windows/Linux/macOS client in a single pass.

---

## 0. Context You Need Before Touching Code

### What this is
A native desktop application that a competitive player downloads and runs **instead of** the WASM/browser client. When a spectator's turn in the queue comes up, their browser deep-links them into the native client, which connects **directly** to the home flycast server — no VPS relay, no browser compositor, no Gamepad API polling. ~6-15ms faster E2E than browser, depending on geography.

### What this is NOT
- A new streaming protocol. **We reuse the existing TA mirror wire format byte-for-byte.**
- A new renderer. **We reuse `packages/renderer/` — the same C++ code that compiles to WASM today gets a second build target for native OpenGL.**
- A replacement for the web app. **king.html stays. WASM stays. Browser is the front door, spectator path, and matchmaking UI. Native is only invoked when it's your turn.**
- A server change. **Flycast needs zero modifications.** It already serves WS:7200 (binary mirror) and listens UDP:7100 (gamepad input). Native client just speaks those protocols.

### The three existing subsystems the native client plugs into

```
┌─────────────────────────────────────────────────────────────────┐
│                     HOME BOX (flycast server)                    │
│                                                                  │
│   ┌──────────────┐   ┌────────────────┐   ┌─────────────────┐   │
│   │ Game loop    │   │ Input server   │   │ Mirror server    │   │
│   │ (MVC2 @60fps)│◄──│ UDP :7100      │   │ WS :7200         │   │
│   │              │   │                │   │ (binary frames)  │   │
│   │              │   │ atomic writes  │   │                  │   │
│   │              │──►│   kcode[] ──►  │   │ publishFrame() ──┤   │
│   └──────────────┘   └────────────────┘   └────────┬─────────┘   │
└────────────────────────────────────────────────────┼─────────────┘
                                                     │
                                                     │ already serves
                                                     │ WS binary stream
                                                     │
              ┌──────────────────────────────────────┴──────────────┐
              │                                                      │
              ▼                                                      ▼
┌─────────────────────┐                              ┌──────────────────────┐
│  VPS RELAY          │                              │  NATIVE CLIENT (NEW) │
│  nobd.net           │                              │                      │
│                     │                              │  Direct WS to home   │
│  Fans out to 500+   │                              │  Direct UDP to home  │
│  browser spectators │                              │  SDL2 window         │
│                     │                              │  libnobdrenderer.a   │
│  (unchanged)        │                              │  vsync-off option    │
└──────────┬──────────┘                              └──────────────────────┘
           │
           ▼
┌──────────────────────┐
│  BROWSER (king.html) │
│  WASM spectators     │
└──────────────────────┘
```

**Key insight: the native client and the VPS relay are peers that both consume the same WS:7200 stream from home.** The VPS exists because browsers can't do raw UDP and need fan-out. The native client doesn't need either — it's a single direct consumer.

---

## 1. Success Criteria — How You Know You're Done

Phase-gated, no "mostly works" nonsense:

| Gate | Acceptance test |
|------|-----------------|
| **G1** | `packages/renderer/` builds a static lib `libnobdrenderer.a` (Linux x86_64) without Emscripten. Same sources that compile to WASM today. |
| **G2** | A standalone C++ program links `libnobdrenderer.a`, creates an SDL2 OpenGL 3.3 context, calls `renderer_init(640, 480)`, and successfully renders a **canned SYNC + delta** from a captured `.bin` file. Pixel output matches WASM browser rendering. |
| **G3** | The same program connects to `ws://<home_ip>:7200`, receives live frames, and renders at 60fps sustained. Visual parity with the browser confirmed side-by-side. |
| **G4** | Raw HID gamepad → UDP `<home_ip>:7100` round trip: press a button in the native client, see the character react in-game. Slot assignment works. |
| **G5** | Relay `/api/claim-slot` endpoint issues a signed token; native client validates it on startup. Token is HMAC-bound to the player_id and slot. |
| **G6** | Deep link `nobd://join?token=...` launches the native client on Linux via `.desktop` file. Browser opens the link, client pops up, auto-connects, game visible. |
| **G7** | Windows `.exe` installer registers the `nobd://` protocol. Same test as G6 on Windows 10/11. |
| **G8** | macOS `.app` bundle with `CFBundleURLTypes` registers `nobd://`. Same test on macOS 13+. |
| **G9** | End-to-end measured latency on LAN: **sub-5ms** button-to-pixel (excludes vblank/game-tick time). Measured via external high-speed camera or LED-pair rig, not self-reported telemetry. |
| **G10** | Browser `king.html` "I GOT NEXT" → match end → deep link → native client → fight → match end → native client exits gracefully → browser back to spectator mode. Full loop, zero manual steps. |

**Until G10 is green, this workstream is not done.** Don't open a PR for "G1-G5 works". Ship the whole thing.

---

## 2. Prerequisites — What Must Already Exist

Before starting Phase 1, verify these are true. If any is false, fix that first.

- [ ] `packages/renderer/` builds cleanly to `renderer.mjs` + `renderer.wasm` via `emcmake cmake .. && emmake make`. Test: `cd packages/renderer && mkdir -p build && cd build && emcmake cmake .. && emmake make -j` should produce `renderer.wasm ~831KB`.
- [ ] Flycast server runs with `MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1` and serves binary frames on `ws://localhost:7200`. Test: `websocat -b ws://localhost:7200 | head -c 8 | xxd` shows `SYNC` or `ZCST` magic.
- [ ] Flycast input server accepts UDP on `:7100`. Test: `echo -ne '\xFF\xFF\xFF\xFF' | nc -u -q 0 localhost 7100` does not error.
- [ ] VPS relay at `nobd.net` is running `maplecast-relay.service` with the latest binary (includes `/api/register`, `/api/signin`, `/api/telemetry`, `/turn-cred`, `/metrics`, `/health` endpoints).
- [ ] SurrealDB is running on the VPS at `127.0.0.1:8000`, namespace `maplecast`, database `arcade`, schema from `web/schema.surql` applied.
- [ ] `web/king.html` loads at `https://nobd.net` and shows the arcade cabinet with WASM streaming.

---

## 3. Expert Consultations — Whose Problems You're Inheriting

### The Renderer Expert
**Who:** The C++ you wrote in `packages/renderer/src/wasm_bridge.cpp` (the standalone mirror renderer).

**What they know:**
- The renderer is already mostly portable — it's a plain C++17 library that calls `gl*` via libretro's GLSM wrapper. The emscripten-specific code is confined to **one file**: `wasm_gl_context.cpp`. Everything else compiles on any platform with a GL ES 3.0 context.
- The `stubs.cpp` file exists because the renderer needs to satisfy unresolved symbols from flycast sources it links (PVR, TA, GLES rend). The stubs are already platform-agnostic — they work the same native or WASM.
- The GLSM patches in `glsm_patched.c` filter out `GL_FOG` and `GL_ALPHA_TEST` enable/disable calls that WebGL2 rejects. **Desktop GL accepts these but ignores them on core profile. The patches are harmless native-side, keep them.**
- The five bugs fixed in commit `466d72d54` (see `docs/ARCHITECTURE.md` § "Five race conditions fixed") are all in `wasm_bridge.cpp`. They apply to the native path identically. **Do not touch them.**

**Warnings:**
- The renderer caches WebGL2 capability state in a module-scoped `Set`. On native, swap to a constant array or `std::unordered_set<GLenum>`.
- `emscripten_set_canvas_element_size()` in `wasm_gl_context.cpp:148` is an Emscripten-only call. Native version calls `SDL_SetWindowSize()`.
- Do not try to use GL 4.x features. The renderer targets **GLES 3.0 / GL 3.3 Core**. That's the Dreamcast fixed-function pipeline emulated through shaders.

### The Protocol Expert
**Who:** The Rust relay in `relay/src/protocol.rs` and the C++ server in `core/network/maplecast_mirror_server.cpp`.

**What they know:**
- The on-wire format has **four possible first bytes**:
  1. `"SYNC"` (0x53 0x59 0x4E 0x43) — uncompressed full state
  2. `"ZCST"` (0x5A 0x43 0x53 0x54) followed by a 4-byte uncompressed size. If size > 1MB, it's a compressed SYNC. Otherwise, a compressed delta.
  3. Anything else — uncompressed delta frame (rare, only if zstd fails)
- Compression is zstd level 1 for deltas, level 3 for SYNC. The decompressor must pre-allocate ≥16MB (SYNC can decompress to 8MB + headers).
- **The current wire format has no length prefix on the WebSocket layer** — the WS framing handles that. When you read from the WS, you get complete message buffers. On raw TCP you'd need to add length prefixes, but we're not doing that.
- The relay's `protocol.rs:55` already implements `is_sync_or_compressed_sync()` — copy that logic into C++ for the native client. It's ~10 lines.

**Warnings:**
- **The decompressed SYNC buffer size must be checked BEFORE decompressing.** A malicious or corrupted frame with a huge `uncompressedSize` header could OOM you. Cap at 16MB.
- **The checksum field in delta frames is currently all zeros.** TCP handles integrity; don't verify it, but don't rely on it either.
- **The `_prevTA` buffer never shrinks.** If the TA buffer size decreases between frames, keep the old bytes in the tail — the delta encoding assumes this. See `wasm_bridge.cpp:553` for the existing handling.

### The Input Expert
**Who:** `core/network/maplecast_input_server.cpp` and the NOBD stick firmware.

**What they know:**
- UDP packets to `:7100` are **4 bytes** from a NOBD stick: `[LT, RT, btn_hi, btn_lo]`. Buttons are active-low in a 16-bit word.
- Packets **from the WebSocket handler loopback** are **5 bytes** with a slot prefix: `[slot, LT, RT, btn_hi, btn_lo]`. The slot byte routes to player 0 or 1.
- The server tracks `srcIP:srcPort` per slot. First packet from an unregistered IP triggers registration; subsequent packets from the same source feed that slot.
- **Ghost input rules:** `disconnectPlayer(slot)` zeros out `srcIP` so old packets don't route. The native client must send a `{type: "leave"}` JSON over the same WS (or a shutdown UDP packet) when exiting, or it'll linger for the 30-second idle-kick.

**Warnings:**
- **Don't use the 5-byte loopback format.** That's only for WS-to-UDP forwarding on the same machine. Native clients speak the 4-byte format from an external IP, identical to a physical NOBD stick.
- **SO_BUSY_POLL is enabled on the UDP socket** — send at any rate you want, the server will keep up. Target 1000Hz (every 1ms) from the native client for overkill.
- **The input server binds UDP based on srcIP.** If a player behind CGNAT loses their port binding, they'll appear to disconnect. This is unavoidable with UDP — plan for reconnect-on-timeout.

### The Auth Expert
**Who:** `relay/src/auth_api.rs` and `relay/src/turn.rs`.

**What they know:**
- `/api/signin` validates against SurrealDB's `player` table using `crypto::argon2::compare(pass_hash, $password)`. Returns the full player row on success.
- `/turn-cred` uses HMAC-SHA1 with a shared secret (`TURN_SECRET` env var) and a 1-hour expiry. The pattern is: `username = "{expiry_unix}:{user_id}"`, `password = base64(hmac_sha1(secret, username))`.
- SurrealDB is reached via `reqwest` from the relay. The relay holds admin credentials in `NOBD_DB_USER` / `NOBD_DB_PASS` env vars.
- The viewer role (read-only) is for browser-side queries. **The claim-slot token must be issued by the relay, not the browser.** The browser never holds signing secrets.

**Warnings:**
- **Token expiry must be short.** Five minutes max. If the token is stolen, the thief has N minutes to join as that player. Keep it tight.
- **Tokens must be single-use.** When the native client redeems a token, mark it used in SurrealDB so the same token can't join twice.
- **The home server IP inside the token is a promise, not a verification.** Anyone with the token can reach the IP; that's fine. But if the home server IP changes (dynamic DNS), the token is stale immediately.

### The Queue Expert
**Who:** `core/network/maplecast_ws_server.cpp` (JSON queue protocol) and `web/js/queue.mjs` (browser side).

**What they know:**
- Browser sends `{type: "queue_join", name: "tris"}` when user clicks "I GOT NEXT".
- Server broadcasts `{type: "status", queue: [...]}` every second with the ordered queue.
- When a slot opens, server sends `{type: "your_turn"}` to the first queued player.
- Current flow: browser receives `your_turn`, auto-sends `{type: "join", id, name, device}`, server assigns slot, browser starts sending gamepad WS binary.

**Warnings:**
- **The native client's join must NOT collide with the browser's auto-join.** The browser-side `your_turn` handler in `ws-connection.mjs:116` immediately sends `{type: "join"}`. When we add the native client path, we must **suppress** the browser's auto-join and instead show a "Launching NOBD Client..." modal.
- **Match-end eviction is slot-specific.** If the native client handles slot 0 and the browser is still open as a spectator, only slot 0 gets the `kicked` message. Browser WS stays connected.

### The Packaging Expert
**Who:** Nobody yet. This is greenfield.

**What they'd know if they existed:**
- Windows protocol handlers live in `HKEY_CLASSES_ROOT\nobd\shell\open\command` with the value `"C:\Path\nobd.exe" "%1"`. No registry = no deep link.
- macOS protocol handlers live in `Info.plist` under `CFBundleURLTypes`. Gatekeeper requires a Developer ID signature OR the user right-clicks → Open the first time.
- Linux protocol handlers live in `~/.local/share/applications/nobd.desktop` with `MimeType=x-scheme-handler/nobd;`. Register via `xdg-mime default nobd.desktop x-scheme-handler/nobd`.
- **All three platforms need code signing** for production. Dev builds can skip it. Ship unsigned initially, add signing in Phase 8.

---

## 4. Architecture — The Native Client Internals

### Module layout (new directory `packages/native-client/`)

```
packages/native-client/
├── CMakeLists.txt              # Builds everything below, links libnobdrenderer.a
├── README.md                   # How to build + run
├── src/
│   ├── main.cpp                # Entry point, arg parsing, URL scheme dispatch
│   ├── window.cpp / .h         # SDL2 window + GL context + vsync toggle
│   ├── ws_client.cpp / .h      # WebSocket client (libwebsockets or tungstenite-cxx)
│   ├── udp_input.cpp / .h      # UDP sender to :7100, raw HID gamepad → packets
│   ├── gamepad.cpp / .h        # SDL2 GameController → NOBD button format
│   ├── token.cpp / .h          # Deep link parser, HMAC verify, token state
│   ├── session.cpp / .h        # High-level session: claim → connect → play → exit
│   └── telemetry.cpp / .h      # Optional: POST to /api/telemetry like browser does
├── resources/
│   ├── icons/
│   │   ├── nobd.ico            # Windows
│   │   ├── nobd.icns           # macOS
│   │   └── nobd.png            # Linux
│   └── nobd.desktop            # Linux .desktop template
├── installer/
│   ├── windows/
│   │   ├── nobd-installer.nsi  # NSIS script, registers protocol handler
│   │   └── build.sh            # makensis wrapper
│   ├── macos/
│   │   ├── Info.plist          # Bundle template with CFBundleURLTypes
│   │   └── build.sh            # hdiutil → .dmg
│   └── linux/
│       ├── postinst            # Debian postinst — registers xdg-mime
│       └── build.sh            # fakeroot dpkg-deb
└── tests/
    ├── canned_sync.bin         # Captured SYNC frame for headless rendering tests
    ├── canned_deltas.bin       # Captured delta stream (first 60 frames of MVC2)
    └── test_renderer.cpp       # G2 acceptance test
```

### Build targets

```
libnobdrenderer.a  (packages/renderer/ — new target)
       │
       │ static link
       ▼
nobd-arcade        (packages/native-client/ — main executable)
       │
       ├── libnobdrenderer.a    (our renderer)
       ├── SDL2                 (window, input, gamepad, main loop)
       ├── libwebsockets        (WS client)
       ├── OpenSSL/mbedTLS      (HMAC + TLS for wss://)
       └── zstd                 (decompress — already in libnobdrenderer but ok to link twice)
```

### Thread model

```
Main thread:
  - SDL event loop
  - Window resize / close events
  - Gamepad polling (16x per frame via SDL_PollEvent burst)
  - Telemetry timer
  - Render submit (glFlush + SDL_GL_SwapWindow)

WS thread:
  - Owns the WebSocket connection
  - Receives binary frames
  - Pushes to a lock-free SPSC queue → main thread

Render happens on main thread (OpenGL contexts are thread-local and moving them around
is a footgun on macOS). WS thread just shovels bytes into the queue.

UDP thread:
  - Separate because we want 1000Hz input polling regardless of render rate
  - Reads from an atomic snapshot of the latest gamepad state
  - Sends a 4-byte packet every 1ms

Total: 3 threads (main + WS + UDP). No worker pool. No complexity.
```

### State machine

```
                ┌──────────────┐
                │  STARTED     │
                │  (no args)   │
                └──────┬───────┘
                       │
                       │ command line parsed
                       │
          ┌────────────┴─────────────┐
          │                          │
          ▼                          ▼
┌──────────────────┐      ┌──────────────────┐
│  STANDALONE      │      │  DEEP LINK       │
│  (manual host)   │      │  (nobd:// URL)   │
│                  │      │                  │
│  ./nobd-arcade   │      │  parsed token    │
│    --host IP     │      │  HMAC verified   │
│    --port 7200   │      │  expiry checked  │
└────────┬─────────┘      └────────┬─────────┘
         │                         │
         └────────────┬────────────┘
                      │
                      ▼
           ┌──────────────────┐
           │  CONNECTING      │
           │  WS → home:7200  │
           └────────┬─────────┘
                    │
                    │ WS opened
                    │
                    ▼
           ┌──────────────────┐
           │  AWAITING_SYNC   │
           │  renderer inited │
           │  window visible  │
           └────────┬─────────┘
                    │
                    │ first SYNC frame
                    │
                    ▼
           ┌──────────────────┐
           │  LIVE            │
           │  rendering deltas│
           │  sending inputs  │
           └────────┬─────────┘
                    │
                    │ window close OR
                    │ match_end message OR
                    │ WS disconnect
                    │
                    ▼
           ┌──────────────────┐
           │  SHUTTING_DOWN   │
           │  send leave JSON │
           │  close WS + UDP  │
           │  destroy renderer│
           └──────────────────┘
```

### Config / command line

```
nobd-arcade [OPTIONS] [nobd://URL]

OPTIONS:
  --host <HOST>           Home server hostname or IP (default: localhost)
  --ws-port <PORT>        WebSocket port on home server (default: 7200)
  --udp-port <PORT>       UDP input port on home server (default: 7100)
  --slot <0|1>            Which player slot to claim (default: from token or 0)
  --token <BASE64>        Claim token from deep link
  --name <NAME>           Player name (default: from token or "GUEST")
  --vsync <on|off>        Vsync mode (default: off for competitive)
  --resolution <HxW>      Window size (default: 960x720)
  --fullscreen            Start fullscreen
  --no-input              Spectator mode — render only, no UDP sending
  --verbose               Debug logging
  --version               Print version and exit
  --help                  Print help and exit

POSITIONAL:
  nobd://URL              Deep link (alternative to --token)
```

---

## 5. Protocol Reference — What the Native Client Must Speak

### 5.1 WebSocket to flycast `ws://<home>:7200` — binary frames

**Incoming (server → client):**

```
╔══════════════════════════════════════════════════════════════╗
║  Every binary WS message is ONE complete frame.              ║
║  No fragmentation, no length prefix — the WS layer handles   ║
║  framing. Read message, classify by magic, decompress if     ║
║  needed, dispatch.                                           ║
╚══════════════════════════════════════════════════════════════╝
```

**Classification (pseudocode — copy into C++):**

```c
FrameType classify(const uint8_t* buf, size_t len) {
    if (len < 4) return FRAME_INVALID;
    uint32_t magic;
    memcpy(&magic, buf, 4);

    // "SYNC" as little-endian u32 = 0x434E5953
    if (magic == 0x434E5953) return FRAME_SYNC_UNCOMPRESSED;

    // "ZCST" as little-endian u32 = 0x5453435A
    if (magic == 0x5453435A) {
        if (len < 8) return FRAME_INVALID;
        uint32_t uncomp_sz;
        memcpy(&uncomp_sz, buf + 4, 4);
        if (uncomp_sz > 16 * 1024 * 1024) return FRAME_INVALID; // DoS guard
        return (uncomp_sz > 1024 * 1024)
            ? FRAME_SYNC_COMPRESSED
            : FRAME_DELTA_COMPRESSED;
    }

    // Otherwise assume raw delta (rare — server only falls back if zstd fails)
    return FRAME_DELTA_UNCOMPRESSED;
}
```

**Decompression:**

For `FRAME_*_COMPRESSED`, skip the 8-byte `ZCST` header and feed the remainder to `ZSTD_decompress(dst, dst_capacity, src + 8, src_len - 8)`. The decompressor state should be a single reused `ZSTD_DCtx*` — allocating per-frame is ~200µs of waste.

**Dispatch after classification/decompression:**

```c
switch (type) {
case FRAME_SYNC_UNCOMPRESSED:
case FRAME_SYNC_COMPRESSED:
    renderer_sync(payload, payload_len);
    _glStateInit = false;  // redo GL enables after SYNC
    break;
case FRAME_DELTA_UNCOMPRESSED:
case FRAME_DELTA_COMPRESSED:
    if (!_glStateInit) {
        glEnable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_STENCIL_TEST);
        glEnable(GL_SCISSOR_TEST);
        _glStateInit = true;
    }
    renderer_frame(payload, payload_len);
    break;
}
```

**Outgoing (client → server):**

The native client **does not send binary frames** over the WS. All input goes via UDP:7100. The only WS traffic from client to server is **JSON control messages**:

```json
// On WS open, claim the slot:
{"type": "join", "id": "<player_id_from_token>", "name": "<name>", "device": "NOBD Native Client"}

// Optional periodic heartbeat (every 10s):
{"type": "ping", "t": <ms_timestamp>}

// On graceful shutdown:
{"type": "leave", "id": "<player_id_from_token>"}
```

The server may send any of these back:

```json
{"type": "status", ...}          // periodic lobby status, 1Hz
{"type": "assigned", "slot": 0}  // confirmation of slot claim
{"type": "pong", "t": <echo>}    // heartbeat response
{"type": "your_turn"}            // queue advancement (should not normally hit the native client — it already has a slot)
{"type": "match_end", "winner": 0, "loser": 1, "winner_name": "...", "loser_name": "..."}
{"type": "kicked", "reason": "match_lost"}  // server-side eviction — trigger shutdown
```

### 5.2 UDP to flycast `<home>:7100` — gamepad input

**Format (send-only):**

```
byte 0: LT  (u8, 0-255, analog left trigger)
byte 1: RT  (u8, 0-255, analog right trigger)
byte 2: buttons high byte (u8)
byte 3: buttons low  byte (u8)
```

The 16-bit button word (`buttons_hi << 8 | buttons_lo`) uses **active-low** encoding. Buttons released = 1, pressed = 0. So "no buttons pressed" = `0xFFFF`.

**Button bit layout** (match NOBD stick firmware):

```
bit  0:  C3     (button 3 — MVC2 "Medium Kick")
bit  1:  C2     (button 2 — MVC2 "Light Kick")
bit  2:  C1     (button 1 — MVC2 "Light Punch")
bit  3:  C4     (button 4 — MVC2 "Medium Punch")
bit  4:  UP
bit  5:  DOWN
bit  6:  LEFT
bit  7:  RIGHT
bits 8-15: reserved (keep as 1s)
```

**Additional buttons** (Hard Punch, Hard Kick, Start) are on the triggers:
- LT full = Assist 1 (MVC2 AA)
- RT full = Assist 2 (MVC2 AB)
- In MVC2, triggers map to special slots; the native client just passes the raw analog value and lets flycast map them.

**Send rate:** 1000Hz target. Poll SDL gamepad state, build the packet, send. Use `sendto()` on a pre-bound UDP socket. No ack, no retry.

**Connection tracking:** The server identifies clients by `srcIP:srcPort`. **Bind your local UDP socket to a fixed port** (any free ephemeral) and reuse it for the entire session so the server sees a stable source.

### 5.3 Deep Link `nobd://join?token=<base64url>`

**URL structure:**

```
nobd://join?token=<base64url(token_json)>

token_json = {
  "v": 1,                                 // version
  "player_id": "fighter_name_lowercase",
  "slot": 0,                              // 0 or 1
  "homeserver": "arcade.example.com",     // hostname or IP
  "ws_port": 7200,
  "udp_port": 7100,
  "expires_at": 1775500000,               // unix seconds
  "nonce": "hex32",                       // 16-byte random, for single-use tracking
  "hmac": "base64url_of_hmac_sha256"      // signature over all fields except "hmac"
}
```

**Signing** (in `relay/src/claim_api.rs`, new file):

```rust
fn sign_token(secret: &[u8], payload: &ClaimPayload) -> String {
    let canonical = serde_json::to_string(payload).unwrap(); // sorted keys
    let mut mac = HmacSha256::new_from_slice(secret).unwrap();
    mac.update(canonical.as_bytes());
    base64url_encode(mac.finalize().into_bytes())
}
```

**Verification** (in native client, `src/token.cpp`):

```cpp
bool verify_token(const std::string& token_b64, const std::string& secret, ClaimToken& out) {
    auto json_bytes = base64url_decode(token_b64);
    auto parsed = nlohmann::json::parse(json_bytes);
    std::string got_hmac = parsed["hmac"];
    parsed.erase("hmac");
    std::string canonical = parsed.dump(); // nlohmann sorts by default
    auto expected_hmac = base64url_encode(hmac_sha256(secret, canonical));
    if (!constant_time_compare(got_hmac, expected_hmac)) return false;
    if (parsed["expires_at"].get<int64_t>() < now_unix_seconds()) return false;
    out = ClaimToken::from_json(parsed);
    return true;
}
```

**The secret is shared between the relay and the native client.** This is a tradeoff:

- **Option A (chosen):** Ship the verification secret in the native client binary. Anyone who extracts it can forge tokens. Acceptable because the forged token only lets them connect to the flycast server, and the flycast server's slot limit is the real gate. Even a forged token can't bypass "someone else is playing slot 0".
- **Option B (rejected):** Native client calls `POST /api/verify-token` on the relay to validate. Adds 50-200ms to startup. Acceptable for production; skip for v1.

**Single-use enforcement:** The relay records the `nonce` in SurrealDB when issuing the token (table `claim_nonce`, TTL 10 minutes). When the native client connects to flycast and flycast forwards a `{type: "join"}` to its own logs, the relay's collector can notice a dupe nonce and... actually, this is hard because the flycast doesn't know about nonces. **Simpler approach:** accept that tokens are replayable within their 5-minute expiry. The real gate is slot availability on the flycast server. Document this tradeoff in the security section.

### 5.4 SurrealDB Schema Additions

Add to `web/schema.surql`:

```surql
-- ============================================================================
-- NATIVE CLIENT CLAIM TOKENS
-- Records each token issued by /api/claim-slot so we can:
--   1. Detect replay (same nonce redeemed twice)
--   2. Audit who claimed what slot and when
--   3. Expire stale tokens automatically
-- ============================================================================
DEFINE TABLE claim_nonce SCHEMAFULL;
DEFINE FIELD nonce       ON claim_nonce TYPE string;
DEFINE FIELD player_id   ON claim_nonce TYPE string;
DEFINE FIELD slot        ON claim_nonce TYPE int;
DEFINE FIELD issued_at   ON claim_nonce TYPE datetime;
DEFINE FIELD expires_at  ON claim_nonce TYPE datetime;
DEFINE FIELD redeemed_at ON claim_nonce TYPE option<datetime>;
DEFINE FIELD client_ip   ON claim_nonce TYPE option<string>;
DEFINE INDEX idx_claim_nonce_unique ON claim_nonce FIELDS nonce UNIQUE;

-- Background cleanup: delete nonces older than 1 day
-- (run periodically from collector or cron)
-- DELETE claim_nonce WHERE expires_at < time::now() - 1d;

-- ============================================================================
-- NATIVE CLIENT INSTALLATIONS
-- Optional: track which players have the native client installed, last
-- reported version, OS, etc. Populated by the client on first connect.
-- ============================================================================
DEFINE TABLE native_install SCHEMAFULL;
DEFINE FIELD player_id    ON native_install TYPE string;
DEFINE FIELD version      ON native_install TYPE string;
DEFINE FIELD os           ON native_install TYPE string;  -- "windows", "macos", "linux"
DEFINE FIELD first_seen   ON native_install TYPE datetime;
DEFINE FIELD last_seen    ON native_install TYPE datetime;
DEFINE FIELD launch_count ON native_install TYPE int DEFAULT 0;
DEFINE INDEX idx_native_install_player ON native_install FIELDS player_id UNIQUE;

-- ============================================================================
-- EXTEND PLAYER TABLE: native client preference
-- When true, the browser's "your_turn" handler launches the native client
-- instead of auto-joining via the browser.
-- ============================================================================
DEFINE FIELD prefers_native ON player TYPE bool DEFAULT false;
```

**Migration:** These are additive DDL statements — safe to apply to the live database with no downtime.

---

## 6. Phase Plan — Execute In This Order

Each phase has: goal, deliverables, blockers, acceptance test. **Do not start phase N+1 until phase N acceptance test passes.**

### Phase 1 — libnobdrenderer: portable static library (G1)

**Goal:** Build the existing WASM renderer sources as a native static library.

**Why first:** If the renderer doesn't build native, nothing else matters. This is the risk step. Front-load it.

**Deliverables:**
- `packages/renderer/CMakeLists.txt` grows a new target `nobdrenderer` (static library) alongside the existing WASM executable target.
- `packages/renderer/src/native_gl_context.cpp` — new file, replaces `wasm_gl_context.cpp` for the native build. Implements the same public functions: `wasm_gl_init()`, `wasm_gl_resize()`, `wasm_gl_destroy()`. **Keep the same names** so `wasm_bridge.cpp` doesn't need `#ifdef` dust. Internally uses SDL2.
  - Note: the file is named `wasm_gl_context.cpp` for historical reasons. For the native build, conditionally compile `native_gl_context.cpp` INSTEAD and leave `wasm_gl_context.cpp` out.
- CMake conditional:
  ```cmake
  if(EMSCRIPTEN)
      set(GL_CONTEXT_SRC src/wasm_gl_context.cpp)
  else()
      set(GL_CONTEXT_SRC src/native_gl_context.cpp)
      find_package(SDL2 REQUIRED)
  endif()
  ```
- New `packages/renderer/include/nobdrenderer.h` — public C API header exposing the existing `renderer_init()`, `renderer_sync()`, `renderer_frame()`, `renderer_resize()`, `renderer_destroy()`, `renderer_set_option()`, `renderer_get_option()`, `renderer_info()`. This is what downstream consumers include.
- Static library output at `packages/renderer/build-native/libnobdrenderer.a` (Linux) / `nobdrenderer.lib` (Windows) / `libnobdrenderer.a` (macOS).

**Blockers:**
- SDL2 must be a build dep. Add a `find_package(SDL2 REQUIRED)` guarded by `if(NOT EMSCRIPTEN)`. Document it in the renderer README.
- The `glsm_patched.c` file may have `#ifdef __EMSCRIPTEN__` branches — audit and make them no-ops for native.
- `wasm_gl_context.cpp:90-100` does `emscripten_webgl_create_context` — the native version must instead create an SDL2 window, call `SDL_GL_CreateContext`, then invoke `rglgen_resolve_symbols(sdl_get_proc_address_wrapper)` exactly the way the WASM version does.

**Key SDL2 boilerplate** (save as `packages/renderer/src/native_gl_context.cpp`):

```cpp
#ifndef __EMSCRIPTEN__

#include <SDL.h>
#include <SDL_opengles2.h>
#include <glsym/rglgen.h>
#include <cstdio>

static SDL_Window*    s_window  = nullptr;
static SDL_GLContext  s_context = nullptr;
static int            s_width   = 640;
static int            s_height  = 480;

extern void initSettings();
extern struct LibretroGraphicsContext theGLContext;
extern struct retro_hw_render_callback hw_render;

static void* wasm_get_proc_address(const char* name) {
    return SDL_GL_GetProcAddress(name);
}

static uintptr_t native_get_current_framebuffer(void) {
    return 0;  // default framebuffer = the SDL window
}

static void* native_retro_get_proc_address(const char* sym) {
    return SDL_GL_GetProcAddress(sym);
}

bool wasm_gl_init(int width, int height) {
    s_width  = width;
    s_height = height;
    initSettings();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[renderer] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    // Request GLES 3.0 (matches WebGL2). Fall back to GL 3.3 Core if unavailable.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    s_window = SDL_CreateWindow(
        "NOBD Arcade",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
    );
    if (!s_window) {
        fprintf(stderr, "[renderer] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    s_context = SDL_GL_CreateContext(s_window);
    if (!s_context) {
        fprintf(stderr, "[renderer] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_GL_MakeCurrent(s_window, s_context);
    SDL_GL_SetSwapInterval(0);  // vsync-off by default for overkill latency

    hw_render.get_current_framebuffer = native_get_current_framebuffer;
    hw_render.get_proc_address        = native_retro_get_proc_address;

    rglgen_resolve_symbols(wasm_get_proc_address);

    printf("[renderer] GL_VERSION:  %s\n", glGetString(GL_VERSION));
    printf("[renderer] GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    extern void setGraphicsContext(void*);
    setGraphicsContext(&theGLContext);
    theGLContext.init();

    glViewport(0, 0, width, height);
    return true;
}

void wasm_gl_resize(int width, int height) {
    s_width  = width;
    s_height = height;
    if (s_window) {
        SDL_SetWindowSize(s_window, width, height);
        SDL_GL_MakeCurrent(s_window, s_context);
        glViewport(0, 0, width, height);
    }
}

void wasm_gl_destroy() {
    if (s_context) { theGLContext.term(); SDL_GL_DeleteContext(s_context); s_context = nullptr; }
    if (s_window)  { SDL_DestroyWindow(s_window);                          s_window  = nullptr; }
    SDL_Quit();
}

int wasm_gl_get_width()  { return s_width;  }
int wasm_gl_get_height() { return s_height; }

// Exposed for main.cpp to drive the swap
SDL_Window* native_get_sdl_window() { return s_window; }

#endif // __EMSCRIPTEN__
```

**Acceptance test (G1):**
```bash
cd packages/renderer
mkdir -p build-native && cd build-native
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
ls -lh libnobdrenderer.a     # Should exist, ~2-5 MB
nm libnobdrenderer.a | grep -E '(renderer_init|renderer_frame|renderer_sync)'  # All present
```

### Phase 2 — Headless render test with canned frames (G2)

**Goal:** Prove the native renderer can replay a captured frame sequence and produce correct pixels.

**Deliverables:**
- Helper script to capture a SYNC + first 60 deltas from the running WASM client. Save as `packages/native-client/tests/canned_sync.bin` and `canned_deltas.bin`.
  - Easiest approach: add a JS dev hook in `web/js/render-worker.mjs` that when `?capture=1` is in the URL, dumps every received WS binary frame to a downloadable blob for 5 seconds. Remove after capture.
- `packages/native-client/tests/test_renderer.cpp`:
  ```cpp
  // Pseudo-outline
  int main() {
      wasm_gl_init(640, 480);  // opens real window for now
      auto sync = read_file("canned_sync.bin");
      renderer_sync(sync.data(), sync.size());

      auto deltas = read_file("canned_deltas.bin");
      // deltas is a concat of length-prefixed frames
      size_t off = 0;
      while (off < deltas.size()) {
          uint32_t len;
          memcpy(&len, deltas.data() + off, 4);
          off += 4;
          renderer_frame(deltas.data() + off, len);
          off += len;
          SDL_GL_SwapWindow(native_get_sdl_window());
          SDL_Delay(16);
      }
      return 0;
  }
  ```
- Side-by-side visual comparison: run the browser WASM with the same captured savestate, run `test_renderer` with the captured deltas, compare a single frame. They should be pixel-identical.

**Blockers:**
- `renderer_frame()` internally assumes the frame buffer is in WASM linear memory (that's how the browser version works — JS writes into `Module.HEAPU8` at `_frameBuf` offset). For the native path, we can just pass a raw `uint8_t*` because `wasm_bridge.cpp:678` already takes a pointer and length. **Verify this is true before starting Phase 2.** If `renderer_frame` expects the pointer to be a wasm heap offset, refactor to accept real pointers.
- Capturing frames: easiest is to add a temporary `onMessage` tap in the browser worker. Or, smarter: write a small Rust tool that connects to the relay as a normal client, reads 100 frames, and writes them to `test.bin` with 4-byte length prefixes. ~50 lines.

**Acceptance test (G2):**
```bash
cd packages/native-client
mkdir -p build && cd build
cmake ..
make test_renderer
./test_renderer ../tests/canned_sync.bin ../tests/canned_deltas.bin
# Window opens, shows MVC2 scene rendered identically to browser
```

### Phase 3 — Live WS client, direct to flycast (G3)

**Goal:** Native client connects to `ws://localhost:7200` (or configured host), receives live frames, and renders at 60fps.

**Deliverables:**
- `src/ws_client.cpp` — wraps libwebsockets or uwebsockets. Chosen: **libwebsockets** for portability (Windows + macOS + Linux without building from source).
  - Single callback: `on_receive(const uint8_t* buf, size_t len)`.
  - Internally runs a service thread that pumps the libwebsockets event loop.
  - Pushes received buffers into an SPSC queue using a fixed ring of `std::unique_ptr<uint8_t[]>` slots. **No locks on the hot path.** Main thread drains via `try_pop()`.
- `src/main.cpp` grows:
  ```cpp
  int main(int argc, char** argv) {
      parse_args(argc, argv);
      if (!wasm_gl_init(960, 720)) return 1;

      WsClient ws;
      ws.connect(config.host, config.ws_port, on_frame);
      // on_frame: push into queue

      bool running = true;
      while (running) {
          SDL_Event e;
          while (SDL_PollEvent(&e)) {
              if (e.type == SDL_QUIT) running = false;
              // ... gamepad, resize, etc.
          }

          // Drain up to N frames this tick (in case we fell behind)
          while (auto frame = ws.try_pop()) {
              process_frame(frame->data(), frame->size());
          }

          SDL_GL_SwapWindow(native_get_sdl_window());
      }

      wasm_gl_destroy();
      return 0;
  }
  ```
- **Frame drop policy:** If the queue has more than 2 pending frames when we start a tick, **drop the oldest** and render only the newest. This is the same policy as the browser — "newest wins". Document this in a comment.
- **Reconnect logic:** On WS close, wait 1 second, reconnect. Exponential backoff up to 5 seconds. Reset renderer state (clear `_glStateInit`) on reconnect because a new SYNC is coming.

**Blockers:**
- libwebsockets can be a pain to integrate. Alternative: use `cpp-httplib` + raw socket for the WS handshake since we only need a client, not a server. Third option: write a ~200-line WS client from scratch using OpenSSL for the TLS handshake. **Recommended: start with libwebsockets, fall back to hand-rolled if dependency hell hits.**
- SDL_PollEvent should be non-blocking; the loop above assumes vsync-off so it spins. For vsync-on mode, add `SDL_GL_SetSwapInterval(1)` and the swap will block on refresh.

**Acceptance test (G3):**
```bash
# Terminal 1: flycast running
./build/flycast /path/to/mvc2.gdi  # with MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1

# Terminal 2: native client
./nobd-arcade --host localhost --vsync off
# Window opens, MVC2 attract mode plays at 60fps, visual parity with browser
```

### Phase 4 — Gamepad input via UDP (G4)

**Goal:** Press a button on the native client's gamepad, see the character react in flycast.

**Deliverables:**
- `src/gamepad.cpp` — SDL2 GameController API wrapper. Maps SDL buttons to NOBD bit layout:
  ```cpp
  struct NobdPacket { uint8_t lt, rt, btn_hi, btn_lo; };

  NobdPacket poll_gamepad(SDL_GameController* gc) {
      uint16_t btn = 0xFFFF;  // all released

      // Face buttons
      if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A)) btn &= ~(1 << 2);  // C1 (LP)
      if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B)) btn &= ~(1 << 1);  // C2 (LK)
      if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X)) btn &= ~(1 << 3);  // C4 (MP)
      if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y)) btn &= ~(1 << 0);  // C3 (MK)

      // D-pad
      if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn &= ~(1 << 4);
      if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn &= ~(1 << 5);
      if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn &= ~(1 << 6);
      if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn &= ~(1 << 7);

      // Analog stick as d-pad fallback (deadzone 0.5)
      int16_t lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
      int16_t ly = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
      const int16_t DZ = 16384;  // 50% of INT16_MAX
      if (ly < -DZ) btn &= ~(1 << 4);
      if (ly >  DZ) btn &= ~(1 << 5);
      if (lx < -DZ) btn &= ~(1 << 6);
      if (lx >  DZ) btn &= ~(1 << 7);

      NobdPacket p;
      p.lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  >> 7;
      p.rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7;
      p.btn_hi = (btn >> 8) & 0xFF;
      p.btn_lo = btn & 0xFF;
      return p;
  }
  ```
- `src/udp_input.cpp` — bound UDP socket, `send_packet(NobdPacket)`. Non-blocking, drop on failure.
- UDP thread: spins at 1000Hz, reads the latest packet from an atomic snapshot (populated by the main thread's gamepad poll), sends over UDP. Use `std::atomic<uint32_t>` to hold the packet as a single 32-bit word so the snapshot is wait-free.
- **WebSocket JSON join:** After WS opens and before rendering starts, send:
  ```json
  {"type": "join", "id": "<from_token_or_guest_id>", "name": "<from_token_or_GUEST>", "device": "NOBD Native Client"}
  ```
- **On shutdown:** send `{"type": "leave", "id": "..."}` BEFORE closing the WS. Server uses this to eagerly clear the slot's `srcIP`.

**Blockers:**
- Windows needs `WSAStartup` before creating the UDP socket. Wrap in a cross-platform shim.
- Some users will have keyboards instead of gamepads. Add a keyboard fallback: map WASD to d-pad, JKL; UIO; to the six face buttons.
- macOS requires the binary to be signed for gamepad permissions to work without prompts. Defer signing to Phase 8.

**Acceptance test (G4):**
```bash
./nobd-arcade --host localhost --slot 0 --name GUEST
# Plug in a gamepad, press a button
# Flycast server logs: "[input-server] slot 0: packet from 127.0.0.1:xxxx"
# In-game character responds
```

### Phase 5 — Relay /api/claim-slot + token flow (G5)

**Goal:** Browser calls the relay, relay issues a signed token, native client validates it.

**Deliverables:**
- `relay/src/claim_api.rs` — new module. Adds `handle_claim_slot(body, db)` that:
  1. Parses `{username, slot, homeserver, session_token}` from request body.
     - `session_token` is the JWT from the existing `/api/signin` flow (already issued, already in the browser's `state.signinToken`).
  2. Verifies the session token against SurrealDB to confirm the user is logged in.
  3. Verifies the user is in slot N of the queue (query the flycast lobby state — either via a new server JSON message or by tracking queue state in the relay).
  4. Generates a `ClaimToken` with: `player_id`, `slot`, `homeserver`, `expires_at = now + 5min`, `nonce = random16`, `hmac`.
  5. Inserts the nonce into `claim_nonce` table for replay detection.
  6. Returns `{"ok": true, "deep_link": "nobd://join?token=..."}`.
- `relay/src/turn.rs` — add routing for `POST /api/claim-slot` in the HTTP listener (next to `/api/signin` / `/api/telemetry`).
- `relay/src/main.rs` — new env var `CLAIM_TOKEN_SECRET` (separate from `TURN_SECRET` so rotating one doesn't break the other).
- `packages/native-client/src/token.cpp` — implements verification as shown in § 5.3. The secret is baked in at compile time via a CMake option: `-DNOBD_CLAIM_SECRET=<hex>`. Use `OpenSSL` or `mbedTLS` for HMAC-SHA256.
- `web/schema.surql` — add the `claim_nonce` table (shown above).
- `web/js/queue.mjs` — new function `claimSlotAsNative()`:
  ```js
  export async function claimSlotAsNative(slot) {
      const res = await fetch('/api/claim-slot', {
          method: 'POST',
          headers: {
              'Content-Type': 'application/json',
              'Authorization': 'Bearer ' + state.signinToken,
          },
          body: JSON.stringify({
              username: state.playerProfile.username,
              slot: slot,
              homeserver: 'YOUR_HOME_IP_HERE',  // TODO: configurable
          }),
      });
      const data = await res.json();
      if (!data.ok) {
          alert('Claim failed: ' + data.error);
          return;
      }
      // Trigger the deep link
      window.location.href = data.deep_link;
  }
  ```
- `web/js/ws-connection.mjs` — modify the existing `your_turn` handler:
  ```js
  case 'your_turn':
      // If native client is installed and user preference is set, launch it.
      if (state.playerProfile?.prefers_native) {
          showNativeLaunchingModal();
          claimSlotAsNative(msg.slot);
      } else {
          // Existing browser auto-join path
          showNewChallenger(state.myName);
          ws.send(JSON.stringify({ type: 'join', id: state.myId, name: state.myName, device }));
      }
      break;
  ```

**Blockers:**
- The relay does not currently know the home server's IP. It has to be configured somewhere. Three options:
  1. **Env var** on the relay: `FLYCAST_HOME_IP=1.2.3.4`. Simple, single-home.
  2. **Registered per-player** in SurrealDB: add `home_server` field to `player` table. Complex, multi-home.
  3. **Hard-coded in the claim-slot call**: the browser sends the home IP (from a config file or URL param). Brittle.
- **Recommendation:** Option 1 for v1. The whole system assumes one arcade at one IP anyway. Document it. Add a `NOBD_HOME_IP` env var to the relay and bake it into tokens.
- Token secret must NOT be the same as `TURN_SECRET`. Generate with `openssl rand -hex 32`.
- **The flycast server does not currently tell the relay about the queue.** The relay only sees broadcast `status` messages from flycast. So "verify user is in slot N of the queue" (step 3 above) means parsing status messages. If the relay maintains a cached view of the last `status` message, this is easy. Add that cache in `fanout.rs`.

**Acceptance test (G5):**
```bash
# 1. Manually issue a token
curl -X POST https://nobd.net/api/claim-slot \
     -H 'Authorization: Bearer <session_token>' \
     -H 'Content-Type: application/json' \
     -d '{"username": "tris", "slot": 0, "homeserver": "home.example.com"}'
# Response: {"ok": true, "deep_link": "nobd://join?token=eyJ..."}

# 2. Extract the token and verify it in the native client
./nobd-arcade --token eyJ...
# Should print: "[token] valid, player_id=tris slot=0 expires_in=298s"
# Then proceed to connect to home.example.com:7200
```

### Phase 6 — Deep link protocol handler (G6, G7, G8)

**Goal:** Click `nobd://` URL in a browser, native client launches and auto-connects.

**Deliverables per platform:**

**Linux (G6):**
- `packages/native-client/resources/nobd.desktop`:
  ```
  [Desktop Entry]
  Type=Application
  Name=NOBD Arcade
  Comment=Native client for NOBD arcade streaming
  Exec=/usr/local/bin/nobd-arcade %U
  Icon=nobd
  Terminal=false
  Categories=Game;
  MimeType=x-scheme-handler/nobd;
  ```
- `packages/native-client/installer/linux/install.sh`:
  ```bash
  #!/bin/bash
  set -e
  install -Dm755 nobd-arcade        /usr/local/bin/nobd-arcade
  install -Dm644 resources/nobd.desktop ~/.local/share/applications/nobd.desktop
  install -Dm644 resources/icons/nobd.png ~/.local/share/icons/nobd.png
  update-desktop-database ~/.local/share/applications/
  xdg-mime default nobd.desktop x-scheme-handler/nobd
  echo "NOBD Arcade installed. Click any nobd:// link to launch."
  ```

**Windows (G7):**
- `packages/native-client/installer/windows/nobd.nsi`:
  ```nsi
  !include "MUI2.nsh"
  Name "NOBD Arcade"
  OutFile "nobd-arcade-installer.exe"
  InstallDir "$PROGRAMFILES64\NOBD"
  RequestExecutionLevel admin

  Section
      SetOutPath $INSTDIR
      File "nobd-arcade.exe"
      File "SDL2.dll"
      File "zstd.dll"

      ; Register protocol handler
      WriteRegStr HKCR "nobd" "" "URL:NOBD Arcade Protocol"
      WriteRegStr HKCR "nobd" "URL Protocol" ""
      WriteRegStr HKCR "nobd\DefaultIcon" "" "$INSTDIR\nobd-arcade.exe,0"
      WriteRegStr HKCR "nobd\shell\open\command" "" '"$INSTDIR\nobd-arcade.exe" "%1"'

      ; Start menu shortcut
      CreateDirectory "$SMPROGRAMS\NOBD"
      CreateShortcut "$SMPROGRAMS\NOBD\NOBD Arcade.lnk" "$INSTDIR\nobd-arcade.exe"

      WriteUninstaller "$INSTDIR\uninstall.exe"
  SectionEnd
  ```
- Use `makensis` (from NSIS, `sudo apt install nsis` or download on Windows).

**macOS (G8):**
- `packages/native-client/installer/macos/Info.plist`:
  ```xml
  <?xml version="1.0" encoding="UTF-8"?>
  <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
  <plist version="1.0">
  <dict>
      <key>CFBundleExecutable</key>
      <string>nobd-arcade</string>
      <key>CFBundleIdentifier</key>
      <string>gg.nobd.arcade</string>
      <key>CFBundleName</key>
      <string>NOBD Arcade</string>
      <key>CFBundleVersion</key>
      <string>1.0.0</string>
      <key>CFBundleShortVersionString</key>
      <string>1.0.0</string>
      <key>CFBundleIconFile</key>
      <string>nobd.icns</string>
      <key>LSMinimumSystemVersion</key>
      <string>11.0</string>
      <key>NSHighResolutionCapable</key>
      <true/>
      <key>CFBundleURLTypes</key>
      <array>
          <dict>
              <key>CFBundleURLName</key>
              <string>NOBD Arcade Protocol</string>
              <key>CFBundleURLSchemes</key>
              <array>
                  <string>nobd</string>
              </array>
          </dict>
      </array>
  </dict>
  </plist>
  ```
- `packages/native-client/installer/macos/build.sh`:
  ```bash
  #!/bin/bash
  set -e
  APP="NOBD Arcade.app"
  rm -rf "$APP"
  mkdir -p "$APP/Contents/MacOS"
  mkdir -p "$APP/Contents/Resources"
  cp nobd-arcade "$APP/Contents/MacOS/"
  cp Info.plist  "$APP/Contents/"
  cp resources/icons/nobd.icns "$APP/Contents/Resources/"
  # Build DMG
  hdiutil create -volname "NOBD Arcade" -srcfolder "$APP" -ov -format UDZO nobd-arcade.dmg
  ```

**Code in native client to parse the URL:**
```cpp
// src/main.cpp
int main(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("nobd://", 0) == 0) {
            cfg.deep_link = arg;
        } else if (arg == "--host" && i + 1 < argc) {
            cfg.host = argv[++i];
        } // ... etc
    }
    if (!cfg.deep_link.empty()) {
        // Parse "nobd://join?token=XYZ"
        auto token = extract_query_param(cfg.deep_link, "token");
        ClaimToken ct;
        if (!verify_token(token, CLAIM_SECRET, ct)) {
            show_error("Invalid or expired token");
            return 2;
        }
        cfg.host        = ct.homeserver;
        cfg.ws_port     = ct.ws_port;
        cfg.udp_port    = ct.udp_port;
        cfg.player_id   = ct.player_id;
        cfg.slot        = ct.slot;
    }
    // ... rest of main
}
```

**Acceptance tests (G6-G8):** Click `nobd://join?token=valid` from a browser on each platform → native client launches → connects → game visible. One click.

### Phase 7 — Browser integration (G10)

**Goal:** Full loop — browser queues player → match ends → deep link fires → native client plays → exit → back to browser.

**Deliverables:**
- `web/king.html` — new modal `#nativeLaunchModal`:
  ```html
  <div class="modal" id="nativeLaunchModal">
    <div class="modal-content">
      <h2>LAUNCHING NOBD CLIENT...</h2>
      <p>Your fight is ready. Launching the native client for lowest latency.</p>
      <p class="small">Nothing happening? <a href="/download">Install the client</a>.</p>
      <button onclick="closeNativeLaunchModal(); abortClaim();">CANCEL</button>
    </div>
  </div>
  ```
- `web/download.html` — new page with install instructions per OS, download links to GitHub Releases. Detect OS via `navigator.userAgent`, show the right download by default.
- `web/js/queue.mjs` — the `claimSlotAsNative()` function shown in Phase 5. On click of "LAUNCH NATIVE" button in the user's profile panel, set `state.playerProfile.prefers_native = true`.
- `web/js/auth.mjs` — add a toggle "Prefer native client" to the profile panel. Persists via a new `/api/set-native-pref` endpoint on the relay, which updates the `player.prefers_native` field in SurrealDB.
- **Graceful handoff:** when the native client opens, it sends `{type: "join", ...}` over WS:7200. The flycast server already emits a `status` message with the updated player list. The browser's existing status handler (`updateLobbyState()`) sees the user in a slot, which flips the UI to "PLAYING" mode. Already works.
- **Return to spectator:** when the native client closes, it sends `{type: "leave"}`. Flycast frees the slot, emits new status, browser sees the user is no longer playing, UI reverts. Already works.

**Critical edge case:** If the browser sent `{type: "your_turn"}` and the user is on a machine WITHOUT the native client installed, the `nobd://` click does nothing silently. Detection:
- Set a timeout: after calling `window.location.href = "nobd://..."`, wait 3 seconds. If we're still on the page (no navigation happened and no native client pinged back), assume it's not installed and fall back to the browser auto-join path.
- Implementation:
  ```js
  async function claimSlotAsNative(slot) {
      const data = await fetch('/api/claim-slot', ...).then(r => r.json());
      if (!data.ok) return fallbackToBrowser();

      const timeoutMs = 3000;
      const launchedAt = Date.now();
      const onVisibilityChange = () => {
          if (document.hidden) {
              // User navigated away — assume native client opened
              clearTimeout(fallback);
              document.removeEventListener('visibilitychange', onVisibilityChange);
          }
      };
      document.addEventListener('visibilitychange', onVisibilityChange);
      const fallback = setTimeout(() => {
          showInstallModal();
      }, timeoutMs);

      window.location.href = data.deep_link;
  }
  ```

**Acceptance test (G10):**
1. Browser loads https://nobd.net.
2. Sign in.
3. In profile, toggle "Prefer native client".
4. Queue up: click "I GOT NEXT".
5. Wait for current match to end.
6. Browser shows "LAUNCHING NOBD CLIENT..." modal.
7. Native client pops up within 1-2 seconds.
8. Play a match using a connected gamepad.
9. Match ends, flycast emits `match_end`, native client shuts down.
10. Browser returns to spectator view.
11. Leaderboard in SurrealDB reflects the match result (the collector already handles this).

### Phase 8 — Release engineering (packaging, CI, signing)

**Goal:** Publishing to GitHub Releases with three platform binaries.

**Deliverables:**
- `.github/workflows/native-client-release.yml`:
  - Triggered on tag `native-client-v*`.
  - Matrix: `ubuntu-22.04`, `windows-2022`, `macos-13`.
  - Each job:
    1. Checkout
    2. Install SDL2, zstd, OpenSSL, libwebsockets
    3. `cmake --build packages/native-client`
    4. Package (NSIS, DMG, or .deb)
    5. Upload artifact
  - Final job: creates GitHub Release with all three binaries attached.
- Code signing (optional for v1):
  - Windows: `signtool sign /f cert.pfx /p $PASS nobd-arcade-installer.exe`
  - macOS: `codesign --deep --sign "Developer ID Application: ..." "NOBD Arcade.app"`, then notarize via `xcrun notarytool submit`
  - Linux: no signing required for .deb or .AppImage

**Deliverable files:**
- `nobd-arcade-linux-x86_64.tar.gz` — tarball of binary + install.sh
- `nobd-arcade-windows-x86_64-installer.exe` — NSIS installer
- `nobd-arcade-macos-universal.dmg` — universal binary DMG

---

## 7. Security Review

### Threat model

| Threat | Mitigation |
|--------|------------|
| Token replay (re-using a stolen token within its 5-min expiry) | Single-use nonce tracked in `claim_nonce` table; second redemption rejected. Slot-level lock on the flycast server (only one player per slot anyway). |
| Forged tokens (attacker extracts HMAC secret from binary) | Accepted risk. Forged token only grants connection to flycast; if slot is taken, attacker is rejected. No privilege escalation. |
| Man-in-the-middle on `ws://` (unencrypted) | Not mitigated in v1. The home→native WS connection is plain `ws://` because the home server can't get a TLS cert for a residential IP. Production fix: stunnel or wss:// with a self-signed cert the native client pins. Document this. |
| DoS from spoofed UDP inputs | The flycast server binds inputs to `srcIP:srcPort`. An attacker can't hijack an existing slot without knowing the legitimate client's source port. They could still spam new source ports to trigger registration logic; flycast's `_webRegistering` mode is off by default so this is bounded. |
| Downgrade attack (user forced to WASM when native is available) | N/A — user choice, not a security boundary. |
| Binary trojans | Mitigated only by code signing and GitHub Releases reputation. Ship unsigned for dev, sign before wide distribution. |
| Leaked admin DB credentials via the client | Client never holds DB credentials. All SurrealDB access goes through the relay's `/api/*` endpoints. |

### Secrets inventory

| Secret | Where it lives | Who can access |
|--------|----------------|----------------|
| `CLAIM_TOKEN_SECRET` (HMAC key) | Relay env var, baked into native client binary via `-DNOBD_CLAIM_SECRET` at build time | Anyone who extracts the native client binary |
| `TURN_SECRET` | Relay env var only | Nobody on the client side |
| `NOBD_DB_PASS` (SurrealDB admin) | Relay env var only | Nobody on the client side |
| Session JWT (signed by SurrealDB) | Browser localStorage after signin | Authenticated user |
| `claim_nonce` records | SurrealDB `claim_nonce` table | Relay admin only |

### Rotation procedure

If `CLAIM_TOKEN_SECRET` is compromised (assume yes the moment a native client binary leaks):
1. Generate new secret: `openssl rand -hex 32`.
2. Update relay env var, restart `maplecast-relay`.
3. Old tokens instantly fail verification on the relay side (can't be issued).
4. Clients with old binaries can't verify NEW tokens — they need an update.
5. **This is a forced upgrade event.** Plan releases such that rotation happens at the same time as a version bump.

---

## 8. Observability — What To Measure

The existing `/api/telemetry` endpoint already accepts per-client reports from the browser. **The native client should POST to the same endpoint** with the same schema + a `client_type: "native"` field. Grafana dashboards get free native metrics.

New metrics:
- `nobd_client_by_type{type="native"}` — count of native clients connected right now
- `nobd_native_version{version="1.0.0"}` — version distribution
- `nobd_native_os{os="windows"}` — OS distribution

Add these in `relay/src/client_telemetry.rs` and `render_prometheus()`.

New Grafana panel: **"Native vs WASM split"** — pie chart of `nobd_client_count` by `client_type`. Shows adoption over time.

---

## 9. Non-Goals / Out of Scope

Explicit list of what this workstream does NOT deliver, so scope creep dies early:

- ❌ **Mobile native (iOS/Android).** Android can run the WASM client in Chrome, which is good enough. iOS can use Safari. Native mobile requires store submission, IAP plumbing, and a completely different UI layer. Post-v1.
- ❌ **Replay recording.** Native client only renders live streams. TA frame ring buffer for replays is a separate workstream.
- ❌ **Voice chat.** Spectators can use Discord. Players are focused on the match.
- ❌ **In-client leaderboard UI.** Leaderboards stay in the browser. Native client just has a close button and a "return to browser" link.
- ❌ **Multi-arcade support.** V1 assumes exactly one home server. Multi-arcade routing is a future problem.
- ❌ **Auto-update.** Native client checks for updates on startup (hits `GET https://nobd.net/api/latest-version`) and shows a banner. Doesn't download automatically. User clicks → GitHub Releases.
- ❌ **Reconnect-during-match resilience.** If the WS drops mid-match, the native client closes and lets the server's idle-kick handle it. Reconnect logic for active matches is a v2 feature.
- ❌ **Custom key bindings.** V1 uses SDL GameController's default mapping. Post-v1 adds a binding screen.
- ❌ **Latency visualization in the client.** Telemetry goes to Grafana; no in-client HUD.

---

## 10. Timeline Estimate (agent-scale)

Measured in "focused work sessions" for a competent agent with this doc as a guide. Humans multiply by 2-3x.

| Phase | Sessions | Cumulative |
|-------|----------|------------|
| 1. libnobdrenderer | 2-3 | 2-3 |
| 2. Canned frame test | 1 | 3-4 |
| 3. Live WS client | 2 | 5-6 |
| 4. Gamepad UDP | 1 | 6-7 |
| 5. Claim token flow | 2 | 8-9 |
| 6. Deep link handlers (all 3 OS) | 3 | 11-12 |
| 7. Browser integration | 2 | 13-14 |
| 8. Release engineering | 2 | 15-16 |

**Call it ~16 sessions, worst case 3 weeks of focused work.** The risk is all in Phase 1. If the renderer builds native on the first day, everything after is plumbing.

---

## 11. Rollback Plan

If any phase goes sideways:

- **Phase 1-2 fail:** The renderer can't be made portable. Fallback: ship the native client as a thin wrapper that embeds an Electron-like WebView running the existing WASM renderer. Kills the latency win but preserves the UX.
- **Phase 3 fails (WS integration):** Use raw TCP with a length prefix on the home server side (add a new server port). More invasive but works.
- **Phase 5 fails (token system):** Ship without deep links. Users manually enter `--host` and `--slot` on the command line. Power-user mode. Add deep links in v1.1.
- **Phase 6 fails (one OS):** Ship the working platforms. Linux first, Windows second, macOS last (hardest to get code signing sorted).
- **Phase 10 (G10) fails:** The loop has a hole somewhere. Log aggressively, find the gap, close it. Do NOT ship a half-working deep link flow — it'll train users to distrust it.

---

## 12. Post-v1 Roadmap

Once v1 ships and is stable:

1. **Reconnect resilience** — handle transient WS drops without losing the match
2. **Key bindings UI** — in-client remapping
3. **Replay capture** — save raw TA stream to disk, scrub later
4. **Mobile native** — starting with Android via the Flycast mobile shell
5. **Multi-arcade routing** — lookup service that tells the client which home server to connect to based on geo
6. **End-to-end encryption** — wss:// with pinned self-signed cert
7. **Spectator native mode** — same client, `--no-input`, connects to relay instead of home, for pro spectators who want vsync-off viewing
8. **Auto-update with delta patches** — bsdiff, no full reinstall

---

## 13. Checklist — Before Writing The First Line Of Code

- [ ] Read this entire doc, top to bottom.
- [ ] Read `docs/ARCHITECTURE.md` § "Five race conditions fixed" — understand the wire format invariants.
- [ ] Read `packages/renderer/src/wasm_bridge.cpp` — understand what `renderer_sync()` and `renderer_frame()` actually do.
- [ ] Read `relay/src/protocol.rs` — cross-reference the wire format section in this doc.
- [ ] Verify the prerequisites in § 2 are all green.
- [ ] Decide on libwebsockets vs hand-rolled WS client. Commit in writing which one.
- [ ] Capture a known-good SYNC + delta sequence from the WASM renderer for Phase 2 testing. Save to `packages/native-client/tests/`.
- [ ] Generate `CLAIM_TOKEN_SECRET` and put it in `.env.relay` (git-ignored). Document it in `docs/VPS-SETUP.md`.
- [ ] Create `packages/native-client/` directory skeleton as shown in § 4.
- [ ] Open a tracking issue / doc with Phase checkboxes.

Then — and only then — start Phase 1.

---

## END OF WORKSTREAM

**No shortcuts. No "good enough for now". This is the path.**

**OVERKILL IS NECESSARY.**
