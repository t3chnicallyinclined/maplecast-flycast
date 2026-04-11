#!/usr/bin/env python3
"""
Try writing bright red palette to multiple candidate PVR addresses.
Writes for 3 seconds each, announces which one it's testing.

Usage:
    python3 tools/inject_test_multi.py <PID>
"""
import sys, struct, time

def read_mem(pid, addr, size):
    try:
        with open(f'/proc/{pid}/mem', 'rb') as f:
            f.seek(addr)
            return f.read(size)
    except:
        return None

def write_mem(pid, addr, data):
    with open(f'/proc/{pid}/mem', 'wb') as f:
        f.seek(addr)
        f.write(data)

def make_red_palette(stride):
    """All-red palette, 16 entries."""
    buf = bytearray(16 * stride)
    colors = [0x0000] + [0xFF00] * 15  # transparent + 15 red
    for i, c in enumerate(colors):
        if stride == 2:
            struct.pack_into('<H', buf, i * 2, c)
        else:
            struct.pack_into('<I', buf, i * 4, c)
    return bytes(buf)

pid = int(sys.argv[1])

# Candidates to test — (address, stride, description)
# For each, we test writing to where bank 16 (P1C1) would be
candidates = [
    # 2-byte stride candidates
    (0xAAB636, 2, "0xAAB636 2-byte (real-looking ARGB4444 data)"),
    (0x8B0E6C, 2, "0x8B0E6C 2-byte (sequential-looking data)"),
    # Try the 0xAAB636 region but as if it IS bank 0 (not offset to bank 16)
    # Maybe the data at 0xAAB636 is already at bank 16's offset
    (0xAAB636 - 512, 2, "0xAAB436 2-byte (0xAAB636 as bank-16 start, base -512)"),
    # Also try writing directly at the address where we see real palette data
    # The data at 0xAAB636 showed F6D0, FA9C, 6FFF patterns — write right there
]

for addr, stride, desc in candidates:
    bank16_addr = addr + 16 * 16 * stride
    pal_size = 16 * stride

    print(f"\n=== Testing: {desc} ===")
    print(f"    Writing RED to bank 16 at 0x{bank16_addr:X} ({stride}-byte stride)")

    orig = read_mem(pid, bank16_addr, pal_size)
    if orig is None:
        print(f"    FAILED to read — skipping")
        continue

    # Show what's there now
    preview = []
    for i in range(min(4, 16)):
        if stride == 2:
            v = struct.unpack_from('<H', orig, i*2)[0]
        else:
            v = struct.unpack_from('<I', orig, i*4)[0] & 0xFFFF
        preview.append(f'{v:04X}')
    print(f"    Current first 4: {' '.join(preview)}")

    red = make_red_palette(stride)
    write_mem(pid, bank16_addr, red)
    print(f"    >>> WROTE RED — look at the game NOW! (3 seconds)")

    # Also try writing directly to the candidate address itself (maybe it IS bank 16)
    orig_direct = read_mem(pid, addr, pal_size)
    write_mem(pid, addr, red)
    print(f"    >>> Also wrote RED directly at 0x{addr:X}")

    time.sleep(3)

    # Restore both
    write_mem(pid, bank16_addr, orig)
    if orig_direct:
        write_mem(pid, addr, orig_direct)
    print(f"    Restored.")

# Also try: scan the WINE/Proton process for the emulator's own palette array
# Digital Eclipse might use RGBA8888 (4 bytes, different format) or a GPU texture
print(f"\n=== Bonus: scanning for RGBA8888 palette (4-byte, full color) ===")
# If the emulator converts ARGB4444 to RGBA8888 for the GPU, Sentinel's
# default colors (purple/silver) in RGBA8888 would be distinctive.
# Sentinel default P1: purple body = roughly (128, 0, 192, 255) = 0x8000C0FF in RGBA
# But we don't know the exact values. Skip this for now.
print("(skipped — need known RGBA8888 reference values)")
