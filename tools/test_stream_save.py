#!/usr/bin/env python3
"""Save MapleCast H.264 stream to file, then play with FFplay/VLC."""
import socket, sys, time

host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 7200
outfile = sys.argv[3] if len(sys.argv) > 3 else "stream.h264"
duration = 5  # seconds

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect((host, port))
print(f"Connected to {host}:{port}, recording {duration}s to {outfile}")

f = open(outfile, "wb")
frames = 0
start = time.time()

try:
    while time.time() - start < duration:
        hdr = b""
        while len(hdr) < 4:
            chunk = s.recv(4 - len(hdr))
            if not chunk: raise ConnectionError()
            hdr += chunk

        size = int.from_bytes(hdr, "little")
        if size > 1_000_000 or size == 0:
            print(f"Bad size: {size}")
            break

        data = b""
        while len(data) < size:
            chunk = s.recv(min(65536, size - len(data)))
            if not chunk: raise ConnectionError()
            data += chunk

        f.write(data)
        frames += 1

except (ConnectionError, ConnectionResetError) as e:
    print(f"Connection lost: {e}")
except KeyboardInterrupt:
    pass

f.close()
s.close()
elapsed = time.time() - start
print(f"Saved {frames} frames in {elapsed:.1f}s to {outfile}")
print(f"Play with: ffplay -f h264 {outfile}")
