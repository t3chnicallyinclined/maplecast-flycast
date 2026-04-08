# MapleCast

**MapleCast turns [Flycast](https://github.com/flyinghead/flycast) into a real-time game streaming server.** One instance of MVC2 (or any Dreamcast/Naomi game) runs on a server somewhere; players and spectators connect from a browser; the server streams the game to everyone at 60 fps using **raw GPU command buffers** instead of compressed video. The result is **pixel-perfect, low-bandwidth (~4 Mbps), low-latency** streaming with native input from real fight sticks or browser gamepads.

It powers [**nobd.net**](https://nobd.net) — an always-on Marvel vs. Capcom 2 cab anyone can spectate or sit down at via a web browser.

## What makes MapleCast different from regular emulator streaming

| | Traditional approach (H.264 / Twitch / Parsec) | MapleCast |
|---|---|---|
| **What's on the wire** | Encoded video frames | Raw TA command buffers + VRAM page diffs |
| **Bandwidth (60 fps MVC2)** | 25–50 Mbps | **~4 Mbps** in match, ~900 Kbps idle |
| **Quality** | Lossy, encoder artifacts on fast motion | **Pixel-perfect, deterministic, byte-identical to native** |
| **Server CPU/GPU** | GPU encoder needed (NVENC etc.) | **Pure CPU, no GPU at all** — runs on a $5/mo VPS |
| **Client** | Video decoder | WebAssembly renderer that replays the GPU commands in WebGL2 |
| **Input latency** | Encoder + decoder + frame delay | Just network RTT + one frame |

The trick is that the server doesn't need to render anything — it captures the raw display lists the game is sending to the GPU and ships them. The browser has a copy of the same renderer (compiled to WebAssembly) and reproduces the frame locally. The wire is **byte-perfect deterministic** end-to-end, verified by a continuous-integration rig that compares server-produced TA buffers against client-received TA buffers down to the byte.

## Highlights

- **Headless build** — `cmake -DMAPLECAST_HEADLESS=ON` produces a 27 MB Flycast binary with **zero GPU/SDL/X11/Vulkan/audio linkage**, runnable on a CPU-only VPS. See [`docs/WASM-BUILD-GUIDE.md`](docs/WASM-BUILD-GUIDE.md) and [`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md).
- **Standalone WebAssembly renderer** — pixel-perfect MVC2 in a browser canvas at 60 fps, ~750 KB compiled. Lives in [`packages/renderer/`](packages/renderer/).
- **Rust spectator relay** — fans the WebSocket stream out to up to 500 concurrent browser viewers per instance, with a SYNC cache so late joiners get instant initial state. Lives in [`relay/`](relay/).
- **Dual-policy input latch** — players choose between LATENCY (lowest delay, can drop very-fast taps) and CONSISTENCY (preserves every press even if it lands between two frame reads). Per-player preference, live A/B toggle. Full design doc in [`docs/INPUT-LATCH.md`](docs/INPUT-LATCH.md).
- **253-byte game state extraction** — reverse-engineered MVC2's RAM into a 253-byte-per-frame snapshot containing health, combos, meter, characters, and timer. Powers the live ELO/leaderboard system. See [`docs/MVC2-MEMORY-MAP.md`](docs/MVC2-MEMORY-MAP.md).

## Documentation

| File | What |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | The big picture — system topology, wire format, frame flow, what runs where |
| [`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md) | How to build and run the headless flycast on your own hardware |
| [`docs/INPUT-LATCH.md`](docs/INPUT-LATCH.md) | The dual-policy input latch model, the dashing bug it fixes, and the per-user preference system |
| [`docs/MVC2-MEMORY-MAP.md`](docs/MVC2-MEMORY-MAP.md) | The 253-byte RAM autopsy of MVC2 |
| [`docs/STREAMING-OPTIONS.md`](docs/STREAMING-OPTIONS.md) | Why TA-mirror streaming beats H.264 for this use case |
| [`docs/WASM-BUILD-GUIDE.md`](docs/WASM-BUILD-GUIDE.md) | Building the standalone WebAssembly renderer |
| [`docs/NAOMI-SPIKE.md`](docs/NAOMI-SPIKE.md) | Current branch: validating the Naomi arcade cart as the production target |

## License

Same as upstream Flycast: **GPL-2.0**. See [`LICENSE`](LICENSE). Every part of MapleCast — the headless server modifications, the standalone WASM renderer, the Rust relay, the browser UI — ships under GPL-2.0 to match.

---

## About the upstream emulator

**This repository is a fork of [Flycast](https://github.com/flyinghead/flycast)**, which is a multi-platform Sega Dreamcast, Naomi, Naomi 2, and Atomiswave emulator derived from [reicast](https://github.com/skmp/reicast-emulator). Flycast is the platform MapleCast builds on; without it, none of this would exist. Configuration and feature documentation for the upstream emulator lives on [TheArcadeStriker's flycast wiki](https://github.com/TheArcadeStriker/flycast-wiki/wiki). Their [Discord server](https://discord.gg/X8YWP8w) is the canonical community for the emulator itself.

[![Android CI](https://github.com/flyinghead/flycast/actions/workflows/android.yml/badge.svg)](https://github.com/flyinghead/flycast/actions/workflows/android.yml)
[![C/C++ CI](https://github.com/flyinghead/flycast/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/flyinghead/flycast/actions/workflows/c-cpp.yml)
[![Nintendo Switch CI](https://github.com/flyinghead/flycast/actions/workflows/switch.yml/badge.svg)](https://github.com/flyinghead/flycast/actions/workflows/switch.yml)
[![Windows UWP CI](https://github.com/flyinghead/flycast/actions/workflows/uwp.yml/badge.svg)](https://github.com/flyinghead/flycast/actions/workflows/uwp.yml)
[![BSD CI](https://github.com/flyinghead/flycast/actions/workflows/bsd.yml/badge.svg)](https://github.com/flyinghead/flycast/actions/workflows/bsd.yml)

<img src="shell/linux/flycast.png" alt="flycast logo" width="150"/>

## Install

### Android ![android](https://flyinghead.github.io/flycast-builds/android.jpg)
Install Flycast from [**Google Play**](https://play.google.com/store/apps/details?id=com.flycast.emulator).
### Flatpak (Linux ![ubuntu logo](https://flyinghead.github.io/flycast-builds/ubuntu.png))

1. [Set up Flatpak](https://www.flatpak.org/setup/).

2. Install Flycast from [Flathub](https://flathub.org/apps/details/org.flycast.Flycast):

`flatpak install -y org.flycast.Flycast`

3. Run Flycast:

`flatpak run org.flycast.Flycast`

### Homebrew (MacOS ![apple logo](https://flyinghead.github.io/flycast-builds/apple.png))

1. [Set up Homebrew](https://brew.sh).

2. Install Flycast via Homebrew:

`brew install --cask flycast`

### iOS

Due to persistent harassment from an iOS user, support for this platform has been dropped. 

### Xbox One/Series ![xbox logo](https://flyinghead.github.io/flycast-builds/xbox.png)

Grab the latest build from [**the builds page**](https://flyinghead.github.io/flycast-builds/), or the [**GitHub Actions**](https://github.com/flyinghead/flycast/actions/workflows/uwp.yml). Then install it using the **Xbox Device Portal**.

### Binaries ![android](https://flyinghead.github.io/flycast-builds/android.jpg) ![windows](https://flyinghead.github.io/flycast-builds/windows.png) ![linux](https://flyinghead.github.io/flycast-builds/ubuntu.png) ![apple](https://flyinghead.github.io/flycast-builds/apple.png) ![switch](https://flyinghead.github.io/flycast-builds/switch.png) ![xbox](https://flyinghead.github.io/flycast-builds/xbox.png)

Get fresh builds for your system [**on the builds page**](https://flyinghead.github.io/flycast-builds/).

**New:** Now automated test results are available as well. 

### Build requirements (Linux):

- **C/C++ compiler toolchain** (e.g. `gcc`/`g++`)
- **CMake**
- **make**
- **libcurl** (development headers)
- **libudev** (development headers)
- **SDL2** (development headers)
- **Graphics API**: Vulcan, OpenGL

### Build instructions:
```
$ git clone --recursive https://github.com/flyinghead/flycast.git
$ cd flycast
$ mkdir build && cd build
$ cmake ..
$ make
```
