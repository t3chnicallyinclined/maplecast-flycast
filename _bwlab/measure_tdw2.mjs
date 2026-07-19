// Measure the ACK-reference TDW2 wire. Subscribes AND sends ACKs (8-byte "ACKF"
// + highest frameId, every other frame) so the server actually references a recent
// frame -> deltas, not all keyframes. Reports keyframe/delta split + Mbps.
import WebSocket from 'ws';
const ws = new WebSocket('ws://127.0.0.1:7200');
let n = 0, keyN = 0, keyB = 0, delN = 0, delB = 0, relN = 0, fitDg = 0, high = -1;
const MAXDG = 1414;
ws.on('open', () => ws.send(JSON.stringify({ type: 'subscribe', mode: 'tdw2' })));
ws.on('message', (buf) => {
  if (!(buf instanceof Buffer) || buf.length < 20) return;
  if (buf.toString('latin1', 0, 4) !== 'TDW2') return;
  const flags = buf[5];
  const frameId = buf.readUInt32LE(8);
  const refId = buf.readUInt32LE(12);
  const isKey = (refId === 0xFFFFFFFF);
  n++;
  if (flags & 4) relN++;
  if (buf.length <= MAXDG) fitDg++;
  if (isKey) { keyN++; keyB += buf.length; } else { delN++; delB += buf.length; }
  if (frameId > high) high = frameId;
  if (frameId % 2 === 0) {                          // ACK the highest frame we hold
    const m = Buffer.alloc(8); m.write('ACKF', 0, 'latin1'); m.writeUInt32LE(high >>> 0, 4);
    ws.send(m);
  }
  if (n >= 500) {
    console.log(`frames=${n}`);
    console.log(`keyframes=${keyN}  avg=${keyN ? (keyB / keyN | 0) : 0}B`);
    console.log(`deltas=${delN}    avg=${delN ? (delB / delN | 0) : 0}B`);
    console.log(`reliable(bit2)=${relN}/${n}  fit-datagram(<=${MAXDG})=${fitDg}/${n}`);
    console.log(`~${((keyB + delB) * 8 / (n / 60) / 1e6).toFixed(2)} Mbps @60fps`);
    ws.close(); process.exit(0);
  }
});
ws.on('error', (e) => { console.log('ERR', e.message); process.exit(1); });
setTimeout(() => { console.log('timeout, got', n); process.exit(1); }, 150000);
