#!/bin/bash
# ============================================================================
# BUILD.SH — Build the standalone WASM mirror renderer
#
# Prerequisites:
#   - Emscripten SDK (emsdk) installed and activated
#   - source /path/to/emsdk/emsdk_env.sh
#
# Usage:
#   ./build.sh          # Release build (O3 + LTO)
#   ./build.sh debug    # Debug build (O0 + assertions)
# ============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
DIST_DIR="$SCRIPT_DIR/dist"

# Check emscripten
if ! command -v emcc &> /dev/null; then
    echo "ERROR: emcc not found. Run: source /path/to/emsdk/emsdk_env.sh"
    exit 1
fi

echo "=== KING OF MARVEL — WASM Renderer Build ==="
echo "emcc: $(emcc --version | head -1)"

# Clean build directory
mkdir -p "$BUILD_DIR" "$DIST_DIR"

# Configure with emcmake
cd "$BUILD_DIR"

if [ "$1" = "debug" ]; then
    echo "=== DEBUG BUILD ==="
    emcmake cmake "$SCRIPT_DIR" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS="-O0 -g" \
        -DCMAKE_CXX_FLAGS="-O0 -g"
else
    echo "=== RELEASE BUILD (OVERKILL MODE) ==="
    emcmake cmake "$SCRIPT_DIR" \
        -DCMAKE_BUILD_TYPE=Release
fi

# Build
emmake make -j$(nproc) 2>&1

# Copy artifacts to dist
cp "$BUILD_DIR/renderer.mjs" "$DIST_DIR/" 2>/dev/null || true
cp "$BUILD_DIR/renderer.wasm" "$DIST_DIR/" 2>/dev/null || true

echo ""
echo "=== BUILD COMPLETE ==="
ls -lh "$DIST_DIR/"
echo ""
echo "WASM size: $(du -h "$DIST_DIR/renderer.wasm" 2>/dev/null | cut -f1 || echo 'N/A')"
echo ""
echo "Deploy: copy dist/renderer.mjs + dist/renderer.wasm to your web app"
