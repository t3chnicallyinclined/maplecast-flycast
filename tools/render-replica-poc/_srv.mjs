import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
const root = process.argv[2] || process.cwd();
const port = +(process.argv[3] || 8099);
const types = { '.html':'text/html', '.mjs':'text/javascript', '.js':'text/javascript', '.json':'application/json', '.png':'image/png', '.wasm':'application/wasm', '.mcrr':'application/octet-stream', '.bin':'application/octet-stream', '.css':'text/css' };
http.createServer((req, res) => {
    const p = decodeURIComponent(req.url.split('?')[0]);
    const fp = path.join(root, p);
    fs.readFile(fp, (e, d) => {
        if (e) { res.writeHead(404); res.end('nf'); return; }
        res.writeHead(200, { 'Content-Type': types[path.extname(fp)] || 'application/octet-stream', 'Access-Control-Allow-Origin': '*' });
        res.end(d);
    });
}).listen(port, () => console.log('SERVER_UP', root, port));
