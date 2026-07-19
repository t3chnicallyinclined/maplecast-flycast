#!/usr/bin/env python3
# noderec_agent.py — per-node dataset recording control agent.
#
# A tiny loopback HTTP service that bridges the node's Caddy (/noderec/*) to the
# node's LOCAL flycast control WS (127.0.0.1:7211) — dataset_record + ram_read.
# It never opens a new public port: Caddy (already terminating TLS for the node)
# reverse-proxies /noderec/* here, key-gated. Pure stdlib (nodes have python3,
# not node). RAM/control never leaves loopback except through Caddy's key gate.
#
#   run:   MC_NODE_KEY=... python3 /opt/maplecast/noderec_agent.py
#   caddy: handle_path /noderec/* { reverse_proxy 127.0.0.1:9098 }
import os, json, socket, base64, struct, time, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

PORT     = int(os.environ.get('MC_NODEREC_PORT', '9098'))
KEY      = os.environ.get('MC_NODE_KEY', '')
CTRL_HOST = '127.0.0.1'
CTRL_PORT = int(os.environ.get('MC_CTRL_PORT', '7211'))
REC_DIR  = os.environ.get('MAPLECAST_RECORDINGS_DIR', '/opt/maplecast/recordings')
INMATCH_ADDR = 0x289624

_recording = {'on': False}          # local intent flag (this agent's last toggle)
_lock = threading.Lock()

# ---- minimal WebSocket client (stdlib) → the control WS ----
def _recv_frame(sock, buf, deadline):
    def need(n):
        while len(buf) < n:
            if time.time() > deadline: raise IOError('ws read timeout')
            d = sock.recv(4096)
            if not d: raise IOError('ws closed')
            buf.extend(d)
    need(2)
    b1 = buf[1]; masked = b1 & 0x80; ln = b1 & 0x7f; off = 2
    if ln == 126:
        need(4); ln = struct.unpack('>H', bytes(buf[2:4]))[0]; off = 4
    elif ln == 127:
        need(10); ln = struct.unpack('>Q', bytes(buf[2:10]))[0]; off = 10
    mk = b''
    if masked:
        need(off + 4); mk = bytes(buf[off:off+4]); off += 4
    need(off + ln)
    opcode = buf[0] & 0x0f
    data = bytes(buf[off:off+ln])
    if masked:
        data = bytes(c ^ mk[i % 4] for i, c in enumerate(data))
    del buf[:off + ln]
    return opcode, data

def ws_ctrl(obj, timeout=6.0):
    """Open a WS to the control server, send one JSON command, return the reply dict."""
    obj = dict(obj); obj.setdefault('reply_id', 'a' + base64.b16encode(os.urandom(4)).decode())
    rid = obj['reply_id']
    s = socket.create_connection((CTRL_HOST, CTRL_PORT), timeout=timeout)
    try:
        s.settimeout(timeout)
        wskey = base64.b64encode(os.urandom(16)).decode()
        s.sendall((f"GET / HTTP/1.1\r\nHost: {CTRL_HOST}:{CTRL_PORT}\r\n"
                   "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                   f"Sec-WebSocket-Key: {wskey}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        buf = bytearray()
        while b'\r\n\r\n' not in buf:
            d = s.recv(4096)
            if not d: raise IOError('ws handshake closed')
            buf += d
        if b' 101' not in bytes(buf).split(b'\r\n', 1)[0]:
            raise IOError('ws handshake failed')
        leftover = bytearray(bytes(buf).split(b'\r\n\r\n', 1)[1])
        # send masked text frame
        payload = json.dumps(obj).encode()
        mask = os.urandom(4)
        hdr = bytearray([0x81])
        n = len(payload)
        if n < 126: hdr.append(0x80 | n)
        elif n < 65536: hdr.append(0x80 | 126); hdr += struct.pack('>H', n)
        else: hdr.append(0x80 | 127); hdr += struct.pack('>Q', n)
        hdr += mask
        s.sendall(bytes(hdr) + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))
        # read text frames until the matching reply (or a plausible reply shape)
        deadline = time.time() + timeout
        for _ in range(50):
            opcode, data = _recv_frame(s, leftover, deadline)
            if opcode == 0x8: raise IOError('ws closed by server')
            if opcode != 0x1: continue
            try: m = json.loads(data.decode('utf-8', 'replace'))
            except Exception: continue
            if m.get('reply_id') == rid or 'ok' in m or 'hex' in m:
                return m
        raise IOError('no reply')
    finally:
        try: s.close()
        except Exception: pass

# ---- .mctele header parse → exact frame count ----
def tele_info(path, size):
    try:
        with open(path, 'rb') as f: b = f.read(20)
        if len(b) < 20 or b[:6] != b'MCTELE': return 0
        blob = struct.unpack('<I', b[12:16])[0]
        nseg = struct.unpack('<I', b[16:20])[0]
        per = 8 + blob; hdr = 44 + 8 * nseg
        return max(0, (size - hdr) // per) if per > 0 else 0
    except Exception: return 0

def latest_mctele():
    try:
        best = None
        for f in os.listdir(REC_DIR):
            if not f.endswith('.mctele'): continue
            st = os.stat(os.path.join(REC_DIR, f))
            if not best or st.st_mtime > best[1]: best = (f, st.st_mtime, st.st_size)
        if best:
            return {'name': best[0], 'size': best[2], 'mtime': best[1],
                    'frames': tele_info(os.path.join(REC_DIR, best[0]), best[2])}
    except Exception: pass
    return None

def _hex_of(m):
    return m.get('hex') or (m.get('data') or {}).get('hex') if isinstance(m.get('data'), dict) else m.get('hex')

def status():
    r = ws_ctrl({'cmd': 'ram_read', 'offset': INMATCH_ADDR, 'size': 1})
    hx = _hex_of(r)
    in_match = bool(hx) and hx != '00'
    cur = latest_mctele()
    writing = bool(cur) and (time.time() - cur['mtime'] < 3)
    with _lock: rec = _recording['on']
    return {'ok': True, 'recording': rec, 'writing': writing, 'inMatch': in_match,
            'mctele': ({'name': cur['name'], 'size': cur['size'], 'frames': cur['frames']} if cur else None)}

def set_record(on):
    m = ws_ctrl({'cmd': 'dataset_record', 'on': bool(on)})
    with _lock: _recording['on'] = bool(on)
    return {'ok': bool(m.get('ok', True)), 'recording': bool(on)}

# ---- HTTP (loopback; Caddy strips the /noderec prefix) ----
class H(BaseHTTPRequestHandler):
    def _auth(self, q):
        return (not KEY) or q.get('key', [''])[0] == KEY or self.headers.get('X-Node-Key') == KEY
    def _send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header('content-type', 'application/json')
        self.send_header('content-length', str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query); p = u.path.rstrip('/') or '/'
        if p in ('/', '/status', '/noderec', '/noderec/status'):
            if not self._auth(q): return self._send(401, {'ok': False, 'error': 'bad key'})
            try: return self._send(200, status())
            except Exception as e: return self._send(502, {'ok': False, 'error': str(e)})
        self._send(404, {'ok': False, 'error': 'not found'})
    def do_POST(self):
        u = urlparse(self.path); q = parse_qs(u.query); p = u.path.rstrip('/') or '/'
        if p in ('/record', '/noderec/record'):
            if not self._auth(q): return self._send(401, {'ok': False, 'error': 'bad key'})
            on = q.get('on', ['0'])[0] in ('1', 'true', 'on')
            try: return self._send(200, set_record(on))
            except Exception as e: return self._send(502, {'ok': False, 'error': str(e)})
        self._send(404, {'ok': False, 'error': 'not found'})
    def log_message(self, *a): pass

if __name__ == '__main__':
    print(f'[noderec] http://127.0.0.1:{PORT} -> control ws://{CTRL_HOST}:{CTRL_PORT}'
          + (' (key-gated)' if KEY else ' (OPEN)'), flush=True)
    ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()
