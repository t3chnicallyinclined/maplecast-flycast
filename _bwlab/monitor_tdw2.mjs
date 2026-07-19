import WebSocket from 'ws';
const ws = new WebSocket('ws://127.0.0.1:7200');
let w = { key:0, keyB:0, del:0, delB:0 }, high=-1, ticks=0;
ws.on('open',()=>ws.send(JSON.stringify({type:'subscribe',mode:'tdw'})));
ws.on('message',(b)=>{
  if(!(b instanceof Buffer)||b.length<20)return;
  if(b.toString('latin1',0,4)!=='TDW2')return;
  const fid=b.readUInt32LE(8), ref=b.readUInt32LE(12);
  if(ref===0xFFFFFFFF){w.key++;w.keyB+=b.length;}else{w.del++;w.delB+=b.length;}
  if(fid>high)high=fid;
  if(fid%2===0){const m=Buffer.alloc(8);m.write('ACKF',0,'latin1');m.writeUInt32LE(high>>>0,4);ws.send(m);}
});
const iv=setInterval(()=>{
  const dAvg=w.del?(w.delB/w.del|0):0;
  console.log(`[${(++ticks*5)}s] TDW2: key=${w.key} delta=${w.del} delta_avg=${dAvg}B ${w.del>50?'<-- MATCH, thin='+((w.delB+w.keyB)*8/((w.del+w.key)/60)/1e6).toFixed(2)+'Mbps':''}`);
  w={key:0,keyB:0,del:0,delB:0};
  if(ticks>=18){clearInterval(iv);ws.close();process.exit(0);}
},5000);
ws.on('error',e=>{console.log('ERR',e.message);process.exit(1);});
