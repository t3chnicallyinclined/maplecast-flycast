// sprite-staf-gl.mjs — minimal WebGL2 textured-triangle renderer for the STAF
// (stripped-TA) channel. This replicates flycast's OWN draw model
// (core/rend/gles/gldraw.cpp + gles.cpp fragment shader): it rasterizes the exact
// per-vertex (x,y,u,v,col) triangles the game's TA produced, with the PVR TSP
// blend/shade state mapped 1:1 to GL — NOT a Canvas2D dest-rect approximation.
//
// Input comes from SpriteClient.onSTAF():
//   client._stafV       Float32Array, 8 floats/vertex [x,y,u,v, r,g,b,a] (3 verts/tri)
//   client.stafQuads    per-tri descriptors {key, blend, shadInstr, ignoreTexA, textured, punch, voff}
//   client._stafTex     Map<tex_id -> {w,h,rgba}>   (decoded RGBA, server-side decode)
//
// We upload each texture to a GL texture once (keyed by tex_id), then draw the
// triangle list batched by GL state (blend/texture/shade), in server z-order
// (op -> pt -> tr — the server emits in that order).

// PVR blend instruction -> GL blend factor, EXACTLY as flycast's SrcBlendGL /
// DstBlendGL (core/rend/gles/gldraw.cpp:53-75). Index = the 3-bit instr.
const SRC_BLEND_GL = [ 'ZERO','ONE','DST_COLOR','ONE_MINUS_DST_COLOR','SRC_ALPHA','ONE_MINUS_SRC_ALPHA','DST_ALPHA','ONE_MINUS_DST_ALPHA' ];
const DST_BLEND_GL = [ 'ZERO','ONE','SRC_COLOR','ONE_MINUS_SRC_COLOR','SRC_ALPHA','ONE_MINUS_SRC_ALPHA','DST_ALPHA','ONE_MINUS_DST_ALPHA' ];

const VS = `#version 300 es
precision highp float;
layout(location=0) in vec2 a_pos;   // 640x480 screen space
layout(location=1) in vec2 a_uv;
layout(location=2) in vec4 a_col;   // 0..1 RGBA
uniform vec2 u_screen;              // (640,480)
out vec2 v_uv;
out vec4 v_col;
void main() {
  vec2 ndc = vec2(a_pos.x / u_screen.x * 2.0 - 1.0,
                  1.0 - a_pos.y / u_screen.y * 2.0);   // flip Y (screen top-left origin)
  gl_Position = vec4(ndc, 0.0, 1.0);
  v_uv = a_uv;
  v_col = a_col;
}`;

// Fragment: replicate flycast's texture<->vertex-color combine (gles.cpp ShadInstr
// block) + IgnoreTexA + punch-through alpha test.
//   u_textured: 0 untextured (use vertex color), 1 textured
//   u_shadInstr: 0 replace, 1 modulate-rgb/replace-a, 2 mix-by-texA, 3 modulate
//   u_ignoreTexA: force texcol.a = 1
//   u_alphaTest: punch-through (>0 -> discard where color.a < value)
const FS = `#version 300 es
precision highp float;
in vec2 v_uv;
in vec4 v_col;
uniform sampler2D u_tex;
uniform int u_textured;
uniform int u_shadInstr;
uniform int u_ignoreTexA;
uniform float u_alphaTest;
out vec4 o_col;
void main() {
  vec4 color = v_col;
  if (u_textured == 1) {
    vec4 texcol = texture(u_tex, v_uv);
    if (u_ignoreTexA == 1) texcol.a = 1.0;
    if (u_shadInstr == 0)      { color = texcol; }
    else if (u_shadInstr == 1) { color.rgb *= texcol.rgb; color.a = texcol.a; }
    else if (u_shadInstr == 2) { color.rgb = mix(color.rgb, texcol.rgb, texcol.a); }
    else                       { color *= texcol; }
  }
  if (u_alphaTest > 0.0 && color.a < u_alphaTest) discard;
  o_col = color;
}`;

export class StafGL {
  constructor() {
    this.ok = false;
    this.gl = null;
    this.canvas = null;
    this._texs = new Map();      // tex_id(string) -> { glTex, w, h }
    this._frameKeys = null;      // (unused; reserved for LRU)
  }

  init(canvas) {
    const gl = canvas.getContext('webgl2', { premultipliedAlpha: false, alpha: true, antialias: false });
    if (!gl) { console.error('[staf-gl] WebGL2 unavailable'); return false; }
    this.gl = gl; this.canvas = canvas;
    const prog = this._link(VS, FS);
    if (!prog) return false;
    this.prog = prog;
    this.u_screen   = gl.getUniformLocation(prog, 'u_screen');
    this.u_tex      = gl.getUniformLocation(prog, 'u_tex');
    this.u_textured = gl.getUniformLocation(prog, 'u_textured');
    this.u_shad     = gl.getUniformLocation(prog, 'u_shadInstr');
    this.u_ignoreA  = gl.getUniformLocation(prog, 'u_ignoreTexA');
    this.u_alpha    = gl.getUniformLocation(prog, 'u_alphaTest');
    this.vbo = gl.createBuffer();
    // 1x1 white fallback texture for untextured / not-yet-received quads.
    this.whiteTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.whiteTex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([255,255,255,255]));
    this.ok = true;
    console.log('[staf-gl] WebGL2 renderer ready');
    return true;
  }

  _link(vsSrc, fsSrc) {
    const gl = this.gl;
    const vs = this._shader(gl.VERTEX_SHADER, vsSrc);
    const fs = this._shader(gl.FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) return null;
    const p = gl.createProgram();
    gl.attachShader(p, vs); gl.attachShader(p, fs); gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) { console.error('[staf-gl] link', gl.getProgramInfoLog(p)); return null; }
    return p;
  }
  _shader(type, src) {
    const gl = this.gl, s = gl.createShader(type);
    gl.shaderSource(s, src); gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) { console.error('[staf-gl] shader', gl.getShaderInfoLog(s)); return null; }
    return s;
  }

  // Upload (or fetch) the GL texture for a tex_id from the client's RGBA cache.
  _texFor(client, key) {
    let t = this._texs.get(key);
    if (t) return t;
    const src = client._stafTex.get(key);
    if (!src) return null;                 // not received yet -> caller uses white
    const gl = this.gl, tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, src.w, src.h, 0, gl.RGBA, gl.UNSIGNED_BYTE, src.rgba);
    // Sprites don't tile in MVC2's quads -> clamp; bilinear like the renderer's default.
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    t = { glTex: tex, w: src.w, h: src.h };
    this._texs.set(key, t);
    return t;
  }

  // Render the client's current STAF frame. screenW/H default 640x480 (TA space).
  render(client, screenW = 640, screenH = 480) {
    if (!this.ok) return;
    const gl = this.gl;
    const tris = client.stafQuads, V = client._stafV, vcount = client._stafVCount | 0;
    gl.viewport(0, 0, this.canvas.width, this.canvas.height);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    if (!tris || !tris.length || !V || !vcount) return;

    gl.useProgram(this.prog);
    gl.uniform2f(this.u_screen, screenW, screenH);
    gl.uniform1i(this.u_tex, 0);
    gl.activeTexture(gl.TEXTURE0);

    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.bufferData(gl.ARRAY_BUFFER, V.subarray(0, vcount * 8), gl.DYNAMIC_DRAW);
    const STRIDE = 8 * 4;
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 2, gl.FLOAT, false, STRIDE, 0);
    gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 2, gl.FLOAT, false, STRIDE, 8);
    gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 4, gl.FLOAT, false, STRIDE, 16);

    gl.enable(gl.BLEND);
    gl.disable(gl.DEPTH_TEST);           // 2D: list order (op->pt->tr) IS the z-order
    gl.disable(gl.CULL_FACE);            // TA winding varies along strips; texture both sides

    // Batch consecutive triangles that share GL state into one drawArrays. The
    // server emits op -> pt -> tr in order, so this preserves draw order exactly.
    let bStart = 0;                       // first vertex of the current batch
    let curSig = null, curTex = null, cur = null;
    const flush = (endVert) => {
      if (!cur || endVert <= bStart) return;
      // blend
      gl.blendFunc(gl[SRC_BLEND_GL[(cur.blend >> 4) & 7]], gl[DST_BLEND_GL[cur.blend & 7]]);
      gl.uniform1i(this.u_textured, cur.textured ? 1 : 0);
      gl.uniform1i(this.u_shad, cur.shadInstr | 0);
      gl.uniform1i(this.u_ignoreA, cur.ignoreTexA ? 1 : 0);
      // Punch-through alpha test ~0.5 (flycast cp_AlphaTestValue), else off.
      gl.uniform1f(this.u_alpha, cur.punch ? (0.5) : 0.0);
      gl.bindTexture(gl.TEXTURE_2D, curTex || this.whiteTex);
      gl.drawArrays(gl.TRIANGLES, bStart, endVert - bStart);
    };

    for (const t of tris) {
      // Resolve texture (white fallback if untextured or not-yet-received).
      let glTex = null, textured = t.textured;
      if (t.textured && t.key) {
        const tx = this._texFor(client, t.key);
        if (tx) glTex = tx.glTex; else textured = 0;   // not received -> draw as untextured (vertex color)
      } else textured = 0;
      const sig = ((t.blend & 0xff) << 8) | ((t.shadInstr & 3) << 2) | ((t.ignoreTexA & 1) << 1) | (t.punch & 1);
      const sameState = (curSig === sig) && (curTex === glTex) && cur && (cur.textured ? 1 : 0) === (textured ? 1 : 0);
      if (!sameState) {
        flush(t.voff);
        bStart = t.voff;
        curSig = sig; curTex = glTex;
        cur = { blend: t.blend, shadInstr: t.shadInstr, ignoreTexA: t.ignoreTexA, punch: t.punch, textured };
      }
    }
    flush(vcount);
  }
}
