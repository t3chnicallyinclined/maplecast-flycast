// renderer-bridge-webgpu.mjs — Drop-in WebGPU replacement for renderer-bridge.mjs
// Same API: initRenderer() + setOpt() — but uses pure JS WebGPU instead of WASM worker
//
// Swap in king.html: change import from './js/renderer-bridge.mjs' to './js/renderer-bridge-webgpu.mjs'

import { FrameDecoder } from '../webgpu/frame-decoder.mjs';
import { TAParser } from '../webgpu/ta-parser.mjs';
import { TextureManager } from '../webgpu/texture-manager.mjs';
import { PVR2Renderer } from '../webgpu/pvr2-renderer.mjs';
import { PostProcessor } from '../webgpu/post-process.mjs';
import { AdaptiveTransport } from '../webgpu/transport.mjs';
import { state } from './state.mjs';

let R, D, P, T, PP;
let _canvas = null;
let _pendingFrame = null;
let _pendingSnap = null;
let _audioWorkletPort = null;
let _fc = 0;

// Telemetry — exported so diagnostics.mjs can read it
export const _telemetry = {
    framesRendered: 0,
    framesDropped: 0,
    intervalSumUs: 0,
    intervalCount: 0,
    intervalMaxUs: 0,
    _lastFrameAt: 0,
};

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

    // Connect via AdaptiveTransport (WebTransport QUIC first, WebSocket fallback)
    const { getRendererWsUrl } = await import('./ws-connection.mjs');
    const wsUrl = await getRendererWsUrl();
    const host = location.hostname;

    const transport = new AdaptiveTransport({
        wsUrl: wsUrl,
        wtUrl: `https://${host}/webtransport`,
        onstatus: (msg) => console.log('[webgpu-bridge]', msg),
        onopen: () => console.log('[webgpu-bridge] Stream connected via', transport.type),
        onclose: () => {
            console.log('[webgpu-bridge] Stream disconnected');
            // Mirror WASM bridge cleanup (renderer-bridge.mjs:260) — drops the
            // streaming flag so lobby.mjs's next status tick re-shows the idle
            // screen overlay. Without this, a dropped stream leaves a dead
            // canvas with nothing visually indicating disconnect.
            state.rendererStreaming = false;
            document.body.classList.remove('streaming');
        },
        onframe: handleFrame,
    });
    transport.connect();
    window._transport = transport; // Expose for gamepad QUIC input

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

    // Init post-processor for resolution scaling + effects
    PP = new PostProcessor();
    PP.init(R.dev, R.fmt);

    // RAF render loop with telemetry
    function renderTick() {
        requestAnimationFrame(renderTick);
        if (!_pendingFrame) return;
        const g = _pendingFrame, snap = _pendingSnap;
        _pendingFrame = null; _pendingSnap = null;

        // Resolution scaling via PostProcessor
        const scale = DBG.resScale || 1;
        PP.ensureTargets(640, 480, scale);
        const rt = scale > 1 ? PP.getRenderTarget() : null;
        R.renderFrame(g, T, snap, D.vram, DBG, rt);
        if (rt && R._lastEncoder) {
            PP.blit(R._lastEncoder, R.ctx.getCurrentTexture().createView(), _canvas.width, _canvas.height, DBG);
            R.dev.queue.submit([R._lastEncoder.finish()]);
        }

        // Telemetry — track real jitter (deviation from 16.67ms)
        const now = performance.now();
        if (_telemetry._lastFrameAt > 0) {
            const intervalUs = (now - _telemetry._lastFrameAt) * 1000;
            const deviationUs = Math.abs(intervalUs - 16667); // deviation from perfect 60fps
            _telemetry.intervalSumUs += deviationUs;
            _telemetry.intervalCount++;
            if (deviationUs > _telemetry.intervalMaxUs) _telemetry.intervalMaxUs = deviationUs;
        }
        _telemetry._lastFrameAt = now;

        // FIRST FRAME: dismiss the idle screen overlay and flip the streaming
        // flag so lobby.mjs (line 60) stops re-asserting display:flex on every
        // 1Hz status tick. Mirrors the WASM bridge's sync_applied handler at
        // renderer-bridge.mjs:265-268 — the WebGPU bridge swap on 2026-04-13
        // missed this and the bug only surfaced once gameplay had idle gaps.
        if (_telemetry.framesRendered === 0) {
            const idle = document.getElementById('idleScreen');
            if (idle) idle.style.display = 'none';
            document.body.classList.add('streaming');
            state.rendererStreaming = true;
            state.connState = 'LIVE';
            console.log('[webgpu-bridge] First frame rendered — idle screen dismissed');
        }
        _telemetry.framesRendered++;
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

function handleFrame(d) {
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

        if (_pendingFrame) _telemetry.framesDropped++;
        _pendingFrame = g;
        _pendingSnap = fr.pvrSnapshot;
    } catch (ex) {
        console.error('[webgpu-bridge]', ex);
    }
}

export function setOpt(opt, val) {
    // Map king.html WASM option indices to WebGPU DBG state
    // opt 0 = resolution, opt 2 = fog, opt 3 = modvol, opt 4 = perstrip sort
    // opt 5 = aniso, opt 6 = tex filter, opt 7 = transparency layers
    if (opt === 0) {
        // Resolution: val is height string (480, 720, 960, 1440, 1920)
        const h = parseInt(val) || 480;
        DBG.resScale = Math.round(h / 480);
    }
    // Named opts from direct DBG access
    if (typeof opt === 'string') DBG[opt] = val;
}

// Wire king.html's CSS effects to WebGPU post-processing
// Called by applyCSSEffects() in settings.mjs — we override it
window.applyCSSEffects = function() {
    if (!window.webgpuDBG) return;
    const D = window.webgpuDBG;
    D.crtAdv = !!document.getElementById('css_scanlines')?.checked;
    D.bloom = !!document.getElementById('css_bloom')?.checked;
    D.sharp = document.getElementById('css_sharpen')?.checked ? 0.5 : 0;
    D.vignette = document.getElementById('css_vignette')?.checked ? 0.5 : 0;
    // Brightness/contrast/saturation sliders
    const bright = document.getElementById('css_bright');
    if (bright) { D.bright = bright.value / 100; const v = document.getElementById('css_bright_val'); if(v) v.textContent = bright.value + '%'; }
    const contrast = document.getElementById('css_contrast');
    if (contrast) { D.contrast = contrast.value / 100; const v = document.getElementById('css_contrast_val'); if(v) v.textContent = contrast.value + '%'; }
    const sat = document.getElementById('css_sat');
    if (sat) { D.sat = sat.value / 100; const v = document.getElementById('css_sat_val'); if(v) v.textContent = sat.value + '%'; }
};

// Override presets
window.presetMaxPerformance = function() {
    Object.assign(DBG, { fxaa:false, bloom:false, crtAdv:false, grain:false, sharp:0, resScale:1, sat:1, contrast:1, bright:1 });
};
window.presetArcade = function() {
    Object.assign(DBG, { crtAdv:true, scanlines:0.3, vignette:0.3, bloom:false, sat:1.1, contrast:1.1, bright:1, resScale:1 });
};
window.presetMaxQuality = function() {
    Object.assign(DBG, { fxaa:true, bloom:true, bloomAmt:0.2, sharp:0.3, sat:1.1, contrast:1.05, bright:1, resScale:2 });
};
