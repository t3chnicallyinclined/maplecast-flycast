// PCM Audio Worklet — runs on dedicated audio thread, never interrupted by main thread
class PCMProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.buffer = [];  // queue of Float32 stereo chunks
    this.started = false;  // wait for buffer to fill before playing
    this.port.onmessage = (e) => {
      // Receive Int16 PCM from main thread, convert to Float32
      const pcm = e.data;  // Int16Array, interleaved L,R,L,R
      const samples = pcm.length / 2;
      const left = new Float32Array(samples);
      const right = new Float32Array(samples);
      for (let i = 0; i < samples; i++) {
        left[i] = pcm[i * 2] / 32768.0;
        right[i] = pcm[i * 2 + 1] / 32768.0;
      }
      this.buffer.push({ left, right });
      // Keep buffer tight for low latency (~85ms max, 8 chunks × 10.67ms)
      while (this.buffer.length > 8) this.buffer.shift();
    };
  }

  process(inputs, outputs, parameters) {
    const outL = outputs[0][0];
    const outR = outputs[0][1];
    if (!outL || !outR) return true;

    // Pre-buffer: wait for 2 chunks (~21ms) before starting playback
    // Low latency for QUIC transport — minimal jitter absorption needed
    if (!this.started) {
      if (this.buffer.length < 2) {
        outL.fill(0);
        outR.fill(0);
        return true;
      }
      this.started = true;
    }

    let written = 0;
    while (written < outL.length && this.buffer.length > 0) {
      const chunk = this.buffer[0];
      const available = chunk.left.length;
      const toCopy = Math.min(available, outL.length - written);

      outL.set(chunk.left.subarray(0, toCopy), written);
      outR.set(chunk.right.subarray(0, toCopy), written);
      written += toCopy;

      if (toCopy >= available) {
        this.buffer.shift();
      } else {
        // Partial consume
        this.buffer[0] = {
          left: chunk.left.subarray(toCopy),
          right: chunk.right.subarray(toCopy)
        };
      }
    }

    // Fill remainder with silence
    for (let i = written; i < outL.length; i++) {
      outL[i] = 0;
      outR[i] = 0;
    }

    return true;
  }
}

registerProcessor('pcm-processor', PCMProcessor);
