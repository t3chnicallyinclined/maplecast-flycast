// pvr2 core shader — ported verbatim from web/webgpu/shaders.mjs (the 20 debug
// "effects" are omitted; they're inert on a normal frame).

struct Uniforms { ndcMat: mat4x4<f32> };
@group(0) @binding(0) var<uniform> uniforms: Uniforms;

struct VIn {
    @location(0) pos: vec3<f32>,
    @location(1) col: vec4<f32>,
    @location(2) spc: vec4<f32>,
    @location(3) uv: vec2<f32>,
};
struct VOut {
    @builtin(position) position: vec4<f32>,
    @location(0) vb: vec4<f32>,
    @location(1) vo: vec4<f32>,
    @location(2) vuv: vec3<f32>,
};

@vertex
fn vs_main(in: VIn) -> VOut {
    var o: VOut;
    let vp = uniforms.ndcMat * vec4<f32>(in.pos, 1.0);
    o.vb = in.col;
    o.vo = in.spc;
    // pos.z is 1/w; carry perspective-correct uv + 1/w, force clip-z=0.
    o.vuv = vec3<f32>(in.uv * in.pos.z, in.pos.z);
    o.position = vec4<f32>(vp.xy, 0.0, 1.0);
    return o;
}

struct FU {
    atv: f32,
    si: u32,
    ht: u32,
    ua: u32,
    ita: u32,
    ho: u32,
    at: u32,
    packed: u32,
};
@group(0) @binding(1) var<uniform> fu: FU;
@group(1) @binding(0) var tex: texture_2d<f32>;
@group(1) @binding(1) var ts: sampler;

struct FIn {
    @location(0) vb: vec4<f32>,
    @location(1) vo: vec4<f32>,
    @location(2) vuv: vec3<f32>,
};
struct FOut {
    @builtin(frag_depth) depth: f32,
    @location(0) color: vec4<f32>,
};

@fragment
fn fs_main(in: FIn) -> FOut {
    var o: FOut;
    let iw = in.vuv.z;
    let sw = select(iw, 0.00001, abs(iw) < 0.00001);
    let uv = in.vuv.xy / sw;

    var c = in.vb;
    var ofs = in.vo;
    if (fu.ua == 0u) { c.a = 1.0; }
    if (fu.ht == 1u) {
        var tc = textureSampleLevel(tex, ts, uv, 0.0);
        if (fu.ita == 1u) { tc.a = 1.0; }
        if (fu.si == 0u) { c = tc; }
        else if (fu.si == 1u) { c = vec4<f32>(c.rgb * tc.rgb, tc.a); }
        else if (fu.si == 2u) { c = vec4<f32>(mix(c.rgb, tc.rgb, tc.a), c.a); }
        else { c = c * tc; }
        if (fu.ho == 1u) { c = vec4<f32>(c.rgb + ofs.rgb, c.a); }
    }
    c = clamp(c, vec4<f32>(0.0), vec4<f32>(1.0));
    if (fu.at == 1u) {
        let qa = floor(c.a * 255.0 + 0.5) / 255.0;
        if (fu.atv > qa) { discard; }
        c.a = 1.0;
    }
    if (c.a < 0.004) { discard; }

    let logDepth = log2(1.0 + max(100000.0 * iw, -0.999999)) / 34.0;
    o.depth = logDepth;
    o.color = clamp(c, vec4<f32>(0.0), vec4<f32>(1.0));
    return o;
}
