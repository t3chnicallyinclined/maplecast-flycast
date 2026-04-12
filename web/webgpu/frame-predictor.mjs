// frame-predictor.mjs — Vertex motion extrapolation for predictive rendering
// Keeps last 2 frames of vertex positions. Predicts next frame by linear extrapolation.
// During idle vblank window, pre-renders predicted frame. Snaps on real data arrival.
//
// Only extrapolates XY positions (not Z, UV, colors) — Z/UV changes mean new geometry.

const STRIDE = 7; // floats per vertex: x, y, z, col, spc, u, v

export class FramePredictor {
    constructor() {
        this._prev = null;   // Float32Array of previous frame's vertex positions
        this._curr = null;   // Float32Array of current frame's vertex positions
        this._pred = null;   // Float32Array of predicted next frame
        this._vertCount = 0;
        this._hasTwoFrames = false;
        this.stats = { predictions: 0, snaps: 0 };
    }

    // Feed a new real frame's vertex data
    feedFrame(vertexData, vertexCount) {
        const floats = new Float32Array(vertexData.buffer, vertexData.byteOffset, vertexCount * STRIDE);

        // Shift: curr → prev
        if (this._curr) {
            if (!this._prev || this._prev.length < floats.length) {
                this._prev = new Float32Array(floats.length);
            }
            this._prev.set(this._curr.subarray(0, Math.min(this._curr.length, floats.length)));
            this._hasTwoFrames = true;
        }

        // Store current
        if (!this._curr || this._curr.length < floats.length) {
            this._curr = new Float32Array(floats.length);
        }
        this._curr.set(floats);
        this._vertCount = vertexCount;
        this.stats.snaps++;
    }

    // Generate a predicted frame by extrapolating XY positions
    // t = interpolation factor (1.0 = one full frame ahead)
    predict(t) {
        if (!this._hasTwoFrames || !this._curr || !this._prev) return null;

        const n = this._vertCount * STRIDE;
        if (!this._pred || this._pred.length < n) {
            this._pred = new Float32Array(n);
        }

        // Copy current frame as base
        this._pred.set(this._curr.subarray(0, n));

        // Extrapolate XY only (indices 0,1 of each vertex)
        for (let i = 0; i < this._vertCount; i++) {
            const o = i * STRIDE;
            const dx = this._curr[o] - this._prev[o];       // X velocity
            const dy = this._curr[o + 1] - this._prev[o + 1]; // Y velocity

            // Only extrapolate if velocity is reasonable (< 20px/frame)
            // Prevents wild predictions during scene changes
            if (Math.abs(dx) < 20 && Math.abs(dy) < 20) {
                this._pred[o] = this._curr[o] + dx * t;
                this._pred[o + 1] = this._curr[o + 1] + dy * t;
            }
        }

        this.stats.predictions++;
        return {
            data: new Uint8Array(this._pred.buffer, 0, n * 4),
            vertexCount: this._vertCount,
        };
    }

    get ready() { return this._hasTwoFrames; }
}
