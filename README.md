# MapleCast

**Pixel-perfect Dreamcast streaming. No server GPU required.**

MapleCast turns [Flycast](https://github.com/flyinghead/flycast) into a real-time game streaming server. Run MVC2 on your laptop, a spare box, or a cheap VPS — the server is pure CPU, no GPU needed. Browsers connect and watch or play at 60 fps. Instead of encoding video, the server streams raw GPU command buffers and the browser replays them locally, producing **byte-identical frames** to a native Dreamcast. The rendering happens in the browser, so even a Chromebook on your LAN can play.

> **Note:** MapleCast has only been tested and tuned for Marvel vs. Capcom 2. The TA streaming approach is Dreamcast-general in theory, but the wire format, game state extraction, skin system, and renderer have all been built around MVC2. Other games may or may not work.

It powers [**nobd.net**](https://nobd.net) — an always-on MVC2 cab anyone can spectate or sit down at from a browser.

| | Video streaming (H.264 / Twitch / Parsec) | MapleCast |
|---|---|---|
| **On the wire** | Encoded video frames | Raw TA command buffers + VRAM diffs |
| **Bandwidth** | 25-50 Mbps | **~4 Mbps** in match, ~900 Kbps idle |
| **Quality** | Lossy, artifacts on fast motion | **Pixel-perfect, deterministic** |
| **Server GPU** | Yes — hardware encoder (NVENC etc.) | **None** — pure CPU |
| **Client** | Video decoder | Any browser with WebGPU/WebGL2 — a Chromebook on your LAN works fine |

---

## Getting started

### What you need

- **Linux, Windows, or macOS** — Linux is the most tested. Windows works natively (full GPU build) or via WSL2 (headless build). See Step 2 for setup.
- **An MVC2 ROM** in GDI format (not included — you'll need to provide your own).
- **Chrome or Edge** — for viewing. Firefox WebGPU support is still experimental.

### Step 1: Clone the repo

```bash
git clone --recursive https://github.com/t3chnicallyinclined/maplecast-flycast
cd maplecast-flycast
```

### Step 2: Install dependencies

<details>
<summary><b>Ubuntu / Debian (native Linux)</b></summary>

```bash
./scripts/install-deps.sh
```

This installs cmake, g++, pkg-config, libcurl, zlib, zstd, udev, and Rust (via rustup).

Or manually:
```bash
sudo apt-get install -y build-essential cmake pkg-config git \
    libcurl4-openssl-dev zlib1g-dev libzstd-dev libudev-dev

# Rust (for the relay)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env
```

</details>

<details>
<summary><b>Windows (native)</b></summary>

You can build and run the full (non-headless) flycast with Visual Studio. This is the GPU build — it opens a window and renders locally, then streams to browsers. Performance is slightly higher than headless since your GPU handles rendering, but the server isn't as lightweight.

1. Install [Visual Studio 2022](https://visualstudio.microsoft.com/) with the **C++ desktop development** workload
2. Install [Rust](https://rustup.rs/) (for the relay)
3. Install [CMake](https://cmake.org/download/) (or use the one bundled with VS)

```powershell
git clone --recursive https://github.com/t3chnicallyinclined/maplecast-flycast
cd maplecast-flycast

# Build the server (full GPU build — not headless)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# Build the relay
cd relay
cargo build --release
```

Run flycast normally, minimize it, and point your browser at the relay.

</details>

<details>
<summary><b>Windows (via WSL2 — headless)</b></summary>

If you want the headless (no GPU) build on Windows, use WSL2:

```powershell
# In PowerShell (as admin)
wsl --install -d Ubuntu
```

Restart, open the Ubuntu terminal, then:

```bash
git clone --recursive https://github.com/t3chnicallyinclined/maplecast-flycast
cd maplecast-flycast
./scripts/install-deps.sh
```

Everything from here on works the same as native Linux. Open **http://localhost:8080/webgpu-test.html** in Chrome on your Windows side — WSL2 forwards the ports automatically.

</details>

<details>
<summary><b>macOS</b></summary>

```bash
brew install cmake pkg-config curl zstd
# Rust (for the relay)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env
```

The headless server builds on macOS but is less tested than Linux.

</details>

### Step 3: Build and run

Open three terminals:

**Terminal 1 — Game server** (builds the headless flycast binary, then runs it):
```bash
./scripts/quickstart.sh server ~/path/to/mvc2.gdi
```

**Terminal 2 — Stream relay** (builds the Rust relay, then runs it):
```bash
./scripts/quickstart.sh relay
```

**Terminal 3 — Web renderer** (starts a local file server, no build needed):
```bash
./scripts/quickstart.sh webgpu
```

### Step 4: Open the browser

Go to **http://localhost:8080/webgpu-test.html** in Chrome.

You should see MVC2 running at 60fps. Plug in a gamepad and play.

> **Shortcut:** `./scripts/quickstart.sh all ~/path/to/mvc2.gdi` builds and runs everything in one terminal.

---

## Scripts

| Script | What it does |
|--------|--------------|
| `scripts/install-deps.sh` | Installs all build dependencies (apt + rustup) |
| `scripts/quickstart.sh server <rom>` | Builds the headless C++ server and runs it |
| `scripts/quickstart.sh relay` | Builds the Rust relay and runs it |
| `scripts/quickstart.sh webgpu` | Serves the WebGPU renderer (no build — pure JS) |
| `scripts/quickstart.sh all <rom>` | All of the above in one command |
| `packages/renderer/build.sh` | Builds the WASM renderer (needs Emscripten SDK) |
| `deploy/scripts/deploy-web.sh <host>` | Deploys web files to a remote server (with backup) |
| `deploy/scripts/deploy-headless.sh <host>` | Deploys the server binary to a remote server |

---

## Building manually

If you want more control than the quickstart script, here are the raw commands.

### Headless server (C++)

```bash
cmake -B build-headless -DMAPLECAST_HEADLESS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-headless -j$(nproc)

# Run it
./build-headless/flycast ~/path/to/mvc2.gdi
```

The `-DMAPLECAST_HEADLESS=ON` flag disables all GPU/SDL/X11/audio linkage. The output is a ~27 MB binary that runs on anything with a CPU.

### Relay (Rust)

```bash
cd relay
cargo build --release

# Run it
./target/release/maplecast-relay \
    --ws-upstream ws://127.0.0.1:7210 \
    --ws-listen 0.0.0.0:7201 \
    --http-listen 127.0.0.1:7202 \
    --no-webtransport
```

### WebGPU renderer (no build)

The WebGPU renderer is pure JavaScript — no build step at all. Just serve the `web/` directory:

```bash
cd web && python3 -m http.server 8080
```

Open **http://localhost:8080/webgpu-test.html**. Edit any `.mjs` file, refresh, see changes.

### WASM renderer (Emscripten)

Only needed if you want to work on the production `king.html` renderer. Most development uses the WebGPU renderer above.

```bash
# Install Emscripten first (one-time)
cd ~ && git clone https://github.com/emscripten-core/emsdk.git
cd emsdk && ./emsdk install latest && ./emsdk activate latest
source ~/emsdk/emsdk_env.sh

# Build
cd /path/to/maplecast-flycast/packages/renderer
./build.sh
```

### Desktop client (full GPU build)

Standard flycast with rendering — for local play or development:

```bash
# Needs additional deps: SDL2, OpenGL
sudo apt-get install -y libsdl2-dev libgl-dev

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/flycast
```

---

## How it connects

```
 Browser (Chrome)
 http://localhost:8080/webgpu-test.html
    │
    │ ws://localhost:7201
    ▼
 relay (:7201)  ←──ws──  flycast headless (:7210)
                          ├─ SH4 emulation (game logic)
                          ├─ TA capture + zstd compress
                          ├─ Input server :7100 (UDP)
                          └─ Audio :7213
```

Three processes on localhost. Same architecture scales to production — just add nginx + TLS in front.

---

## What's in the repo

```
maplecast-flycast/
├── core/network/maplecast_*.cpp   Headless server — TA capture, input, audio, game state
├── relay/                         Rust fan-out relay (WebSocket + WebTransport)
├── web/
│   ├── webgpu-test.html           WebGPU renderer — pure JS, no build, fastest dev loop
│   ├── webgpu/                    WebGPU modules (parser, renderer, effects, transport)
│   ├── king.html                  Production WASM+WebGL2 renderer (needs build)
│   └── js/                        Shared JS modules (gamepad, state, WS connection)
├── packages/renderer/             WASM renderer build (Emscripten)
├── scripts/
│   ├── install-deps.sh            Install all build dependencies
│   └── quickstart.sh              Build + run any component in one command
├── deploy/                        Production deploy scripts (backup + rollback)
└── docs/                          Architecture, wire format, input latch, memory map
```

---

## Documentation

| Doc | What |
|-----|------|
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | System topology, wire format, frame flow, the six regression bugs |
| [DEVELOPER-GUIDE.md](docs/DEVELOPER-GUIDE.md) | All five components explained, build targets, key files, how they connect |
| [WEBGPU-RENDERER.md](docs/WEBGPU-RENDERER.md) | WebGPU renderer deep dive — parser, shaders, effects pipeline |
| [DEPLOYMENT.md](docs/DEPLOYMENT.md) | Building and deploying to a remote server |
| [INPUT-LATCH.md](docs/INPUT-LATCH.md) | Dual-policy input latch (latency vs consistency) |
| [SKIN-SYSTEM.md](docs/SKIN-SYSTEM.md) | Live palette swap system — 5,200 community skins |
| [MVC2-MEMORY-MAP.md](docs/MVC2-MEMORY-MAP.md) | 253-byte per-frame RAM extraction |
| [WASM-BUILD-GUIDE.md](docs/WASM-BUILD-GUIDE.md) | Building the WASM renderer (Emscripten) |
| [STREAMING-OPTIONS.md](docs/STREAMING-OPTIONS.md) | Why TA mirror streaming beats H.264 |

---

## Highlights

- **Headless build** — 27 MB binary, zero GPU/SDL/X11 linkage. Runs anywhere.
- **WebGPU renderer** — pure JavaScript, no compile step. 15+ post-processing effects, 3D arena backgrounds, live diagnostics. Edit and refresh.
- **WASM renderer** — pixel-perfect WebGL2, ~750 KB compiled. The production renderer on nobd.net.
- **Rust relay** — fans WebSocket stream to 500 concurrent viewers. SYNC cache for instant late-joiner state. WebTransport (QUIC/UDP) support.
- **253-byte game state** — reverse-engineered MVC2 RAM into per-frame health, combos, meter, characters, timer. Powers live ELO and leaderboards.
- **5,200 community skins** — live PVR palette swaps visible to all viewers. One click, instant color change, no ROM hack.
- **Dual-policy input** — per-player choice between lowest-latency and every-press-guaranteed modes.
- **AF_XDP zero-copy input** — kernel-bypass UDP for hardware fight sticks at 12 KHz.

---

## License

GPL-2.0, same as upstream Flycast. See [LICENSE](LICENSE).

This is a fork of [Flycast](https://github.com/flyinghead/flycast), a multi-platform Sega Dreamcast/Naomi/Atomiswave emulator derived from [reicast](https://github.com/skmp/reicast-emulator). Upstream docs: [flycast wiki](https://github.com/TheArcadeStriker/flycast-wiki/wiki). Community: [Discord](https://discord.gg/X8YWP8w).
