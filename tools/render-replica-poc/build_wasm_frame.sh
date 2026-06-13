#!/bin/bash
# build_wasm_frame.sh — PHASE 2: emcc-compile the WHOLE-FRAME transpiled render to WASM.
#
# Output: web/render-replica/render_frame.{wasm,mjs}
#   render_frame_ta(uint8_t* ram16mb, uint8_t* out_ta, uint32_t out_cap) -> uint32_t ta_len
#   render_frame_body_count() -> uint32_t   (bodies the slot-walk rendered)
#   render_frame_quad_count() -> uint32_t   (total body tiles)
#
# Sources (all reproducible; gen_*.c auto-generated):
#   gen_walker_root.c  loc_8c0308c2  (the slot-table walk = render ROOT)
#   render_frame.c     render_frame + render_object_full + cursor bookkeeping
#   gen_render_object.c loc_8c03093c (per-object setup: anchor+scale deposit)
#   gen_transform_obj.c loc_8c122560 (world->screen ftrv, hand-verified)
#   gen_submit_params.c loc_8C1244B0 finalize (PCW/ISP/TSP/TCW from resident rectab)
#   gen_walker.c        loc_8c0344d4 (the proven 9/9 @0.00px body walker)
#   gen_leaf.c          loc_8C11E460 (floor leaf)
#   wasm_entry_frame.c  the render_frame_ta() entry + PVR2 sprite-TA writer
#
# Prereq: emscripten (emsdk). Activate first, e.g.:
#   source /c/Users/trist/emsdk/emsdk_env.sh
#
# Usage:  ./build_wasm_frame.sh
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$SCRIPT_DIR/../../web/render-replica"

if ! command -v emcc &>/dev/null; then
    echo "ERROR: emcc not found. Activate emsdk first:"
    echo "  source /c/Users/trist/emsdk/emsdk_env.sh"
    exit 1
fi
echo "[build_wasm_frame] emcc: $(emcc --version | head -1)"

# Regenerate the transpiled C (deterministic, from the disasm). ALWAYS regenerate the
# auto-gen units so a generator/disasm change (e.g. the loc_8c030af8 satellite dispatch)
# is picked up — these are pure functions of the marvelous2 .asm, cheap and reproducible.
cd "$SCRIPT_DIR"
python gen_walker_root.py
python gen_render_object.py
python gen_render_satellite.py
[ -f gen_walker.c ]        || python gen_walker.py
[ -f gen_leaf.c ]          || python gen_leaf.py

mkdir -p "$OUT_DIR"

SRCS="gen_walker_root.c render_frame.c gen_render_object.c gen_render_satellite.c \
    gen_transform_obj.c gen_submit_params.c gen_walker.c gen_leaf.c wasm_entry_frame.c"
EXPORTS='["_render_frame_ta","_render_frame_body_count","_render_frame_sat_count","_render_frame_quad_count","_render_frame_quad_sels","_render_frame_quad_gfx1s","_render_frame_quad_colrow","_render_frame_quad_mirror","_malloc","_free"]'

# WEB target (the live client uses web/render-replica/render_frame.{mjs,wasm}).
emcc -O2 -fno-strict-aliasing $SRCS \
    -o "$OUT_DIR/render_frame.mjs" \
    -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORT_NAME=createRenderFrame \
    -s ENVIRONMENT=web -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=67108864 \
    -s EXPORTED_FUNCTIONS="$EXPORTS" \
    -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPU8","HEAPU32"]'

# NODE target (the offline probes/repro import render_frame_node.{mjs,wasm}).
emcc -O2 -fno-strict-aliasing $SRCS \
    -o "$SCRIPT_DIR/render_frame_node.mjs" \
    -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORT_NAME=createRenderFrame \
    -s ENVIRONMENT=node -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=67108864 \
    -s EXPORTED_FUNCTIONS="$EXPORTS" \
    -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPU8","HEAPU32"]'

echo ""
echo "[build_wasm_frame] BUILD COMPLETE:"
ls -lh "$OUT_DIR"/render_frame.wasm "$OUT_DIR"/render_frame.mjs "$SCRIPT_DIR"/render_frame_node.wasm
