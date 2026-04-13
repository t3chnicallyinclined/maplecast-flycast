// renderer-bridge-webgpu.mjs — Drop-in WebGPU replacement for renderer-bridge.mjs
// Same API: initRenderer() + setOpt() — but uses pure JS WebGPU instead of WASM worker
//
// Swap in king.html: change import from './js/renderer-bridge.mjs' to './js/renderer-bridge-webgpu.mjs'

import { FrameDecoder } from '../webgpu/frame-decoder.mjs';
import { TAParser } from '../webgpu/ta-parser.mjs';
import { TextureManager } from '../webgpu/texture-manager.mjs';
import { PVR2Renderer } from '../webgpu/pvr2-renderer.mjs';
import { state } from './state.mjs';

let R, D, P, T;
let _canvas = null;
let _pendingFrame = null;
let _pendingSnap = null;
let _audioWorkletPort = null;
let _fc = 0;

// Default debug state matching webgpu-test.html gold standard
const DBG = {
    drawOpaque: true, drawPunch: true, drawTrans: true,
    shaderMode: 'normal', trDepthFunc: 6, trDepthWrite: true,
    noSort: true, singlePass: true, zEpsilon: 0.00005,
    opMax: 0, trMax: 0, opSkip: 0, trSkip: 0,
};

export async function initRenderer() {
    _canvas = document.getElementById('game-canvas');
    if (!_canvas) throw new Error('game-canvas element not found');
    _canvas.style.width = '100%';
    _canvas.style.height = '100%';

    // Init WebGPU renderer
    R = new PVR2Renderer();
    await R.init(_canvas);
    console.log('[webgpu-bridge] WebGPU renderer initialized');

    D = new FrameDecoder();
    P = new TAParser();
    T = new TextureManager(R.dev);

    // Connect to stream
    const { getRendererWsUrl, getRendererAudioWsUrl } = await import('./ws-connection.mjs');
    const wsUrl = await getRendererWsUrl();
    const audioWsUrl = await getRendererAudioWsUrl();

    console.log('[webgpu-bridge] Connecting to', wsUrl);
    connectStream(wsUrl);

    // Audio
    try {
        const audioCtx = new AudioContext({ sampleRate: 44100 });
        await audioCtx.audioWorklet.addModule('/pcm-worklet.js');
        const node = new AudioWorkletNode(audioCtx, 'pcm-processor', { outputChannelCount: [2] });
        node.connect(audioCtx.destination);
        _audioWorkletPort = node.port;
        if (audioCtx.state === 'suspended') {
            const unlock = () => { audioCtx.resume(); document.removeEventListener('click', unlock); };
            document.addEventListener('click', unlock);
        }
        console.log('[webgpu-bridge] Audio worklet ready');
    } catch (e) {
        console.warn('[webgpu-bridge] Audio init failed:', e);
    }

    // RAF render loop
    function renderTick() {
        requestAnimationFrame(renderTick);
        if (!_pendingFrame) return;
        const g = _pendingFrame, snap = _pendingSnap;
        _pendingFrame = null; _pendingSnap = null;
        R.renderFrame(g, T, snap, D.vram, DBG);
        _fc++;
    }
    requestAnimationFrame(renderTick);

    // Expose renderer controls globally for king.html UI integration
    state.diag = state.diag || {};
    state.diag.rendererType = 'webgpu';
    window.webgpuDBG = DBG;
    window.webgpuRenderer = R;
    window.webgpuTexMgr = T;
    console.log('[webgpu-bridge] Controls exposed: window.webgpuDBG (try webgpuDBG.bloom=true)');
}

function connectStream(wsUrl) {
    const ws = new WebSocket(wsUrl);
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => console.log('[webgpu-bridge] Stream connected');
    ws.onclose = () => { console.log('[webgpu-bridge] Stream disconnected, reconnecting...'); setTimeout(() => connectStream(wsUrl), 2000); };
    ws.onerror = () => {};
    ws.onmessage = (e) => {
        if (typeof e.data === 'string') return;
        const d = new Uint8Array(e.data);

        // Audio packet
        if (d.length >= 4 && d[0] === 0xAD && d[1] === 0x10) {
            if (_audioWorkletPort) {
                _audioWorkletPort.postMessage(new Int16Array(d.buffer, d.byteOffset + 4, (d.length - 4) / 2));
            }
            return;
        }

        try {
            const fr = D.applyFrame(d);
            if (!fr) return;

            T.setDirtyPages(fr.dirtyPageList, fr.pvrDirty);
            if (!T._pal || fr.vramDirty || fr.pvrDirty) T.updatePalette(D.pvrRegs);

            const g = P.parse(fr.taBuffer, fr.taSize);
            try { P.fillBGP(g, D.pvrRegs, D.vram); } catch (e2) {}

            _pendingFrame = g;
            _pendingSnap = fr.pvrSnapshot;
        } catch (ex) {
            console.error('[webgpu-bridge]', ex);
        }
    };
}

export function setOpt(opt, val) {
    // Map king.html quality options to WebGPU DBG state
    if (opt === 'quality') {
        // quality: 0=low, 1=medium, 2=high
    }
    DBG[opt] = val;
}
