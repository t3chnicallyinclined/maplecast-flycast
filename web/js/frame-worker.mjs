// ============================================================================
// FRAME-WORKER.MJS — Dedicated WebSocket Worker for binary frame reception
//
// This Worker owns a separate WebSocket connection dedicated ONLY to binary
// frame data. No JSON. No parsing. No event loop contention with the UI.
//
// Frames are transferred to the main thread via postMessage with Transferable
// ArrayBuffer — ZERO COPY. The ArrayBuffer ownership moves from Worker heap
// to main thread heap without any memcpy.
//
// WHY: The main thread's event loop handles chat, queue, auth, DOM updates,
// gamepad polling, etc. Any of these can delay binary frame processing by
// 1-5ms. This Worker ensures frame data is received and forwarded with
// ZERO contention. The only thing on this thread is recv → transfer.
//
// OVERKILL IS NECESSARY.
// ============================================================================

let ws = null;
let frameCount = 0;

self.onmessage = (e) => {
  if (e.data.type === 'connect') {
    connect(e.data.url);
  } else if (e.data.type === 'disconnect') {
    if (ws) ws.close();
  }
};

function connect(url) {
  if (ws) ws.close();

  ws = new WebSocket(url);
  ws.binaryType = 'arraybuffer';

  ws.onopen = () => {
    self.postMessage({ type: 'status', status: 'connected' });
  };

  ws.onclose = () => {
    self.postMessage({ type: 'status', status: 'disconnected' });
    // Auto-reconnect
    setTimeout(() => connect(url), 2000);
  };

  ws.onmessage = (e) => {
    if (typeof e.data === 'string') {
      // Forward JSON to main thread (rare, for status/control)
      self.postMessage({ type: 'json', data: e.data });
      return;
    }

    // Binary frame — transfer ArrayBuffer ownership (ZERO COPY)
    frameCount++;
    self.postMessage(
      { type: 'frame', buffer: e.data, seq: frameCount },
      [e.data]  // Transferable list — moves ownership, no copy
    );
  };

  ws.onerror = () => {
    self.postMessage({ type: 'status', status: 'error' });
  };
}
