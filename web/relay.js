// ============================================================================
// RELAY.JS — P2P Spectator Relay for MapleCast TA Mirror
//
// Server feeds 2-3 seed spectators. Seeds relay delta frames to children
// via WebRTC DataChannels. Each relay node serves up to 3 children.
// Tree topology managed by server via WebSocket JSON messages.
//
// DataChannels:
//   ta-mirror  — unreliable, unordered (delta frames, ~65KB @ 60fps)
//   ta-sync    — reliable, ordered (8MB initial SYNC, chunked to 64KB)
//
// OVERKILL IS NECESSARY.
// ============================================================================
//
// !!! FRAGILE — DO NOT DECOMPRESS OR PARSE FRAMES IN JS !!!
//
// This relay forwards WIRE BYTES verbatim. Browsers receive compressed (ZCST)
// frames from the upstream and either render them locally OR pass them
// through to children unchanged. Decompression happens INSIDE the WASM
// renderer (packages/renderer/src/wasm_bridge.cpp). The reason: there is no
// pure-JS zstd decoder in this app. If you "improve" this file by parsing or
// decompressing frames in JS, you will:
//
//   1. Add latency that we explicitly tuned out (the whole streaming path
//      exists to be sub-5ms end-to-end on LAN).
//   2. Re-introduce the bugs we already fixed in the WASM bridges by
//      duplicating the parser in a fifth language.
//
// The ONLY thing this file is allowed to read out of the wire bytes is the
// ZCST magic at offset 0 (to detect compressed vs uncompressed) and the
// uncompressedSize at offset 4 (to distinguish compressed SYNC from
// compressed delta). Anything else MUST stay opaque.
//
// SYNC handling rule (post-2026-04-06): the relay does NOT cache or rebuild
// SYNC packets from delta state. The server is the single source of truth
// for SYNC. Two paths:
//
//   1. New child connecting to a relay parent → server sends a fresh
//      compressed SYNC over WebSocket directly to the child via onOpen().
//      The relay parent does NOT send a cached SYNC — that path is gone.
//
//   2. Mid-stream scene change → server pushes a fresh compressed SYNC over
//      WebSocket to all seeds. Seeds forward it to children over the
//      reliable ta-sync DataChannel chunked into 64KB pieces.
//
// Why no cache: maintaining a cached SYNC by applying delta dirty pages
// inherits any bug in the dirty-page-tracking. If the server misses a DMA
// write, the relay's cache is stale and every new child inherits the
// staleness. Trusting only the server SYNC is simpler and correct.
//
// See docs/ARCHITECTURE.md "Mirror Wire Format — Rules of the Road" for the
// canonical list of rules all four parsers must obey.
// ============================================================================

class MapleCastRelay {
  constructor(ws, onFrame, onSync) {
    this.ws = ws;
    this.onFrame = onFrame;   // (ArrayBuffer) => void — feeds WASM renderer
    this.onSync = onSync;     // (ArrayBuffer) => void — feeds WASM renderer SYNC
    this.peerId = null;
    this.role = null;         // 'seed', 'relay', 'leaf' — set by server

    // Parent connection (upstream — where we receive frames from)
    this.parentPc = null;
    this.parentMirrorDc = null;  // unreliable: delta frames
    this.parentSyncDc = null;    // reliable: mid-stream SYNC chunks

    // Child connections (downstream — where we forward frames to)
    this.children = new Map();   // peerId -> { pc, mirrorDc, syncDc }

    this.syncReceived = false;

    // Frame ordering
    this.highestFrameNum = 0;

    // Stats
    this.framesReceived = 0;
    this.framesRelayed = 0;
    this.framesDropped = 0;

    // SYNC chunk reassembly (receiving mid-stream SYNC from parent)
    this._syncChunks = null;
    this._syncTotalChunks = 0;
    this._syncReceivedChunks = 0;

    // RTC config
    this.rtcConfig = {
      iceServers: [
        { urls: ['stun:stun.l.google.com:19302', 'stun:stun1.l.google.com:19302'] }
      ]
    };
  }

  // ---- Public API ----

  handleMessage(msg) {
    switch (msg.type) {
      case 'relay_role':
        this.peerId = msg.peerId;
        this.role = msg.role;
        console.log(`[relay] Role assigned: ${this.role} (peerId=${this.peerId})`);
        break;

      case 'relay_assign_parent':
        this._connectToParent(msg.parentId);
        break;

      case 'relay_assign_child':
        // Child will initiate — we just wait for the offer via relay_signal
        console.log(`[relay] Expecting child: ${msg.childId}`);
        break;

      case 'relay_remove_child':
        this._removeChild(msg.childId);
        break;

      case 'relay_orphaned':
        console.log('[relay] Parent died — waiting for reassignment');
        this._cleanupParent();
        break;

      case 'relay_signal':
        this._handleSignal(msg.fromPeerId, msg.signal);
        break;
    }
  }

  // Called when a frame arrives from server WebSocket (seeds only)
  handleServerFrame(data) {
    this._onFrameFromUpstream(data);
  }

  // Called when SYNC arrives from server WebSocket (seeds only)
  handleServerSync(data) {
    this.syncReceived = true;
    this.onSync(data);
  }

  // ---- Frame Pipeline ----

  _onFrameFromUpstream(data) {
    const view = new DataView(data instanceof ArrayBuffer ? data : data.buffer);

    // Audio packet detection FIRST — audio shares the binary WebSocket
    // with video frames and rides the same P2P fan-out tree, so every
    // child relay in the tree sees audio packets alongside video.
    //
    // Audio header: [0xAD][0x10][seqHi][seqLo][512 × int16 stereo PCM]
    // = 2052 bytes. We detect before any video-frame parsing runs so the
    // audio bytes never touch the frameNum discard logic (bytes 4-7 of an
    // audio packet are PCM samples, not a frame counter).
    const isAudio = data.byteLength >= 4 &&
      view.getUint8(0) === 0xAD && view.getUint8(1) === 0x10;

    if (isAudio) {
      this.framesReceived++;           // audio counts as traffic too
      this.onFrame(data);              // route to handler; client detects audio, fans to worklet
      if (this.role === 'seed' || this.role === 'relay') {
        // Audio is tiny and unreliable-delivery-tolerant — use the same
        // delta-frame fan-out path (fire-and-forget via ta-mirror DC).
        this._forwardToChildren(data);
      }
      return;
    }

    // Detect ZCST compressed frame — magic bytes 0x5A 0x43 0x53 0x54
    // (server-side compression of large packets; SYNCs are always ZCST)
    const isCompressed = data.byteLength >= 4 &&
      view.getUint32(0, true) === 0x5453435A;
    // Detect raw SYNC magic — server side scene-change broadcasts may also
    // arrive uncompressed if zstd is disabled.
    const isRawSync = data.byteLength >= 4 &&
      view.getUint32(0, true) === 0x434E5953; // "SYNC" little-endian

    // Compressed frames + raw SYNCs are full-state packets, not deltas.
    // They get a different forwarding path (reliable, chunked).
    const isFullState = isCompressed || isRawSync;

    // Frame ordering: discard late arrivals (only for uncompressed deltas —
    // compressed/SYNC frames have no frameNum in a stable position)
    if (!isFullState && data.byteLength >= 8) {
      const frameNum = view.getUint32(4, true);
      if (frameNum <= this.highestFrameNum && this.highestFrameNum - frameNum < 1000) {
        // Late frame — skip (but allow wraparound)
        return;
      }
      this.highestFrameNum = frameNum;
    }

    this.framesReceived++;

    // Feed to WASM renderer (WASM handles decompression internally)
    this.onFrame(data);

    // Forward to children (if seed or relay)
    if (this.role === 'seed' || this.role === 'relay') {
      if (isFullState) {
        // Full-state SYNC packets are too big for unreliable DataChannel
        // (1MB+, and SCTP message size caps at ~256KB in Chrome). Send
        // them over the reliable, ordered ta-sync DC chunked into 64KB
        // pieces.
        this._forwardFullStateToChildren(data);
      } else {
        // Delta frames — fire-and-forget over unreliable ta-mirror DC.
        this._forwardToChildren(data);
      }
    }
  }

  _forwardToChildren(data) {
    for (const [childId, child] of this.children) {
      if (child.mirrorDc && child.mirrorDc.readyState === 'open') {
        // Backpressure: skip if child is lagging (> ~2 frames buffered)
        if (child.mirrorDc.bufferedAmount > 150000) {
          this.framesDropped++;
          continue;
        }
        try {
          child.mirrorDc.send(data);
          this.framesRelayed++;
        } catch (e) { /* DC closing */ }
      }
    }
  }

  _forwardFullStateToChildren(data) {
    // Chunk a full-state SYNC packet over each child's reliable ta-sync DC.
    // Wire format: [chunkIdx:u16][totalChunks:u16][data]
    const arr = data instanceof ArrayBuffer ? new Uint8Array(data) : new Uint8Array(data.buffer);
    const CHUNK_SIZE = 64 * 1024;
    const totalChunks = Math.ceil(arr.length / CHUNK_SIZE);

    let sentTo = 0;
    for (const [childId, child] of this.children) {
      if (!child.syncDc || child.syncDc.readyState !== 'open') continue;
      try {
        for (let i = 0; i < totalChunks; i++) {
          const start = i * CHUNK_SIZE;
          const end = Math.min(start + CHUNK_SIZE, arr.length);
          const chunkData = arr.subarray(start, end);
          const packet = new Uint8Array(4 + chunkData.length);
          const pView = new DataView(packet.buffer);
          pView.setUint16(0, i, true);
          pView.setUint16(2, totalChunks, true);
          packet.set(chunkData, 4);
          child.syncDc.send(packet);
        }
        sentTo++;
      } catch (e) {
        console.warn(`[relay] full-state SYNC forward to ${childId} failed:`, e?.message);
      }
    }
    if (sentTo > 0) {
      console.log(`[relay] forwarded full-state SYNC (${(arr.length / 1024 / 1024).toFixed(1)} MB, ${totalChunks} chunks) to ${sentTo} children`);
    }
  }

  // ---- SYNC chunk reassembly (mid-stream SYNC forwarded from upstream) ----

  _handleSyncChunk(data) {
    const arr = new Uint8Array(data);
    if (arr.length < 4) return;
    const view = new DataView(arr.buffer);
    const chunkIdx = view.getUint16(0, true);
    const totalChunks = view.getUint16(2, true);
    const chunkData = arr.subarray(4);

    if (!this._syncChunks || this._syncTotalChunks !== totalChunks) {
      this._syncChunks = new Array(totalChunks);
      this._syncTotalChunks = totalChunks;
      this._syncReceivedChunks = 0;
    }

    if (!this._syncChunks[chunkIdx]) {
      this._syncChunks[chunkIdx] = chunkData.slice(); // copy
      this._syncReceivedChunks++;
    }

    if (this._syncReceivedChunks === totalChunks) {
      // Reassemble
      let totalLen = 0;
      for (const c of this._syncChunks) totalLen += c.length;
      const full = new Uint8Array(totalLen);
      let off = 0;
      for (const c of this._syncChunks) { full.set(c, off); off += c.length; }

      console.log(`[relay] mid-stream SYNC reassembled: ${(totalLen / 1024 / 1024).toFixed(1)} MB`);
      this._syncChunks = null;
      this.syncReceived = true;
      this.onSync(full.buffer);
    }
  }

  // ---- WebRTC Connection Management ----

  _connectToParent(parentId) {
    console.log(`[relay] Connecting to parent ${parentId}`);
    this._cleanupParent();

    const pc = new RTCPeerConnection(this.rtcConfig);
    this.parentPc = pc;

    // Create DataChannels (offerer creates them)
    this.parentMirrorDc = pc.createDataChannel('ta-mirror', {
      ordered: false, maxRetransmits: 0
    });
    this.parentSyncDc = pc.createDataChannel('ta-sync', {
      ordered: true  // reliable for SYNC
    });

    this.parentMirrorDc.binaryType = 'arraybuffer';
    this.parentSyncDc.binaryType = 'arraybuffer';

    this.parentMirrorDc.onopen = () => {
      console.log('[relay] Mirror DC to parent OPEN');
    };
    this.parentMirrorDc.onmessage = (e) => {
      this._onFrameFromUpstream(e.data);
    };

    this.parentSyncDc.onopen = () => {
      console.log('[relay] Sync DC to parent OPEN');
    };
    this.parentSyncDc.onmessage = (e) => {
      this._handleSyncChunk(e.data);
    };

    pc.onicecandidate = (e) => {
      if (e.candidate) {
        this.ws.send(JSON.stringify({
          type: 'relay_signal',
          toPeerId: parentId,
          signal: { type: 'ice-candidate', candidate: e.candidate }
        }));
      }
    };

    pc.onconnectionstatechange = () => {
      if (pc.connectionState === 'disconnected' || pc.connectionState === 'failed') {
        console.log('[relay] Parent connection lost');
        this.ws.send(JSON.stringify({ type: 'relay_parent_lost' }));
        this._cleanupParent();
      }
    };

    // Create and send offer
    pc.createOffer().then(offer => {
      return pc.setLocalDescription(offer);
    }).then(() => {
      this.ws.send(JSON.stringify({
        type: 'relay_signal',
        toPeerId: parentId,
        signal: { type: 'offer', sdp: pc.localDescription }
      }));
    }).catch(e => console.error('[relay] Offer creation failed:', e));
  }

  _handleSignal(fromPeerId, signal) {
    if (signal.type === 'offer') {
      // Incoming child connection
      this._acceptChild(fromPeerId, signal);
    } else if (signal.type === 'answer') {
      // Answer to our offer (from parent)
      if (this.parentPc) {
        this.parentPc.setRemoteDescription(new RTCSessionDescription(signal.sdp))
          .catch(e => console.error('[relay] setRemoteDescription failed:', e));
      }
    } else if (signal.type === 'ice-candidate') {
      // ICE candidate — figure out which PC it's for
      const child = this.children.get(fromPeerId);
      if (child && child.pc) {
        child.pc.addIceCandidate(new RTCIceCandidate(signal.candidate))
          .catch(e => {});
      } else if (this.parentPc) {
        this.parentPc.addIceCandidate(new RTCIceCandidate(signal.candidate))
          .catch(e => {});
      }
    }
  }

  _acceptChild(childId, signal) {
    console.log(`[relay] Accepting child ${childId}`);

    const pc = new RTCPeerConnection(this.rtcConfig);
    const child = { pc, mirrorDc: null, syncDc: null };
    this.children.set(childId, child);

    pc.ondatachannel = (e) => {
      const dc = e.channel;
      dc.binaryType = 'arraybuffer';

      if (dc.label === 'ta-mirror') {
        child.mirrorDc = dc;
        dc.onopen = () => {
          console.log(`[relay] Mirror DC to child ${childId} OPEN`);
        };
      } else if (dc.label === 'ta-sync') {
        child.syncDc = dc;
        dc.onopen = () => {
          // No initial SYNC push — child gets fresh SYNC from server WS
          // (onOpen handler) on connect. This DC is reserved for mid-stream
          // SYNC forwards on scene change.
          console.log(`[relay] Sync DC to child ${childId} OPEN`);
        };
      }
    };

    pc.onicecandidate = (e) => {
      if (e.candidate) {
        this.ws.send(JSON.stringify({
          type: 'relay_signal',
          toPeerId: childId,
          signal: { type: 'ice-candidate', candidate: e.candidate }
        }));
      }
    };

    pc.onconnectionstatechange = () => {
      if (pc.connectionState === 'disconnected' || pc.connectionState === 'failed') {
        console.log(`[relay] Child ${childId} disconnected`);
        this._removeChild(childId);
      }
    };

    // Set offer and create answer
    pc.setRemoteDescription(new RTCSessionDescription(signal.sdp)).then(() => {
      return pc.createAnswer();
    }).then(answer => {
      return pc.setLocalDescription(answer);
    }).then(() => {
      this.ws.send(JSON.stringify({
        type: 'relay_signal',
        toPeerId: childId,
        signal: { type: 'answer', sdp: pc.localDescription }
      }));
    }).catch(e => console.error('[relay] Answer creation failed:', e));
  }

  // ---- Cleanup ----

  _cleanupParent() {
    if (this.parentMirrorDc) { try { this.parentMirrorDc.close(); } catch(e){} }
    if (this.parentSyncDc) { try { this.parentSyncDc.close(); } catch(e){} }
    if (this.parentPc) { try { this.parentPc.close(); } catch(e){} }
    this.parentPc = null;
    this.parentMirrorDc = null;
    this.parentSyncDc = null;
  }

  _removeChild(childId) {
    const child = this.children.get(childId);
    if (!child) return;
    if (child.mirrorDc) { try { child.mirrorDc.close(); } catch(e){} }
    if (child.syncDc) { try { child.syncDc.close(); } catch(e){} }
    if (child.pc) { try { child.pc.close(); } catch(e){} }
    this.children.delete(childId);
    console.log(`[relay] Removed child ${childId} (${this.children.size} remaining)`);
  }

  destroy() {
    this._cleanupParent();
    for (const [id] of this.children) this._removeChild(id);
    this.children.clear();
  }

  // ---- Stats ----

  getStats() {
    return {
      peerId: this.peerId,
      role: this.role,
      children: this.children.size,
      framesReceived: this.framesReceived,
      framesRelayed: this.framesRelayed,
      framesDropped: this.framesDropped,
      syncReceived: this.syncReceived,
      highestFrame: this.highestFrameNum
    };
  }
}
