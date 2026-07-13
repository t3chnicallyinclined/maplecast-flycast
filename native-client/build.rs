// Compile the transpiled-SH4 body drawer natively. gsta_render_frame.c is a single
// translation unit that #includes the render-replica-poc .c files.
fn main() {
    let c = "../core/network/gsta_render_frame.c";
    println!("cargo:rerun-if-changed={c}");
    cc::Build::new()
        .file(c)
        .include("../tools/render-replica-poc")
        .warnings(false)
        .opt_level(2)
        .compile("gsta_render_frame");
    #[cfg(unix)]
    println!("cargo:rustc-link-lib=m");
}
