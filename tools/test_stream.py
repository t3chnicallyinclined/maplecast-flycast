#!/usr/bin/env python3
"""Test MapleCast H.264 stream receiver."""
import socket, time, sys

host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 7200

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect((host, port))
print(f"Connected to {host}:{port}")

frames = 0
start = time.time()
try:
    while True:
        # Read 4-byte length header
        hdr = b""
        while len(hdr) < 4:
            chunk = s.recv(4 - len(hdr))
            if not chunk:
                raise ConnectionError("disconnected")
            hdr += chunk

        size = int.from_bytes(hdr, "little")
        if size > 1_000_000 or size == 0:
            print(f"Bad frame size: {size} — protocol error?")
            break

        # Read frame data
        data = b""
        while len(data) < size:
            chunk = s.recv(min(65536, size - len(data)))
            if not chunk:
                raise ConnectionError("disconnected")
            data += chunk

        frames += 1
        elapsed = time.time() - start
        if frames % 30 == 0:
            fps = frames / elapsed
            print(f"Frame {frames}: {size:,} bytes | {fps:.1f} fps")

except (ConnectionError, ConnectionResetError) as e:
    print(f"Connection lost: {e}")
except KeyboardInterrupt:
    pass

elapsed = time.time() - start
if frames > 0:
    print(f"\nTotal: {frames} frames in {elapsed:.1f}s = {frames/elapsed:.1f} fps")
else:
    print("No frames received")
s.close()
