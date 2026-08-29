#!/usr/bin/env python3
"""gsta_serve.py <file.gstarec> [port] — pure-GSTA replay streamer (NO emulator).

Serves a recorded GSTA/PALF/OBJS stream over WebSocket exactly like the live mirror
server, so the existing sprite viewer plays it unchanged. Each connection gets its own
playback from match start at recorded pacing; at end the socket closes (refresh = rewatch).

Memory-lean: one indexing pass stores (file_off, t, op, len) per record; clients stream
records straight off disk. Trim: playback spans 2s before the first GSTA frame with >=2
active fighters through 5s after the last such frame (menu prefix + idle tail dropped).
GSTA payload: 'GSTA'(4)+ver(1)+hdr(20), slots at 25 + i*57, active=+0.
"""
import sys, socket, struct, threading, time, hashlib, base64

path = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8207
# argv[3]: expected team cids "23,17,55,42,44,50" — anchors the play window on frames whose
# slots actually hold this match's characters (char-select/VS frames carry roster GARBAGE
# cids that the sprite client renders as random characters — the "Amingo and SonSon" bug).
TEAM = set(int(x) for x in sys.argv[3].split(",")) if len(sys.argv) > 3 else None
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MIN_GSTA = 29 + 6 * 57  # 'GSTA'(4) + 25-byte header + 6*57 slots (per sprite-client.mjs decoder: B=4, slots at B+25)

# ── index + trim (single pass, header peek only) ──────────────────────
index = []                     # (off_payload, t, op, ln, fc)
first_fight = last_fight = prev_hp = None
cur_fc = 0                     # latest game frame counter; non-GSTA records inherit it
with open(path, "rb") as f:
    assert f.read(8) == b"GSTAREC1"
    off = 8
    while True:
        hdr = f.read(13)
        if len(hdr) < 13:
            break
        t, op, ln = struct.unpack("<dBI", hdr)
        peek = f.read(min(ln, 36 + 6 * 57))
        f.seek(ln - len(peek), 1)
        if len(peek) >= MIN_GSTA and peek[:4] == b"GSTA":
            # Game frame counter (u32 LE at payload+25; sprite-client reads B+21 with B=4).
            # Playback paces on THIS, not recorded arrival times — the recorder stamps
            # network arrival, and replaying arrival jitter replays the stutter.
            cur_fc = struct.unpack_from("<I", peek, 25)[0]
            if TEAM is not None:
                # Fight boundary = HEALTH MOVEMENT while the full match roster is loaded.
                # cids alone span char-select..idle (autoselect writes them early, they
                # persist after K.O.); hp only *changes* during actual fighting.
                hits = sum(1 for i in range(6) if peek[29 + i * 57 + 1] in TEAM)
                if hits == 6:
                    hp = bytes(peek[29 + i * 57 + 3] for i in range(6))
                    if prev_hp is not None and hp != prev_hp:
                        if first_fight is None:
                            first_fight = t
                        last_fight = t
                    prev_hp = hp
        index.append((off + 13, t, op, ln, cur_fc))
        off += 13 + ln

if first_fight is not None:
    t_lo, t_hi = first_fight - 3.0, last_fight + 5.0
    index = [r for r in index if t_lo <= r[1] <= t_hi]
print(f"[gsta-serve] {len(index)} records in play window "
      f"({(index[-1][1]-index[0][1]):.1f}s) from {path}", flush=True)

# ── ws plumbing ───────────────────────────────────────────────────────
def frame_hdr(op, ln):
    if ln < 126:
        return bytes([0x80 | op, ln])
    if ln < 65536:
        return bytes([0x80 | op, 126]) + struct.pack(">H", ln)
    return bytes([0x80 | op, 127]) + struct.pack(">Q", ln)

def client(conn, addr):
    fh = None
    try:
        conn.settimeout(10)
        req = b""
        while b"\r\n\r\n" not in req:
            chunk = conn.recv(4096)
            if not chunk:
                return
            req += chunk
        key = None
        for line in req.split(b"\r\n"):
            if line.lower().startswith(b"sec-websocket-key:"):
                key = line.split(b":", 1)[1].strip().decode()
        if not key:
            return
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        conn.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                      "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n").encode())
        conn.settimeout(30)
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print(f"[gsta-serve] viewer {addr} — playback start", flush=True)
        fh = open(path, "rb")
        # GAME-CLOCK pacing: virtual time advances by the GSTA frame counter delta
        # (clamped — heals menu jumps/wraps), 1/60s per game frame. Recorded wall-clock
        # deltas replay network-arrival jitter (v2 bug); the game clock is metronome-even.
        start = time.monotonic()
        vt = 0.0
        prev_fc = index[0][4]
        for foff, t, op, ln, fc in index:
            step = (fc - prev_fc) / 60.0
            vt += min(max(step, 0.0), 0.1)
            prev_fc = fc
            ahead = (start + vt) - time.monotonic()
            if ahead > 0.001:
                time.sleep(ahead)
            fh.seek(foff)
            conn.sendall(frame_hdr(op, ln) + fh.read(ln))
        time.sleep(5)
        conn.sendall(frame_hdr(8, 2) + struct.pack(">H", 1000))
    except OSError:
        pass
    finally:
        if fh:
            fh.close()
        try: conn.close()
        except OSError: pass
        print(f"[gsta-serve] viewer {addr} done", flush=True)

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(8)
print(f"[gsta-serve] listening on 127.0.0.1:{port}", flush=True)
while True:
    c, a = srv.accept()
    threading.Thread(target=client, args=(c, a), daemon=True).start()
