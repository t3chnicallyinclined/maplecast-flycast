// mini-player.mjs — a small, self-contained live renderer for match previews.
// Reuses the exact WebGPU render pipeline king.html uses (PVR2Renderer +
// FrameDecoder + TAParser + TextureManager), but scoped to one instance, one
// canvas, and one node's relay URL — no audio, no post-processing, no globals.
//
//   const mp = new MiniPlayer(canvasEl, 'wss://sea.nobd.net/ws',
//                             { onstatus: s => ... });   // s: connecting|connected|playing|closed|gpu-error
//   mp.stop();   // tears down the socket + render loop
//
// WS-only: AdaptiveTransport skips WebTransport when no wtUrl is given, so we
// never need a per-node QUIC cert.

import { FrameDecoder } from './frame-decoder.mjs';
import { TAParser } from './ta-parser.mjs';
import { TextureManager } from './texture-manager.mjs';
import { PVR2Renderer } from './pvr2-renderer.mjs';
import { AdaptiveTransport } from './transport.mjs';

// Same gold-standard debug flags renderer-bridge-webgpu.mjs uses.
const DBG = {
  drawOpaque: true, drawPunch: true, drawTrans: true,
  shaderMode: 'normal', trDepthFunc: 6, trDepthWrite: true,
  noSort: true, singlePass: true, zEpsilon: 0.00005,
  opMax: 0, trMax: 0, opSkip: 0, trSkip: 0,
};

export class MiniPlayer {
  constructor(canvas, wsUrl, opts = {}) {
    this.canvas = canvas;
    this.wsUrl = wsUrl;
    this.onstatus = opts.onstatus || (() => {});
    this._pending = null; this._snap = null;
    this._raf = 0; this._alive = true; this._frames = 0;
    this._transport = null;
    this.onstatus('connecting');
    this._start();
  }

  async _start() {
    try {
      this.R = new PVR2Renderer();
      await this.R.init(this.canvas);
      this.D = new FrameDecoder();
      this.P = new TAParser();
      this.T = new TextureManager(this.R.dev);
    } catch (e) {
      console.warn('[mini-player] GPU init failed:', e);
      this.onstatus('gpu-error');
      return;
    }
    if (!this._alive) { this._teardown(); return; }  // stopped during async init

    this._transport = new AdaptiveTransport({
      wsUrl: this.wsUrl,                 // wtUrl omitted → WebSocket only
      onopen: () => this.onstatus('connected'),
      onclose: () => this.onstatus('closed'),
      onframe: (d) => this._onFrame(d),
    });
    this._transport.connect();

    const tick = () => {
      if (!this._alive) return;
      this._raf = requestAnimationFrame(tick);
      if (!this._pending) return;
      const g = this._pending, snap = this._snap;
      this._pending = null; this._snap = null;
      try { this.R.renderFrame(g, this.T, snap, this.D.vram, DBG, null); } catch (e) {}
      if (this._frames++ === 0) this.onstatus('playing');
    };
    this._raf = requestAnimationFrame(tick);
  }

  _onFrame(d) {
    if (d.length >= 4 && d[0] === 0xAD && d[1] === 0x10) return;  // skip audio
    try {
      const fr = this.D.applyFrame(d);
      if (!fr) return;
      this.T.setDirtyPages(fr.dirtyPageList, fr.pvrDirty);
      if (!this.T._pal || fr.vramDirty || fr.pvrDirty) this.T.updatePalette(this.D.pvrRegs);
      const g = this.P.parse(fr.taBuffer, fr.taSize);
      try { this.P.fillBGP(g, this.D.pvrRegs, this.D.vram); } catch (e) {}
      this._pending = g; this._snap = fr.pvrSnapshot;
    } catch (e) { /* one bad frame shouldn't kill the preview */ }
  }

  stop() { this._alive = false; this._teardown(); }

  _teardown() {
    cancelAnimationFrame(this._raf);
    try { this._transport && this._transport.close && this._transport.close(); } catch (e) {}
    try { this.R && this.R.destroy && this.R.destroy(); } catch (e) {}
  }
}
