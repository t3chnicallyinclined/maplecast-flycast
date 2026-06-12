#!/bin/bash
# build_wasm.sh — emcc-compile the TRANSPILED MVC2 render to WASM.
#
# Output: web/render-replica/render_replica.{wasm,mjs}
#   render_object(uint8_t* ram16mb, uint32_t node_guest_addr,
#                 uint8_t* out_ta, uint32_t out_cap) -> uint32_t ta_len
#
# Sources (all reproducible; the gen_*.c are auto-generated, see run.cmd / codegen.py):
#   gen_walker.c   loc_8c0344d4  (the proven 9/9 @0.00px walker)
#   gen_leaf.c     loc_8C11E460  (the ftrc-magic floor leaf)
#   gen_submit.c   loc_8C124AB0  (the corner-transform, linked for coverage)
#   wasm_entry.c   the render_object() entry + TA writer + stubs/capture hook
#
# Prereq: emscripten (emsdk). The project's packages/renderer/build.sh assumes the
# same. Activate first, e.g.:
#   source /c/Users/trist/emsdk/emsdk_env.sh   (or your emsdk path)
#
# Usage:  ./build_wasm.sh
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$SCRIPT_DIR/../../web/render-replica"

# Regenerate image_dump.h (resident node/descriptor image + EXP_* control words)
# from the local RAM dump if it's missing. ROM-derived; gitignored.
if [ ! -f "$SCRIPT_DIR/image_dump.h" ]; then
    echo "[build_wasm] image_dump.h missing — regenerating via build_image_dump.py"
    ( cd "$SCRIPT_DIR" && python build_image_dump.py >/dev/null )
fi

if ! command -v emcc &>/dev/null; then
    echo "ERROR: emcc not found. Activate emsdk first:"
    echo "  source /c/Users/trist/emsdk/emsdk_env.sh"
    exit 1
fi
echo "[build_wasm] emcc: $(emcc --version | head -1)"

mkdir -p "$OUT_DIR"
cd "$SCRIPT_DIR"

emcc -O2 -fno-strict-aliasing \
    gen_walker.c gen_leaf.c gen_submit.c wasm_entry.c \
    -o "$OUT_DIR/render_replica.mjs" \
    -s MODULARIZE=1 \
    -s EXPORT_ES6=1 \
    -s EXPORT_NAME=createRenderReplica \
    -s ENVIRONMENT=web \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s INITIAL_MEMORY=67108864 \
    -s EXPORTED_FUNCTIONS='["_render_object","_render_object_quad_count","_render_object_capture_count","_render_set_opaque","_malloc","_free"]' \
    -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPU8","HEAPU32"]'

echo ""
echo "[build_wasm] BUILD COMPLETE:"
ls -lh "$OUT_DIR"/render_replica.wasm "$OUT_DIR"/render_replica.mjs
