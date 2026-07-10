// _ctrl_flip_active.mjs — flip P1C1/P1C2 active bytes + move the incoming char to
// the outgoing char's position: the true INSTANT point swap (run after _ctrl_charswap
// --mode swap, or standalone on any loaded team).
const P1C1 = 0x268340, P1C2 = 0x268E88;
const ws = new WebSocket('ws://127.0.0.1:7211');
let rid = 0; const pend = new Map();
const cmd = o => new Promise((res,rej)=>{ const id='r'+(++rid); pend.set(id,res);
  ws.send(JSON.stringify({...o, reply_id:id})); setTimeout(()=>rej(new Error('timeout')),8000); });
ws.onmessage = ev => { const m=JSON.parse(ev.data); const r=pend.get(m.reply_id); if(r){pend.delete(m.reply_id);r(m);} };
ws.onopen = async () => { try {
  const h1=(await cmd({cmd:'ram_read',offset:P1C1,size:0x40})).data.hex;
  const h2=(await cmd({cmd:'ram_read',offset:P1C2,size:0x40})).data.hex;
  const a1=h1.slice(0,2), a2=h2.slice(0,2);
  const onScreen = a1==='01' ? {off:P1C1,hex:h1,oa:a1,other:P1C2,ohex:h2} : {off:P1C2,hex:h2,oa:a2,other:P1C1,ohex:h1};
  const pos = onScreen.hex.slice(0x34*2,(0x34+8)*2);
  console.log(`[flip] on-screen slot=0x${onScreen.off.toString(16)} -> benching; other slot takes point at pos ${pos}`);
  await cmd({cmd:'ram_write', offset:onScreen.other,      hex:'01'});   // incoming: active
  await cmd({cmd:'ram_write', offset:onScreen.other+0x34, hex:pos});    // ...at the outgoing char's spot
  await cmd({cmd:'ram_write', offset:onScreen.off,        hex:'00'});   // outgoing: benched
  const v1=(await cmd({cmd:'ram_read',offset:P1C1,size:4})).data.hex, v2=(await cmd({cmd:'ram_read',offset:P1C2,size:4})).data.hex;
  console.log(`[verify] P1C1 active=${v1.slice(0,2)} cid=0x${v1.slice(2,4)}  P1C2 active=${v2.slice(0,2)} cid=0x${v2.slice(2,4)}`);
  process.exit(0);
} catch(e){ console.error('[fail]',e.message); process.exit(1); } };
