// cap_userplay.mjs — dual-socket raw capture: ZCS2/ZCST wire + replica-live
// FRMx, timestamped, for offline frame-by-frame garble analysis (char flip).
// Usage: node cap_userplay.mjs [seconds] [outdir]
// Output: outdir/wire.bin + outdir/replica.bin — length-prefixed records:
//   [u32 len][u8 sock][f64 t_ms][payload]   sock: 0=wire 1=replica
// ROM-DERIVED — never commit.
import WebSocket from 'ws';
import { createWriteStream, mkdirSync } from 'fs';

const secs = parseInt(process.argv[2] || '180', 10);
const dir = process.argv[3] || 'C:/Users/trist/projects/maplecast-flycast/_bwlab/_cap_userplay';
mkdirSync(dir, { recursive: true });
const out = createWriteStream(dir + '/cap.bin');
let n = 0, bytes = 0;
const t0 = Date.now();

function rec(sock, data) {
    const b = Buffer.from(data);
    const hdr = Buffer.alloc(13);
    hdr.writeUInt32LE(b.length, 0);
    hdr.writeUInt8(sock, 4);
    hdr.writeDoubleLE(Date.now() - t0, 5);
    out.write(hdr); out.write(b);
    n++; bytes += b.length;
}

const w1 = new WebSocket('wss://nobd.net/ws');
w1.on('open', () => console.log('wire connected'));
w1.on('message', d => rec(0, d));
w1.on('error', e => console.log('wire err', e.message));

const w2 = new WebSocket('wss://nobd.net/replica-live');
w2.on('open', () => console.log('replica connected'));
w2.on('message', d => rec(1, d));
w2.on('error', e => console.log('replica err', e.message));

const iv = setInterval(() => console.log(`t+${((Date.now()-t0)/1000)|0}s  ${n} msgs  ${(bytes/1048576).toFixed(1)} MB`), 15000);
setTimeout(() => { clearInterval(iv); out.end(() => { console.log(`DONE: ${n} msgs, ${(bytes/1048576).toFixed(1)} MB -> ${dir}/cap.bin`); process.exit(0); }); }, secs * 1000);
