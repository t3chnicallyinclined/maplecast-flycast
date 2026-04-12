// decode-worker.mjs — Decode + parse on a separate CPU core
// Main thread only does GPU render (~1.39ms) while this worker
// prepares the next frame's vertex data (~0.48ms) in parallel.
//
// Protocol:
//   main → worker: { type:'frame', data: ArrayBuffer (transferred) }
//   worker → main: { type:'parsed', ...result (transferred) }
//   worker → main: { type:'sync' }  (SYNC received, no render data)

import { FrameDecoder } from './frame-decoder.mjs';
import { TAParser } from './ta-parser.mjs';

const D = new FrameDecoder();
const P = new TAParser();

self.onmessage = (e) => {
    if (e.data.type === 'frame') {
        try {
            const raw = new Uint8Array(e.data.data);
            const fr = D.applyFrame(raw);

            if (!fr) {
                self.postMessage({ type: 'sync', frameNum: D.frameNum });
                return;
            }

            // Parse TA
            const g = P.parse(fr.taBuffer, fr.taSize);
            try { P.fillBGP(g, D.pvrRegs, D.vram); } catch (e2) {}

            // Copy vertex data (it's a subarray of parser's internal buffer)
            const vtx = new Uint8Array(g.vertexData.byteLength);
            vtx.set(g.vertexData);

            // Pack poly lists into flat Int32Arrays for zero-copy transfer
            const packPolys = (list) => {
                const arr = new Int32Array(list.length * 7);
                for (let i = 0; i < list.length; i++) {
                    const p = list[i], o = i * 7;
                    arr[o] = p.first; arr[o+1] = p.count;
                    arr[o+2] = p.isp; arr[o+3] = p.tsp;
                    arr[o+4] = p.tcw; arr[o+5] = p.pcw;
                    arr[o+6] = p.tileclip;
                }
                return arr;
            };

            const op = packPolys(g.opaque);
            const pt = packPolys(g.punchThrough);
            const tr = packPolys(g.translucent);

            // Pack render passes
            const passes = g.renderPasses || [{ op_count: g.opaque.length, pt_count: g.punchThrough.length, tr_count: g.translucent.length }];
            const rp = new Int32Array(passes.length * 3);
            for (let i = 0; i < passes.length; i++) {
                rp[i*3] = passes[i].op_count;
                rp[i*3+1] = passes[i].pt_count;
                rp[i*3+2] = passes[i].tr_count;
            }

            // Transfer everything to main thread (zero-copy via transferable)
            self.postMessage({
                type: 'parsed',
                vtx: vtx.buffer,
                vertexCount: g.vertexCount,
                op: op.buffer, opLen: g.opaque.length,
                pt: pt.buffer, ptLen: g.punchThrough.length,
                tr: tr.buffer, trLen: g.translucent.length,
                rp: rp.buffer, rpLen: passes.length,
                snap: fr.pvrSnapshot,
                vramDirty: fr.vramDirty,
                pvrDirty: fr.pvrDirty,
                dirtyPages: fr.dirtyPageList,
                frameNum: fr.frameNum,
                // VRAM + PVR regs needed for texture decode on main thread
                vram: D.vram,       // NOT transferred — worker keeps ownership
                pvrRegs: D.pvrRegs, // NOT transferred — worker keeps ownership
            }, [vtx.buffer, op.buffer, pt.buffer, tr.buffer, rp.buffer]);
        } catch (ex) {
            self.postMessage({ type: 'error', message: ex.message });
        }
    }
};
