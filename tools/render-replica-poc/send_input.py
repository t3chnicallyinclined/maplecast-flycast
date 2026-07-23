#!/usr/bin/env python3
"""Send input-server-format UDP packets to the executor server: {u16 buttons; u8 lt; u8 rt; u32 seq}.
DC buttons are active-low (0xFFFF = neutral; clear a bit to press). Scripts idle->jump->HP."""
import socket, struct, time, sys
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7100
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
addr = ('127.0.0.1', PORT)
NEUTRAL = 0xFFFF
seq = 0
def send(buttons):
    global seq
    sock.sendto(struct.pack('<HBBI', buttons & 0xFFFF, 0, 0, seq), addr); seq += 1
for i in range(300):                       # ~5s at 60Hz
    if   60 <= i < 120: b = NEUTRAL & ~0x0010   # Up = jump
    elif 150 <= i < 180: b = NEUTRAL & ~0x0200  # Y -> HP
    else:                b = NEUTRAL
    send(b); time.sleep(1/60)
print(f"sent {seq} input packets to :{PORT}")
