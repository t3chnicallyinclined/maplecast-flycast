// gl_direct.c — Direct GL calls bypassing GLSM macros
// The GLSM redefines glEnable/glDisable to rglEnable/rglDisable.
// These functions call the REAL GL functions for use in glcache.
#include <GLES3/gl3.h>

void gl_direct_enable(unsigned int cap) {
    glEnable(cap);
}

void gl_direct_disable(unsigned int cap) {
    glDisable(cap);
}

void gl_direct_blend_func(unsigned int src, unsigned int dst) {
    glBlendFunc(src, dst);
}
