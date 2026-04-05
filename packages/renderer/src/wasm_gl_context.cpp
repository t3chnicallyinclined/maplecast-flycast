// ============================================================================
// WASM_GL_CONTEXT.CPP — Standalone WebGL2 context for emscripten
//
// Creates a WebGL2 context directly on a <canvas> element.
// No RetroArch, no libretro, no EmulatorJS. Pure WebGL2.
//
// CRITICAL: Initializes the libretro GLSM (GL State Machine) function
// pointers via rglgen_resolve_symbols. Without this, all rgl* wrapper
// calls crash with "function signature mismatch".
// ============================================================================

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>
#include <cstdio>

// GL headers + GLSM
#include "wsi/context.h"
#include "wsi/gl_context.h"
#include <glsym/rglgen.h>
#include <glsm/glsm.h>
#include <libretro.h>

// Forward declarations
extern void initSettings();
extern LibretroGraphicsContext theGLContext;

static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE glContext = 0;
static int canvasWidth = 640;
static int canvasHeight = 480;

// ============================================================================
// GL proc address wrapper for emscripten → rglgen
// rglgen expects: rglgen_func_t (*)(const char*)  i.e. void(*)(void) (*)(const char*)
// emscripten gives: void* (*)(const char*)
// ============================================================================

static rglgen_func_t wasm_get_proc_address(const char* name) {
    return (rglgen_func_t)emscripten_webgl_get_proc_address(name);
}

// ============================================================================
// RetroArch hw_render callbacks — GLSM calls these, they must not be null
// ============================================================================

// hw_render is defined in glsm.c as a global
extern struct retro_hw_render_callback hw_render;

// get_current_framebuffer — returns FBO 0 (the canvas) since we don't
// use RetroArch's FBO management
static uintptr_t wasm_get_current_framebuffer(void) {
    return 0; // FBO 0 = the canvas backbuffer
}

// get_proc_address — same wrapper as above but with retro callback signature
static retro_proc_address_t wasm_retro_get_proc_address(const char* sym) {
    return (retro_proc_address_t)emscripten_webgl_get_proc_address(sym);
}

// ============================================================================
// WebGL2 context creation — called from renderer_init()
// ============================================================================

bool wasm_gl_init(int width, int height) {
    canvasWidth = width;
    canvasHeight = height;

    // Initialize settings with DC defaults
    initSettings();

    // Set canvas size
    emscripten_set_canvas_element_size("#game-canvas", width, height);

    // Create WebGL2 context with optimal attributes
    EmscriptenWebGLContextAttributes attrs;
    emscripten_webgl_init_context_attributes(&attrs);
    attrs.majorVersion = 2;              // WebGL2 = OpenGL ES 3.0
    attrs.minorVersion = 0;
    attrs.alpha = 1;                     // Alpha channel needed — GL_DST_ALPHA blend reads it
    attrs.depth = 1;                     // Need depth buffer for PVR rendering
    attrs.stencil = 1;                   // Need stencil for modifier volumes
    attrs.antialias = 0;                 // No AA — pixel-perfect rendering
    attrs.premultipliedAlpha = 1;        // Chrome compositor needs this to avoid black boxes
    attrs.preserveDrawingBuffer = 0;     // Don't preserve — faster swap
    attrs.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
    attrs.failIfMajorPerformanceCaveat = 0;
    attrs.enableExtensionsByDefault = 1;
    attrs.renderViaOffscreenBackBuffer = 0;

    glContext = emscripten_webgl_create_context("#game-canvas", &attrs);
    if (glContext <= 0) {
        printf("[renderer] Failed to create WebGL2 context: %lu\n", (unsigned long)glContext);
        return false;
    }

    EMSCRIPTEN_RESULT res = emscripten_webgl_make_context_current(glContext);
    if (res != EMSCRIPTEN_RESULT_SUCCESS) {
        printf("[renderer] Failed to make WebGL2 context current: %d\n", res);
        return false;
    }

    printf("[renderer] WebGL2 context created: %dx%d\n", width, height);

    // ================================================================
    // CRITICAL: Set up RetroArch hw_render callbacks BEFORE GLSM init
    // GLSM calls hw_render.get_current_framebuffer() and get_proc_address()
    // during rendering. If these are null = instant crash with
    // "function signature mismatch" (null function pointer call in WASM).
    // ================================================================
    hw_render.get_current_framebuffer = wasm_get_current_framebuffer;
    hw_render.get_proc_address = wasm_retro_get_proc_address;
    printf("[renderer] hw_render callbacks set\n");

    // Resolve all rgl* function pointers (rglGetString, rglViewport, etc.)
    rglgen_resolve_symbols(wasm_get_proc_address);
    printf("[renderer] GLSM function pointers resolved\n");

    // Now we can safely use rgl* wrappers (or raw GL — they both work now)
    printf("[renderer] GL_VERSION: %s\n", glGetString(GL_VERSION));
    printf("[renderer] GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    // Initialize the libretro GL context wrapper
    // This calls postInit() → findGLVersion() which queries GL capabilities
    extern void setGraphicsContext(GraphicsContext* ctx);
    setGraphicsContext(&theGLContext);
    theGLContext.init();
    printf("[renderer] LibretroGraphicsContext initialized\n");

    // Set viewport
    glViewport(0, 0, width, height);

    return true;
}

void wasm_gl_resize(int width, int height) {
    canvasWidth = width;
    canvasHeight = height;
    emscripten_set_canvas_element_size("#game-canvas", width, height);
    if (glContext > 0) {
        emscripten_webgl_make_context_current(glContext);
        glViewport(0, 0, width, height);
    }
}

void wasm_gl_destroy() {
    if (glContext > 0) {
        theGLContext.term();
        emscripten_webgl_destroy_context(glContext);
        glContext = 0;
    }
}

int wasm_gl_get_width() { return canvasWidth; }
int wasm_gl_get_height() { return canvasHeight; }

#endif // __EMSCRIPTEN__
