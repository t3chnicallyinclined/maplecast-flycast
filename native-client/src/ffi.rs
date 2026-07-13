//! FFI to the transpiled-SH4 body drawer (core/network/gsta_render_frame.c).
//!
//! render_frame() walks the MVC2 slot table over a resident 16MB SH4 RAM image
//! and emits body sprite quads. Zero the ctx, set ctx.ram, call render_frame(&ctx),
//! then read render_frame_scene()[0..render_frame_nscene()].

/// The SH4 execution context. Only `.ram` is set by the caller; the rest is scratch.
/// Layout == tools/render-replica-poc/sh4ctx.h (232 bytes on LP64/Win64).
#[repr(C)]
#[derive(Clone)]
pub struct GstaSh4Ctx {
    pub r: [u32; 16],
    pub fr: [f32; 16],
    pub xf: [f32; 16],
    pub fpscr: u32,
    pub fpul: u32,
    pub pr: u32,
    pub macl: u32,
    pub mach: u32,
    pub sr_t: u32,
    pub gbr: u32,
    pub _pool: u32,
    pub ram: *mut u8,
}

impl GstaSh4Ctx {
    pub fn zeroed() -> Self {
        // SAFETY: all fields are POD; a zeroed ctx with a null ram is the documented
        // reset state (caller sets .ram before render_frame).
        unsafe { std::mem::zeroed() }
    }
}

/// The emitted quad. Layout == the REAL struct at render-replica-poc/render_frame.c:264
/// (88 bytes) — NOT the stale 80-byte typedef in gsta_render_frame.h.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SceneQuad {
    pub pcw: u32,
    pub isp: u32,
    pub tsp: u32,
    pub tcw: u32,
    pub recidx: u32,
    pub ax: f32,
    pub ay: f32,
    pub bx: f32,
    pub by: f32,
    pub cx: f32,
    pub cy: f32,
    pub dx: f32,
    pub dy: f32,
    pub u1: f32,
    pub v1: f32,
    pub sel: u32,
    pub gfx1: u32,
    pub mirror: u32,
    pub mirror_v: u32,
    pub rawflags: u32,
    pub facing: u32,
    pub z: f32,
}

// Layout guards — catch any mismatch at compile time (the header/struct disagree).
const _: () = assert!(std::mem::size_of::<GstaSh4Ctx>() == 232);
const _: () = assert!(std::mem::size_of::<SceneQuad>() == 88);

extern "C" {
    pub fn render_frame(c: *mut GstaSh4Ctx);
    pub fn render_frame_nscene() -> i32;
    pub fn render_frame_scene() -> *const SceneQuad;
    pub fn render_frame_body_count() -> u32;
    pub fn render_frame_set_body_tcws(tcws: *const u32, n: i32);
    pub fn gsta_quad_srcdesc(out: *mut u8, cap: u32) -> u32;
    pub fn gsta_quad_is_effect(out: *mut u8, cap: u32) -> u32;
}

/// Force-link probe: safe before any frame (returns the current g_nscene, 0 at rest).
pub fn link_probe() -> i32 {
    unsafe { render_frame_nscene() }
}

/// Per-quad [m, cx, ry, flags] source descriptors for the current frame's `n` quads
/// (the emit-time tile-carve key). Returns exactly `n` entries (missing padded 0).
pub fn quad_srcdesc(n: usize) -> Vec<[u8; 4]> {
    let mut buf = vec![0u8; n * 4];
    let _ = unsafe { gsta_quad_srcdesc(buf.as_mut_ptr(), n as u32) };
    (0..n)
        .map(|i| [buf[i * 4], buf[i * 4 + 1], buf[i * 4 + 2], buf[i * 4 + 3]])
        .collect()
}

/// Per-quad "is a BIT15 effect quad" flags for the current frame's `n` quads.
pub fn quad_is_effect(n: usize) -> Vec<bool> {
    let mut buf = vec![0u8; n];
    let _ = unsafe { gsta_quad_is_effect(buf.as_mut_ptr(), n as u32) };
    buf.iter().map(|&b| b != 0).collect()
}
