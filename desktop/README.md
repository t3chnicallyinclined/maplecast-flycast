# MapleCast Desktop (Tauri shell)

A thin native desktop client for MapleCast. It opens a window on the **live
WebGPU client** (`https://nobd.net/webgpu-test.html`) and adds the one thing a
browser cannot: a **native controller → UDP `:7100` input path**, polled at
~1 kHz off the webview thread instead of the browser's rAF-gated Gamepad API
over a TCP WebSocket.

This is **Phase 1** of the native-client plan (see
[`docs/STATE-2026-07-13.md`](../docs/STATE-2026-07-13.md) → "native client"):
reuse the entire existing renderer unchanged, bank the native-input latency win,
zero renderer rewrite.

---

## Why a shell around the remote site (not a bundled app)

1. **ROM-safety.** `web/` contains ROM-derived sprite rips (gitignored,
   copyrighted). Bundling them into a downloadable binary is a *worse*
   distribution problem than committing them. So the app loads the client
   **remotely** — the rips stay server-side.
2. **WebGPU secure context.** `https://` is a secure context, so `navigator.gpu`
   works in WebView2 with no `file://` workarounds.
3. **Instant updates.** Push a web change to prod and every desktop client gets
   it on next launch — no app rebuild.

The native value-add is purely: native UDP input + deep-link launch + being an
installable, auto-updating low-latency client.

---

## The "click a link and play" model (deep links)

A native app can't literally be "click a URL → instantly playing" the way the
browser is — a desktop app means download → install → OS security prompt. The
reconciling pattern (Discord/Zoom/Steam) is a **custom URL scheme**:

```
maplecast://match/<id>?slot=1
```

- **App installed** → the OS launches (or focuses) MapleCast straight into that
  match. Handled here by `tauri-plugin-deep-link` + `tauri-plugin-single-instance`.
- **App not installed** → your existing web page is the fallback: it plays
  instantly in-browser and offers an "install for lower latency" download.

So the **browser stays the front door**; this app is the low-latency upgrade.

> Honest caveat: first install shows a Windows SmartScreen / macOS Gatekeeper
> warning unless the binary is **code-signed** (costs money / an Apple Developer
> account). Auto-update (below) is smooth *after* the one-time install.

---

## Prerequisites

- **Rust** (MSVC toolchain) — present on this box: `cargo 1.93.1`,
  host `x86_64-pc-windows-msvc`.
- **Visual Studio Build Tools** (the MSVC linker) — VS 2019/2022 detected. Run
  `cargo tauri dev` from a **"x64 Native Tools Command Prompt for VS"** (or any
  shell where `link.exe`/`cl.exe` are on PATH) so linking succeeds.
- **WebView2 Runtime** — present (Edge 150). End users get it via the Evergreen
  bootstrapper if missing.
- **Tauri CLI**:
  ```
  cargo install tauri-cli --version "^2"
  ```

## First-time setup

```
# 1) Generate app icons (Tauri needs these to bundle; dev may warn without them)
cd C:\Users\trist\projects\maplecast-flycast\desktop\src-tauri
cargo tauri icon C:\path\to\a\1024x1024\logo.png

# 2) Run it (dev) — loads https://nobd.net/webgpu-test.html?native=1
cd C:\Users\trist\projects\maplecast-flycast\desktop
cargo tauri dev
```

### Point it somewhere else (e.g. a local tunnel / OVH test box)

```
set MAPLECAST_URL=http://localhost:3000/webgpu-test.html
set MAPLECAST_INPUT_HOST=127.0.0.1
set MAPLECAST_INPUT_PORT=7100
set MAPLECAST_SLOT=0
cargo tauri dev
```

| Env var | Default | Meaning |
|---|---|---|
| `MAPLECAST_URL` | `https://nobd.net/webgpu-test.html` | Page the window loads |
| `MAPLECAST_INPUT_HOST` | host of `MAPLECAST_URL` | `:7100` UDP input server host |
| `MAPLECAST_INPUT_PORT` | `7100` | UDP input port |
| `MAPLECAST_SLOT` | `0` | Player slot this client drives (0 = P1, 1 = P2) |

---

## The input wire (CONFIRMED byte-exact)

Native controller → `"PC"` 11-byte UDP packet to `host:7100`. Verified against
the server parser (`core/network/maplecast_input_server.cpp:1237`) and the
native reference sender (`core/network/maplecast_input_sink.cpp:111-138`). See
[`src-tauri/src/input.rs`](src-tauri/src/input.rs) `build_input_packet`.

```
off sz field    endian   note
 0   2  "PC"     -        0x50 0x43  (a remote client MUST use this; the 5-byte /
                                      0x49 forms are loopback-only, rejected :1225)
 2   1  slot     -        0=P1, 1=P2 (server auto-binds slot→src IP:port, FCFS)
 3   4  seq      LITTLE   strictly monotonic per source; seq<=last => DROPPED
 7   1  LT       -        0..255 analog trigger (assist A1)
 8   1  RT       -        0..255 analog trigger (assist A2)
 9   1  btn_hi   BIG      high byte of active-low 16-bit DreamcastKey mask
10   1  btn_lo   BIG      low byte
```

Button bits (`core/input/gamepad.h`, active-low, bit clear = pressed):
`A=0x0004 B=0x0002 X=0x0400 Y=0x0200 Start=0x0008 U/D/L/R=0x10/0x20/0x40/0x80`.
Neutral = `0xFFFF`. The gamepad→bit mapping mirrors `web/js/gamepad.mjs:149-162`.

> **`MAPLECAST=1` gate:** the server only binds `:7100` when the `MAPLECAST` env
> var is set (`core/emulator.cpp:1365`). Without it, input is silently dropped.
> This has cost hours before — verify the target server has it.

---

## Required companion change (before real use)

The desktop app polls the pad natively **and** the page still runs its own
browser gamepad→WebSocket sender → **input is sent twice**. Add a one-line guard
in [`web/js/gamepad.mjs`](../web/js/gamepad.mjs) (or where polling starts) to
**skip `startGamepadPolling()` when the URL has `?native=1`**. This app appends
`native=1` to every URL it loads for exactly this purpose.

---

## The "play the opponent" gap (session / slot binding)

Investigated against the current code — **honest status**:

- **There is no shared match-token concept today.** Slot assignment is
  **first-come-first-served**: whoever joins the flycast WS first is P1, second
  is P2 (`maplecast_input_server.cpp registerPlayer :1730-1780`). The `"PC"`
  packet just *asserts* its slot in byte 2 — and there is **no auth**
  (`:1372`), last-writer-wins, so a browser and this app both claiming the same
  slot will **fight over it**.
- The **hub** (Pillar 5, `hub/src/queue.rs`) already computes the right pairing
  (first queued = slot 0, second = slot 1) and picks the lowest-RTT server, but
  it hands each player a **per-player** token, not a shared room id.

So to make `maplecast://match/<id>` mean "you are P2 on server X":
1. Add a shared **match/room record** — the hub's `Matched{server, slot, partner}`
   state is the natural place to mint and store a shared `<id>`.
2. The link carries `slot`; this client already parses `?slot=` and puts it in
   the packet's byte 2 (`match_url_from_deep_link` in `input.rs`).
3. Ideally add a signed token so the slot claim is authenticated before `:7100`
   is exposed to arbitrary internet clients.

This is the main **new server-side work** the "send a link, play the opponent"
vision needs. The client scaffold is ready for it.

---

## Latency: what this buys, what it doesn't

- **Banks the native-input win** — ~1 kHz native poll + raw UDP removes the
  browser's ~8 ms rAF Gamepad sampling and the TCP/TLS/nginx input hop.
- **Does NOT change the present path** — the WebGPU canvas still presents through
  Chromium's compositor (a co-dominant ~1 frame tax). Escaping that needs a
  native `wgpu`+`winit` renderer (Phase 3), which is a real rewrite.

---

## Not yet wired (deliberate TODOs)

- **Icons** — run `cargo tauri icon` (gitignored; not committed as binaries).
- **Auto-update** — needs a signing keypair (`cargo tauri signer generate`) and a
  hosted `latest.json`; add the `updater` plugin + `plugins.updater` config once
  there's a release host. Do NOT commit the private key.
- **Code signing** — for a warning-free install.
- **Redundant send** — the native reference sends each packet twice (T+0, T+1ms)
  for loss tolerance (server dedups on seq). This scaffold sends once.
- **ACK/RTT** — the server ACKs each packet with `[0xFE][seq][ts]`; unused here
  (could drive an E2E latency readout).
- **Hot-swap pad** — picks the first connected pad; the browser walks all 4 by
  recency.
