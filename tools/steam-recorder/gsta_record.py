#!/usr/bin/env python3
"""gsta_record.py <out.gstarec> [host:port] — record the mirror server's GSTA/PALF/OBJS
WebSocket stream to a file. Stdlib-only raw WS client (server->client frames are unmasked).

File format: magic "GSTAREC1", then per record: [f64 t_monotonic][u8 opcode][u32 len][payload].
Connecting counts as a viewer (emission is gated on clientCount>0), so this can be the ONLY
client and the stream still flows. Runs until the socket closes (emulator exit) or 20 min.
"""
import sys, socket, base64, os, struct, time

out = sys.argv[1]
host, port = (sys.argv[2].split(":") + ["8200"])[:2] if len(sys.argv) > 2 else ("127.0.0.1", "8200")
port = int(port)

s = socket.create_connection((host, port), timeout=15)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall((f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
           f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
resp = b""
while b"\r\n\r\n" not in resp:
    chunk = s.recv(4096)
    if not chunk:
        sys.exit("handshake: connection closed")
    resp += chunk
head, _, rest = resp.partition(b"\r\n\r\n")
assert b" 101 " in head.split(b"\r\n")[0], head[:200]
buf = bytearray(rest)

def need(n):
    while len(buf) < n:
        chunk = s.recv(65536)
        if not chunk:
            raise ConnectionError("closed")
        buf.extend(chunk)

def send_pong(payload):
    mask = os.urandom(4)
    body = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    ln = len(payload)
    if ln < 126:
        hdr = bytes([0x8A, 0x80 | ln])
    else:
        hdr = bytes([0x8A, 0x80 | 126]) + struct.pack(">H", ln)
    s.sendall(hdr + mask + body)

f = open(out, "wb")
f.write(b"GSTAREC1")
t_end = time.monotonic() + 1200
nrec = 0
try:
    while time.monotonic() < t_end:
        need(2)
        b1, b2 = buf[0], buf[1]
        opcode = b1 & 0x0F
        ln = b2 & 0x7F
        off = 2
        if ln == 126:
            need(4); ln = struct.unpack(">H", bytes(buf[2:4]))[0]; off = 4
        elif ln == 127:
            need(10); ln = struct.unpack(">Q", bytes(buf[2:10]))[0]; off = 10
        if b2 & 0x80:  # masked server frame (nonstandard) — skip mask key
            need(off + 4 + ln)
            mask = bytes(buf[off:off + 4])
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(buf[off + 4:off + 4 + ln]))
            del buf[:off + 4 + ln]
        else:
            need(off + ln)
            payload = bytes(buf[off:off + ln])
            del buf[:off + ln]
        if opcode == 8:
            break
        if opcode == 9:
            send_pong(payload); continue
        if opcode in (1, 2) and ln:
            f.write(struct.pack("<dBI", time.monotonic(), opcode, len(payload)))
            f.write(payload)
            nrec += 1
except (ConnectionError, socket.timeout):
    pass
f.close()
print(f"recorded {nrec} frames -> {out} ({os.path.getsize(out)} bytes)")
