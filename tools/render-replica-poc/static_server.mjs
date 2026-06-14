// minimal static file server for the repo root (HUD validation harness support).
import http from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { join, extname, normalize } from 'node:path';
const ROOT = process.argv[2] || process.cwd();
const PORT = +(process.argv[3] || 8099);
const MIME = { '.html':'text/html', '.mjs':'text/javascript', '.js':'text/javascript',
  '.json':'application/json', '.wasm':'application/wasm', '.png':'image/png',
  '.bin':'application/octet-stream', '.mcrr':'application/octet-stream', '.css':'text/css' };
http.createServer(async (req, res) => {
  try {
    const url = decodeURIComponent(req.url.split('?')[0]);
    const p = normalize(join(ROOT, url));
    if (!p.startsWith(normalize(ROOT))) { res.writeHead(403).end(); return; }
    const s = await stat(p).catch(() => null);
    if (!s || s.isDirectory()) { res.writeHead(404).end('not found'); return; }
    const buf = await readFile(p);
    res.writeHead(200, { 'Content-Type': MIME[extname(p).toLowerCase()] || 'application/octet-stream',
                         'Access-Control-Allow-Origin': '*' });
    res.end(buf);
  } catch (e) { res.writeHead(500).end(String(e)); }
}).listen(PORT, '127.0.0.1', () => console.log(`static server: http://127.0.0.1:${PORT} root=${ROOT}`));
