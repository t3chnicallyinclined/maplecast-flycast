# Deploying MapleCast

This is the public-facing deployment overview. It tells you how to build and run MapleCast / nobd.net's flycast variants on your own hardware. Operator-specific runbooks (live VPS credentials, admin panel internals, business roadmap) live in a separate private repo and are not published here.

## What you can build from this repo

| Variant | Build target | What it is |
|---|---|---|
| **headless flycast** (recommended for servers) | `cmake -DMAPLECAST_HEADLESS=ON -B build-headless && cmake --build build-headless` | CPU-only mirror server. No GPU, no SDL, no X11, no audio. ~26 MB stripped binary. Runs MVC2 + TA mirror streaming on a $5/month VPS. See [WORKSTREAM-HEADLESS-SERVER.md](WORKSTREAM-HEADLESS-SERVER.md). |
| **GPU flycast** (for local/cab play) | `cmake -B build && cmake --build build` | Standard flycast with full rendering. Used at a physical cab or for local LAN play with sub-millisecond input latency. |
| **WASM renderer** (browser viewer) | `cd packages/renderer && bash build.sh` | Standalone WebAssembly renderer that consumes the TA mirror stream and draws MVC2 in a browser canvas. See [WASM-BUILD-GUIDE.md](WASM-BUILD-GUIDE.md). |

## Architecture overview

Read [ARCHITECTURE.md](ARCHITECTURE.md) for the full mental model. Short version:

1. A flycast instance runs MVC2 and emits a deterministic byte-perfect stream of raw GPU commands (TA buffers) + VRAM page diffs over a WebSocket.
2. A Rust relay fans the stream out to N browser clients.
3. Browsers run the WASM renderer to draw the stream pixel-perfect at 60 fps.
4. Players send gamepad input back over the same WebSocket connection (or via direct UDP for hardware sticks).

## Runbook for your own deployment

The high-level recipe:

1. **Build the headless binary** (see table above).
2. **Install the systemd unit** from `deploy/systemd/maplecast-headless.service` (template provided in this repo).
3. **Drop your MVC2 ROM** at the path the unit expects (default `/opt/maplecast/roms/mvc2.gdi`).
4. **Optionally**: install the [Rust relay](../relay/) for browser fan-out.
5. **Optionally**: serve the static `web/` directory under nginx for the King of Marvel UI.

The deploy script at `deploy/scripts/deploy-headless.sh` automates the binary build + install + systemd dance against a remote host you have SSH access to. Read it before running it — it does an `ldd` sanity check to make sure your build doesn't accidentally link `libGL`/`libSDL`/etc.

## What's NOT in this repo

- **Operator credentials and the live VPS configuration for nobd.net** — these are operator-private and live in a separate repo.
- **The /overlord admin panel internals** — the admin panel exists at https://nobd.net/overlord but its endpoint map and auth flow are not published.
- **PYQU (putyourquarterup.com) product/business roadmap** — that's a separate product layer being developed alongside MapleCast.

If you're an authorized operator who needs the private docs, contact the maintainer. If you're a contributor who wants to set up your own deployment, this file plus the public docs in `docs/` should be enough — open an issue if anything is missing.
