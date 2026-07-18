// Subscribe to the local TDW server, categorize kfdelta frames: keyframe vs delta,
// reliable vs droppable, and SIZE distribution. Tells us why the split degenerated.
import WebSocket from 'ws';
const ws = new WebSocket('ws://127.0.0.1:7200');
let n = 0, keyN = 0, keyBytes = 0, delN = 0, delBytes = 0;
let relN = 0, fitDgram = 0, kfdeltaN = 0, nonKf = 0;
let delMax = 0, delMin = 1e9;
const MAXDG = 1414;
ws.on('open', () => ws.send(JSON.stringify({ type: 'subscribe', mode: 'tdw' })));
ws.on('message', (buf) => {
  if (!(buf instanceof Buffer) || buf.length < 6) return;
  const magic = buf.toString('latin1', 0, 4);
  if (magic !== 'TDW1') return; // ignore TDWS/SYNC for this tally
  const flags = buf[5];
  const isKey = (flags & 1) !== 0, isRel = (flags & 4) !== 0, isKf = (flags & 0x80) !== 0;
  n++;
  if (isKf) kfdeltaN++; else nonKf++;
  if (isRel) relN++;
  if (buf.length <= MAXDG) fitDgram++;
  if (isKey) { keyN++; keyBytes += buf.length; }
  else { delN++; delBytes += buf.length; delMax = Math.max(delMax, buf.length); delMin = Math.min(delMin, buf.length); }
  if (n >= 400) {
    console.log(`frames=${n}  kfdelta=${kfdeltaN} nonKf=${nonKf}`);
    console.log(`keyframes: ${keyN}  avg=${keyN ? (keyBytes / keyN | 0) : 0}B`);
    console.log(`deltas:    ${delN}  avg=${delN ? (delBytes / delN | 0) : 0}B  min=${delMin}B  max=${delMax}B`);
    console.log(`reliable(bit2)=${relN}/${n}  fit-datagram(<=${MAXDG}B)=${fitDgram}/${n}`);
    const mbps = (keyBytes + delBytes) * 8 / (n / 60) / 1e6;
    console.log(`~${mbps.toFixed(2)} Mbps @60fps`);
    ws.close(); process.exit(0);
  }
});
ws.on('error', (e) => { console.log('ERR', e.message); process.exit(1); });
setTimeout(() => { console.log('timeout, got', n); process.exit(1); }, 20000);
