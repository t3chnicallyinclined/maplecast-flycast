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
    this.parentSyncDc = null;    // reliable: SYNC chunks

    // Child connections (downstream — where we forward frames to)
    this.children = new Map();   // peerId -> { pc, mirrorDc, syncDc, syncSent }

    // Cached SYNC state — maintained live by applying dirty pages
    this.cachedVram = null;      // Uint8Array(8MB)
    this.cachedPvr = null;       // Uint8Array(32KB)
    this.syncReceived = false;

    // Frame ordering
    this.highestFrameNum = 0;

    // Stats
    this.framesReceived = 0;
    this.framesRelayed = 0;
    this.framesDropped = 0;

    // SYNC chunk reassembly (receiving from parent)
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
    this._cacheSyncFromRaw(data);
    this.onSync(data);
  }

  // ---- Frame Pipeline ----

  _onFrameFromUpstream(data) {
    // Frame ordering: discard late arrivals
    const view = new DataView(data instanceof ArrayBuffer ? data : data.buffer);
    if (data.byteLength >= 8) {
      const frameNum = view.getUint32(4, true);
      if (frameNum <= this.highestFrameNum && this.highestFrameNum - frameNum < 1000) {
        // Late frame — skip (but allow wraparound)
        return;
      }
      this.highestFrameNum = frameNum;
    }

    this.framesReceived++;

    // Feed to WASM renderer
    this.onFrame(data);

    // Update cached SYNC state with dirty pages from this frame
    this._updateCachedState(data);

    // Forward to children (if seed or relay)
    if (this.role === 'seed' || this.role === 'relay') {
      this._forwardToChildren(data);
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

  // ---- SYNC Caching ----

  _cacheSyncFromRaw(data) {
    // Parse SYNC: "SYNC"(4) + vramSize(4) + vram(N) + pvrSize(4) + pvr(N)
    const arr = new Uint8Array(data instanceof ArrayBuffer ? data : data.buffer);
    if (arr.length < 12) return;
    const view = new DataView(arr.buffer, arr.byteOffset);

    let off = 4; // skip "SYNC"
    const vramSize = view.getUint32(off, true); off += 4;
    this.cachedVram = new Uint8Array(vramSize);
    this.cachedVram.set(arr.subarray(off, off + vramSize));
    off += vramSize;

    const pvrSize = view.getUint32(off, true); off += 4;
    this.cachedPvr = new Uint8Array(pvrSize);
    this.cachedPvr.set(arr.subarray(off, off + pvrSize));
  }

  _updateCachedState(data) {
    // Apply dirty pages from delta frame to cached VRAM/PVR
    if (!this.cachedVram || !this.cachedPvr) return;

    const arr = new Uint8Array(data instanceof ArrayBuffer ? data : data.buffer);
    if (arr.length < 80) return;
    const view = new DataView(arr.buffer, arr.byteOffset);

    // Skip header: frameSize(4) + frameNum(4) + pvr_snapshot(64)
    let off = 72;
    const taSize = view.getUint32(off, true); off += 4;
    const deltaPayloadSize = view.getUint32(off, true); off += 4;

    // Skip TA data + checksum
    off += deltaPayloadSize + 4;

    // Parse dirty pages
    if (off + 4 > arr.length) return;
    const dirtyCount = view.getUint32(off, true); off += 4;

    for (let d = 0; d < dirtyCount; d++) {
      if (off + 5 + 4096 > arr.length) break;
      const regionId = arr[off++];
      const pageIdx = view.getUint32(off, true); off += 4;
      const pageOff = pageIdx * 4096;

      if (regionId === 1 && pageOff + 4096 <= this.cachedVram.length) {
        this.cachedVram.set(arr.subarray(off, off + 4096), pageOff);
      } else if (regionId === 3 && pageOff + 4096 <= this.cachedPvr.length) {
        this.cachedPvr.set(arr.subarray(off, off + 4096), pageOff);
      }
      off += 4096;
    }
  }

  _buildSyncBuffer() {
    // Build SYNC packet from cached state
    if (!this.cachedVram || !this.cachedPvr) return null;
    const vramSize = this.cachedVram.length;
    const pvrSize = this.cachedPvr.length;
    const totalSize = 4 + 4 + vramSize + 4 + pvrSize;
    const buf = new Uint8Array(totalSize);
    const view = new DataView(buf.buffer);

    let off = 0;
    buf[off++] = 0x53; buf[off++] = 0x59; buf[off++] = 0x4E; buf[off++] = 0x43; // "SYNC"
    view.setUint32(off, vramSize, true); off += 4;
    buf.set(this.cachedVram, off); off += vramSize;
    view.setUint32(off, pvrSize, true); off += 4;
    buf.set(this.cachedPvr, off);

    return buf;
  }

  // ---- SYNC Chunked Transfer ----

  _sendSyncToChild(childId) {
    const child = this.children.get(childId);
    if (!child || !child.syncDc || child.syncDc.readyState !== 'open') return;
    if (child.syncSent) return;

    const syncBuf = this._buildSyncBuffer();
    if (!syncBuf) {
      console.warn('[relay] No cached SYNC to send');
      return;
    }

    // Chunk into 64KB pieces: [chunkIdx:u16][totalChunks:u16][data:<=64KB]
    const CHUNK_SIZE = 64 * 1024;
    const totalChunks = Math.ceil(syncBuf.length / CHUNK_SIZE);

    console.log(`[relay] Sending SYNC to ${childId}: ${(syncBuf.length / 1024 / 1024).toFixed(1)} MB in ${totalChunks} chunks`);

    for (let i = 0; i < totalChunks; i++) {
      const start = i * CHUNK_SIZE;
      const end = Math.min(start + CHUNK_SIZE, syncBuf.length);
      const chunkData = syncBuf.subarray(start, end);

      const packet = new Uint8Array(4 + chunkData.length);
      const pView = new DataView(packet.buffer);
      pView.setUint16(0, i, true);           // chunkIdx
      pView.setUint16(2, totalChunks, true);  // totalChunks
      packet.set(chunkData, 4);

      try { child.syncDc.send(packet); }
      catch (e) { console.error('[relay] SYNC chunk send failed:', e); return; }
    }
    child.syncSent = true;
  }

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

      console.log(`[relay] SYNC reassembled: ${(totalLen / 1024 / 1024).toFixed(1)} MB`);
      this._syncChunks = null;
      this.syncReceived = true;
      this._cacheSyncFromRaw(full.buffer);
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
    const child = { pc, mirrorDc: null, syncDc: null, syncSent: false };
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
          console.log(`[relay] Sync DC to child ${childId} OPEN — sending SYNC`);
          this._sendSyncToChild(childId);
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
