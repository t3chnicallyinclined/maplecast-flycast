// probe_twiddle_math.mjs — PURE MATH: does a full WxH PVR twiddle, sliced into 512B
// chunks at +0x200 steps, equal a sequence of independent 32x32 twiddled tiles?
// And in what GRID ORDER do the +0x200 chunks of a full WxH twiddle visit the 32x32 tiles?
// This needs NO capture — it's the twiddle algebra the renderer (twop) vs the engine
// (whole-part twiddle) disagree on.

// flycast twiddle (matches texture-manager twop / tw): interleave bits of x and y up to
// min(bx,by); above that, linear in the longer dimension.
function tw(x, y, w, h) {
    // returns the LINEAR PAL4-pixel index into the twiddled storage for pixel (x,y) of a WxH tex.
    const bx = Math.log2(w), by = Math.log2(h);
    const sq = Math.min(bx, by);
    let r = 0, b = 0;
    for (let i = 0; i < sq; i++) { r |= ((x >> i) & 1) << b; b++; r |= ((y >> i) & 1) << b; b++; }
    // remaining higher bits of the longer axis are appended linearly (PVR rectangular twiddle)
    if (bx > by) { r |= (x >> sq) << b; }
    else if (by > bx) { r |= (y >> sq) << b; }
    return r;
}

function tileGridOrder(W, H) {
    // For a full WxH twiddle, walk pixel index 0..W*H-1 in storage order; record which 32x32
    // tile (tileX,tileY) each 512-byte (1024-pixel) chunk predominantly covers.
    const idx2xy = new Array(W * H);
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) idx2xy[tw(x, y, W, H)] = [x, y];
    const txN = W / 32, tyN = H / 32;
    const chunks = [];
    for (let c = 0; c < (W * H) / 1024; c++) {
        // pixels c*1024 .. c*1024+1023
        const counts = new Map();
        for (let k = 0; k < 1024; k++) { const xy = idx2xy[c * 1024 + k]; if (!xy) continue; const tx = xy[0] >> 5, ty = xy[1] >> 5; const key = tx + ',' + ty; counts.set(key, (counts.get(key) || 0) + 1); }
        // dominant tile
        let best = '', bc = -1; for (const [k, v] of counts) if (v > bc) { bc = v; best = k; }
        chunks.push({ chunk: c, tile: best, pure: counts.size === 1 });
    }
    return { chunks, txN, tyN };
}

for (const [W, H] of [[32, 32], [32, 64], [32, 128], [64, 16], [64, 64], [128, 16], [128, 128], [64, 128]]) {
    const { chunks, txN, tyN } = tileGridOrder(W, H);
    const allPure = chunks.every(c => c.pure);
    const order = chunks.map(c => c.tile).join(' ');
    console.log(`${W}x${H} (${txN}x${tyN} tiles): each 512B chunk is one pure 32x32 tile? ${allPure}`);
    console.log(`   +0x200-step grid order (tileX,tileY): ${order}`);
}
