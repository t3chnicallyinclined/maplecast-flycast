# MapleCast native client — port plan

Fresh, clean native client. **We do not reinvent the renderer** — we re-host the
*working* WebGPU renderer natively via `wgpu` (same WGSL shaders, no browser).

## Why this shape
- The **WebGPU browser renderer works** (prod, `web/webgpu-test.html`). The
  native flycast/`render_frame`-in-OpenGL client never reached 100% — abandoned.
- `wgpu` (Rust) runs the **same WGSL** the browser uses, on native Vulkan/D3D12/Metal.
  So the shaders + pipeline port over; only the JS *glue* is reimplemented.
- **rustls gives us direct `wss://play.nobd.net`** — the TLS gap that blocked the
  C++ flycast client (its build dropped OpenSSL) simply doesn't exist here.
- Goal: **extreme low latency, low bandwidth, native, direct to nobd.**

## What is REUSED (not rewritten)
| Piece | Working source | How it comes over |
|---|---|---|
| Render shaders | `web/webgpu/pvr2-renderer.mjs` (WGSL) | Copy WGSL ~verbatim into wgpu pipelines |
| Sprite/body shaders | `web/webgpu/sprite-gpu.mjs` (WGSL) | Same |
| Body geometry drawer | `render_frame` C (`core/network/gsta_render_frame.c`, also `render_frame.wasm`) | Compile the **C native** (`cc` crate / FFI) — it's portable C, already works as wasm |
| Wire decode | `web/webgpu-test.html` worker + `web/webgpu/frame-decoder.mjs` + `fzstd.mjs` | Port to Rust: `zstd` crate (streaming) + the SoA/STM2/GSTA parse |
| Input | `desktop/src-tauri/src/input.rs` (proven) | Already lifted → `src/input.rs` |
| Wire format facts | server `maplecast_mirror.cpp` (ZCS2/GSTA/STM2) | Reference for the Rust parser |

## What is NEW (native glue only)
- winit + wgpu window/surface/present (done: M0).
- `tokio-tungstenite` + rustls WS client → `wss://play.nobd.net/ws` (+ `/replica-live`).
- Rust ZCS2 streaming-zstd decoder + frame reassembly (the browser's JS worker, in Rust).
- Feed decoded TA + `render_frame` body quads into the wgpu pipeline.

## Milestones
- **M0 — foundation (DONE):** wgpu window clears the screen; native UDP:7100 input wired in. `cargo run`.
- **M1 — wire in:** connect `wss://play.nobd.net/ws` (rustls), streaming-zstd decode, reassemble one frame, log its structure. No pixels yet — prove the bytes arrive + decode.
- **M2 — stage/effects/HUD:** port the pvr2 WGSL + TA parse; draw the on-wire TA (stage, effects, HUD render already on the thin wire). First real pixels.
- **M3 — bodies:** compile `render_frame` native; parse GSTA/STM2 body state; emit sprite-TA quads; draw the fighters. Full game.
- **M4 — latency polish:** present-mode (Immediate/Mailbox), decode off the render thread, measure press→pixel, tune. Direct-to-nobd, no tunnel.

## Open question carried over
- **Endpoint:** `wss://play.nobd.net/ws` carries both wires (browser ZCS2 + legacy ZCST). Confirm which the client subscribes to and whether `/replica-live` (body state) is publicly exposed. Input `:7100/udp` is already public + direct.
