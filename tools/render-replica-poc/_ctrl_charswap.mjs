// _ctrl_charswap.mjs — LIVE instant point-character swap experiment (mod-layer PoC).
// Swaps the FULL P1C1 <-> P1C2 char structs (0x5A4 bytes) via the control WS
// ram_read/ram_write, preserving each slot's original world position, with a
// savestate safety net. Node 22 built-in WebSocket; run against an ssh tunnel:
//   ssh -N -L 7211:127.0.0.1:7211 root@149.28.44.118 &
//   node _ctrl_charswap.mjs [--mode read|teleport|swap|restore]
const MODE = (process.argv.includes('--mode') ? process.argv[process.argv.indexOf('--mode')+1] : 'read');
const P1C1 = 0x268340, P1C2 = 0x268E88, P2C1 = 0x2688E4, STRIDE = 0x5A4;
const SAFETY_SLOT = 97;

const ws = new WebSocket('ws://127.0.0.1:7211');
let rid = 0; const pend = new Map();
function cmd(o){ return new Promise((res,rej)=>{ const id='r'+(++rid); pend.set(id,res);
  ws.send(JSON.stringify({...o, reply_id:id})); setTimeout(()=>{ if(pend.has(id)){pend.delete(id);rej(new Error('timeout '+o.cmd));} }, 8000); }); }
ws.onmessage = ev => { try{ const m=JSON.parse(ev.data); const r=pend.get(m.reply_id);
  if(r){ pend.delete(m.reply_id); r(m); } }catch{} };
ws.onerror = e => { console.error('[ctrl] ws error — is the tunnel up?'); process.exit(1); };

const rd = async (off,size)=>{ const m=await cmd({cmd:'ram_read', offset:off, size});
  if(!m.ok) throw new Error('ram_read '+JSON.stringify(m)); return m.data.hex; };
const wr = async (off,hex)=>{ const m=await cmd({cmd:'ram_write', offset:off, hex});
  if(!m.ok) throw new Error('ram_write '+JSON.stringify(m)); return m; };
const f32 = (hex,off)=>{ const b=Buffer.from(hex.slice(off*2,(off+4)*2),'hex'); return b.readFloatLE(0); };

ws.onopen = async () => { try {
  const st = await cmd({cmd:'status'});
  console.log('[status]', JSON.stringify(st.data||st));

  if (MODE==='restore'){ console.log('[restore] loading safety slot', SAFETY_SLOT);
    console.log(JSON.stringify(await cmd({cmd:'savestate_load', slot:SAFETY_SLOT}))); process.exit(0); }

  const c1 = await rd(P1C1, STRIDE), c2 = await rd(P1C2, STRIDE), p2 = await rd(P2C1, 0x40);
  const info = (n,h)=>console.log(`  ${n}: active=${h.slice(0,2)} cid=0x${h.slice(2,4)} x=${f32(h,0x34).toFixed(1)} y=${f32(h,0x38).toFixed(1)} hp=${parseInt(h.slice(0x420*2,0x420*2+2),16)}`);
  console.log('[read] char slots:'); info('P1C1(point)',c1); info('P1C2(bench)',c2); info('P2C1',p2);
  if (MODE==='read') process.exit(0);

  console.log('[safety] savestate_save slot', SAFETY_SLOT, '->',
    JSON.stringify((await cmd({cmd:'savestate_save', slot:SAFETY_SLOT})).data));

  if (MODE==='teleport'){
    // Proof-of-life: swap P1C1 <-> P2C1 world X/Y (8 bytes @ +0x34) — instant side switch.
    const a=c1.slice(0x34*2,(0x34+8)*2), b=p2.slice(0x34*2,(0x34+8)*2);
    await wr(P1C1+0x34, b); await wr(P2C1+0x34, a);
    console.log('[teleport] P1<->P2 positions swapped — watch the stream!');
    process.exit(0);
  }

  if (MODE==='swap'){
    // THE instant character swap: P1C1 contents <-> P1C2 contents, each keeping
    // its slot's original world position (+0x34 x, +0x38 y) so the incoming char
    // appears where the point char stood. Slot ADDRESSES never move, so engine
    // globals/slot-table refs stay valid; in-struct GFX/DAT pointers travel with
    // their character. Known cosmetic hazard: live satellites (capes) anchor to
    // slot addresses and will follow the swapped-in char until they re-derive.
    const posC1 = c1.slice(0x34*2,(0x34+8)*2), posC2 = c2.slice(0x34*2,(0x34+8)*2);
    const swapped1 = c2.slice(0, 0x34*2) + posC1 + c2.slice((0x34+8)*2);
    const swapped2 = c1.slice(0, 0x34*2) + posC2 + c1.slice((0x34+8)*2);
    await wr(P1C1, swapped1); await wr(P1C2, swapped2);
    console.log('[swap] P1C1 <-> P1C2 struct contents swapped (positions preserved).');
    const v1 = await rd(P1C1, 8), v2 = await rd(P1C2, 8);
    console.log(`[verify] P1C1 cid=0x${v1.slice(2,4)}  P1C2 cid=0x${v2.slice(2,4)}`);
    console.log('If the game misbehaves:  node _ctrl_charswap.mjs --mode restore');
    process.exit(0);
  }
} catch(e){ console.error('[fail]', e.message); process.exit(1); } };
