// render-worker.mjs — Dedicated render thread for WebGPU renderer
// Owns: OffscreenCanvas, WebSocket, frame decode, TA parse, WebGPU render
// Main thread only handles debug panel UI + telemetry display

import { FrameDecoder } from './frame-decoder.mjs';
import { TAParser } from './ta-parser.mjs';
import { TextureManager } from './texture-manager.mjs';
import { PVR2Renderer } from './pvr2-renderer.mjs';

let R = null, D = null, P = null, T = null;
let ws = null, dbg = {};

// Telemetry (exponential moving averages)
const A = 0.05;
let maDecode=0, maParse=0, maRender=0, maTotal=0, maE2E=0;
let e2eMin=999, e2eMax=0, e2eCount=0, totalFrames=0;
let fps=0, fpsFrames=0, fpsLast=performance.now();

self.onmessage = async (e) => {
    const msg = e.data;
    switch (msg.type) {
        case 'init': return handleInit(msg);
        case 'dbg': dbg = msg.dbg || {}; return;
        case 'shutdown': if(ws) try{ws.close();}catch{}; return;
    }
};

async function handleInit(msg) {
    const canvas = msg.canvas;
    const wsUrl = msg.wsUrl;

    try {
        R = new PVR2Renderer();
        await R.init(canvas);
        D = new FrameDecoder();
        P = new TAParser();
        T = new TextureManager(R.dev);

        self.postMessage({ type: 'ready' });
        connectWS(wsUrl);
    } catch (err) {
        self.postMessage({ type: 'error', message: err.message });
    }
}

function connectWS(url) {
    ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => self.postMessage({ type: 'ws_open' });
    ws.onclose = () => { self.postMessage({ type: 'ws_close' }); setTimeout(() => connectWS(url), 2000); };
    ws.onerror = () => {};
    ws.onmessage = (e) => {
        if (typeof e.data === 'string') return;
        handleFrame(new Uint8Array(e.data));
    };
}

function handleFrame(data) {
    try {
        const wsArrival = performance.now();

        const t0 = performance.now();
        const fr = D.applyFrame(data);
        const decodeMs = performance.now() - t0;

        if (!fr) return;

        if (!T._lastPvrRegs || fr.vramDirty) { T.invalidateAll(); }
        T._lastPvrRegs = D.pvrRegs;
        if (fr.vramDirty || fr.pvrDirty) T.updatePalette(D.pvrRegs);

        const t1 = performance.now();
        const g = P.parse(fr.taBuffer, fr.taSize);
        try { P.fillBGP(g, D.pvrRegs, D.vram); } catch(e) {}
        const parseMs = performance.now() - t1;

        const t2 = performance.now();
        R.renderFrame(g, T, fr.pvrSnapshot, D.vram, dbg);
        const renderMs = performance.now() - t2;

        // Update moving averages
        const total = decodeMs + parseMs + renderMs;
        const e2e = performance.now() - wsArrival;
        maDecode += (decodeMs - maDecode) * A;
        maParse += (parseMs - maParse) * A;
        maRender += (renderMs - maRender) * A;
        maTotal += (total - maTotal) * A;
        maE2E += (e2e - maE2E) * A;
        if (e2e > e2eMax) e2eMax = e2e;
        if (e2e < e2eMin) e2eMin = e2e;
        e2eCount++;
        totalFrames++;
        fpsFrames++;

        // FPS calc
        const now = performance.now();
        if (now - fpsLast >= 2000) {
            fps = (fpsFrames / (now - fpsLast) * 1000).toFixed(1);
            fpsFrames = 0;
            fpsLast = now;
        }

        // Send telemetry to main every 30 frames
        if (totalFrames % 30 === 0) {
            const ts = T.stats;
            self.postMessage({
                type: 'telemetry',
                fps, maDecode, maParse, maRender, maTotal, maE2E,
                e2eMin, e2eMax, totalFrames,
                texHits: ts.hits, texMisses: ts.misses, texReused: ts.reused,
                passes: g.renderPasses ? g.renderPasses.length : 1,
                frameNum: D.frameNum,
                syncs: D.stats.syncs, keyframes: D.stats.keyframes,
            });
            ts.hits = 0; ts.misses = 0; ts.reused = 0;
            if (e2eCount > 300) { e2eCount = 0; e2eMax = 0; e2eMin = 999; }
        }
    } catch (ex) {
        // Don't spam errors
        if (totalFrames % 60 === 0) self.postMessage({ type: 'error', message: ex.message });
    }
}
