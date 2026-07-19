# tdw-wasm — the browser TDW decoder

Compiles the **native-client `tdw.rs`** to wasm so the browser can reconstruct the
game from the sub-1 Mbps TDW dict-wire (state, not video). Same decoder as the
native client — byte-exact by construction.

## What it is / isn't

- **Is:** a `cdylib` that `#[path]`-includes `../native-client-tdw/src/tdw.rs`
  **read-only** and exposes a tiny raw C-ABI (`tdw_new` / `tdw_inbuf` /
  `tdw_snapshot` / `tdw_feed` / `tdw_dict_len` / `tdw_synced` / `tdw_ack_frame`).
  The JS wrapper is [`web/webgpu/tdw-decoder.mjs`](../web/webgpu/tdw-decoder.mjs);
  it repackages a decoded frame as a legacy keyframe so the **unchanged**
  FrameDecoder / TAParser / PVR2Renderer path draws it.
- **Isn't:** an edit of `tdw.rs`. The QUIC / TDW2 agent owns that file; we only
  read it. So there's no merge collision — but the wasm can go **stale** when the
  wire changes. Rebuild after any `tdw.rs` change (see below).

## Build

```bash
EMSDK=~/emsdk ./build.sh        # writes ../web/webgpu/tdw.wasm
```

Needs the emsdk clang toolchain — `zstd`'s C compiles to wasm through it
(`zstd-sys` ships its own `rust_zstd_wasm_shim_*`, so no libc is required and there
are no missing imports). Output is ~350 KB (LTO; ~100–130 KB gzipped).

## Proven

`tdw.rs` compiled native (real libzstd) vs this wasm build (emsdk zstd) decode a
live prod capture **byte-identical, 481/481 frames** (per-frame TA fnv64). The
full browser chain (decode → synth legacy frame → real FrameDecoder.applyFrame →
real TAParser.parse) is headless-validated byte-exact, and the WebGPU render is
user-confirmed live at `/tdw-test.html`.

## Transports

`feed()` dispatches **TDW1** (streaming zstd, reliable — WS today) and **TDW2**
(ACK-reference keyframe/delta, loss-tolerant — for datagram / WebTransport). The
loss-tolerant TDW2 wire is what makes a future browser-over-WebTransport path safe
(streaming-zstd desyncs on datagram loss; TDW2 recovers at the next keyframe).

## TODO (when the native-client-tdw merge settles)

Promote the read-only `#[path]` to a real shared **`tdw-core`** crate that both
`native-client-tdw` and this crate depend on — one source of truth, so the wasm
can't diverge from the native decoder.
