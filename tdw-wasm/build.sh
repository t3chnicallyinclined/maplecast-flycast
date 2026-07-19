#!/usr/bin/env bash
# Build the browser TDW decoder wasm from native-client-tdw/src/tdw.rs (read-only)
# and copy it to web/webgpu/tdw.wasm. Requires the emsdk clang toolchain — zstd's
# C compiles to wasm through it (self-contained rust_zstd_wasm_shim_*, no libc).
#
#   EMSDK=~/emsdk ./build.sh
#
# Run this after ANY change to native-client-tdw/src/tdw.rs so the browser wasm
# doesn't silently go stale vs the native/server wire.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
EMSDK="${EMSDK:-$HOME/emsdk}"

CC="$EMSDK/upstream/bin/clang.exe"; [ -f "$CC" ] || CC="$EMSDK/upstream/bin/clang"
AR="$EMSDK/upstream/bin/llvm-ar.exe"; [ -f "$AR" ] || AR="$EMSDK/upstream/bin/llvm-ar"
[ -f "$CC" ] || { echo "emsdk clang not found (looked under $EMSDK/upstream/bin). Set EMSDK." >&2; exit 1; }

export CC_wasm32_unknown_unknown="$CC"
export AR_wasm32_unknown_unknown="$AR"

cd "$HERE"
cargo build --release --target wasm32-unknown-unknown
cp target/wasm32-unknown-unknown/release/tdw_wasm.wasm "$HERE/../web/webgpu/tdw.wasm"
echo "wrote $(cd "$HERE/.." && pwd)/web/webgpu/tdw.wasm ($(wc -c < "$HERE/../web/webgpu/tdw.wasm") bytes)"
