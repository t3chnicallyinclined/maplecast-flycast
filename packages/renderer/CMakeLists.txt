cmake_minimum_required(VERSION 3.16)
project(flycast_mirror_renderer C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Root of the flycast source tree
set(FLYCAST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# ==============================================================================
# Source files — the minimum viable renderer (~35 files)
# ==============================================================================

set(RENDERER_SOURCES
    # ---- Bridge (entry point) ----
    src/wasm_bridge.cpp
    src/wasm_gl_context.cpp
    src/stubs.cpp

    # ---- PVR (Tile Accelerator + registers) ----
    ${FLYCAST_ROOT}/core/hw/pvr/pvr_regs.cpp
    ${FLYCAST_ROOT}/core/hw/pvr/pvr_mem.cpp
    ${FLYCAST_ROOT}/core/hw/pvr/ta_vtx.cpp
    ${FLYCAST_ROOT}/core/hw/pvr/ta_ctx.cpp
    ${FLYCAST_ROOT}/core/hw/pvr/ta_util.cpp

    # ---- GLES Renderer ----
    ${FLYCAST_ROOT}/core/rend/gles/gles.cpp
    ${FLYCAST_ROOT}/core/rend/gles/gldraw.cpp
    ${FLYCAST_ROOT}/core/rend/gles/gltex.cpp
    ${FLYCAST_ROOT}/core/rend/gles/naomi2.cpp
    ${FLYCAST_ROOT}/core/rend/gles/quad.cpp
    ${FLYCAST_ROOT}/core/rend/gles/postprocess.cpp

    # ---- Texture / Rendering Support ----
    ${FLYCAST_ROOT}/core/rend/texconv.cpp
    ${FLYCAST_ROOT}/core/rend/TexCache.cpp
    ${FLYCAST_ROOT}/core/rend/sorter.cpp
    ${FLYCAST_ROOT}/core/rend/CustomTexture.cpp
    ${FLYCAST_ROOT}/core/rend/osd.cpp

    # ---- Core Infrastructure ----
    ${FLYCAST_ROOT}/core/stdclass.cpp
    ${FLYCAST_ROOT}/shell/libretro/option.cpp
    ${FLYCAST_ROOT}/core/cfg/cfg.cpp

    # ---- WSI ----
    ${FLYCAST_ROOT}/core/wsi/gl_context.cpp

    # ---- Dependencies ----
    ${FLYCAST_ROOT}/core/deps/xbrz/xbrz.cpp

    # ---- Libretro GL state machine (PATCHED for WebGL2 — skip GL_FOG/GL_ALPHA_TEST) ----
    ${CMAKE_CURRENT_SOURCE_DIR}/src/glsm_patched.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gl_direct.c
    ${FLYCAST_ROOT}/core/deps/libretro-common/glsym/rglgen.c
    ${FLYCAST_ROOT}/core/deps/libretro-common/glsym/glsym_es3.c

    # ---- WSI libretro context (provides theGLContext) ----
    ${FLYCAST_ROOT}/core/wsi/libretro.cpp
)

# ==============================================================================
# Include paths
# ==============================================================================

set(RENDERER_INCLUDES
    ${FLYCAST_ROOT}/core
    ${FLYCAST_ROOT}/core/deps
    ${FLYCAST_ROOT}/core/deps/glm
    ${FLYCAST_ROOT}/core/deps/xxHash
    ${FLYCAST_ROOT}/core/deps/stb
    ${FLYCAST_ROOT}/core/deps/nowide/include
    ${FLYCAST_ROOT}/core/deps/libretro-common/include
    ${FLYCAST_ROOT}/core/deps/json
    ${FLYCAST_ROOT}/core/rend/gles
    ${FLYCAST_ROOT}/core/hw/pvr
    ${FLYCAST_ROOT}/shell/libretro
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ==============================================================================
# Compile definitions
# ==============================================================================

set(RENDERER_DEFINES
    LIBRETRO
    GLES
    GLES3
    HAVE_OPENGLES3
    HAVE_OPENGLES
    TARGET_NO_THREADS
    STANDALONE_MIRROR_RENDERER
    XXH_INLINE_ALL
)

# ==============================================================================
# Build target
# ==============================================================================

if(EMSCRIPTEN)
    add_executable(renderer ${RENDERER_SOURCES})

    target_include_directories(renderer PRIVATE ${RENDERER_INCLUDES})
    target_compile_definitions(renderer PRIVATE ${RENDERER_DEFINES})

    # Emscripten link flags — OVERKILL OPTIMIZATION
    set(EM_LINK_FLAGS
        "-s MODULARIZE=1"
        "-s EXPORT_ES6=1"
        "-s EXPORTED_FUNCTIONS=['_renderer_init','_renderer_frame','_renderer_sync','_renderer_resize','_renderer_destroy','_renderer_info','_malloc','_free']"
        "-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','HEAPU32']"
        "-s ALLOW_MEMORY_GROWTH=1"
        "-s INITIAL_MEMORY=33554432"
        "-s MAXIMUM_MEMORY=134217728"
        "-s USE_WEBGL2=1"
        "-s FULL_ES3=1"
        "-s MIN_WEBGL_VERSION=2"
        "-s MAX_WEBGL_VERSION=2"
        "-s NO_EXIT_RUNTIME=1"
        "-s ENVIRONMENT='web'"
        "-s FILESYSTEM=0"
        "-s FORCE_FILESYSTEM=0"
        "-s DISABLE_EXCEPTION_CATCHING=0"
        "-fexceptions"
        "-s ASSERTIONS=0"
        "-O3"
        "-fno-lto"
        "--closure 0"
    )
    string(REPLACE ";" " " EM_LINK_FLAGS_STR "${EM_LINK_FLAGS}")

    set_target_properties(renderer PROPERTIES
        SUFFIX ".mjs"
        LINK_FLAGS "${EM_LINK_FLAGS_STR}"
    )

    # Compile flags
    target_compile_options(renderer PRIVATE
        -O3
        -fno-lto
        -fexceptions
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-sign-compare
        -Wno-missing-braces
    )
else()
    message(FATAL_ERROR "This project must be built with Emscripten. Use: emcmake cmake ..")
endif()
