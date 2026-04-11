#!/usr/bin/env python3
"""
Test palette injection into Marvel vs Capcom Fighting Collection.
Writes a bright test palette to P1C1's bank to confirm we found the right address.

Usage:
    python3 tools/inject_palette_test.py <PID> <PVR_BASE_HEX>
"""

import sys
import struct
import os
import time

def read_mem(pid, addr, size):
    with open(f'/proc/{pid}/mem', 'rb') as f:
        f.seek(addr)
        return f.read(size)

def write_mem(pid, addr, data):
    with open(f'/proc/{pid}/mem', 'wb') as f:
        f.seek(addr)
        f.write(data)

def main():
    if len(sys.argv) < 3:
        print(f"Usage: python3 {sys.argv[0]} <PID> <PVR_BASE_HEX>")
        print(f"Example: python3 {sys.argv[0]} 3463151 0x21912C")
        sys.exit(1)

    pid = int(sys.argv[1])
    pvr_base = int(sys.argv[2], 16)

    # PVR palette entry stride: the scan assumed 4 bytes per entry (32-bit PVR regs).
    # Bank N starts at pvr_base + N * 16 * 4 = pvr_base + N * 64

    # First, let's read and display the current P1C1 palette (bank 16)
    bank = 16
    bank_addr = pvr_base + bank * 16 * 4  # 16 colors * 4 bytes per entry
    print(f"[inject] PID={pid}")
    print(f"[inject] PVR base=0x{pvr_base:X}")
    print(f"[inject] P1C1 bank {bank} at 0x{bank_addr:X}")

    current = read_mem(pid, bank_addr, 64)
    print(f"\n[inject] Current P1C1 palette (bank {bank}):")
    for i in range(16):
        val = struct.unpack_from('<I', current, i * 4)[0]
        argb = val & 0xFFFF
        a = (argb >> 12) & 0xF
        r = (argb >> 8) & 0xF
        g = (argb >> 4) & 0xF
        b = argb & 0xF
        print(f"  [{i:2d}] 0x{val:08X} (ARGB4444: {a:X}{r:X}{g:X}{b:X})")

    # Write a test palette — bright red/blue alternating
    # ARGB4444: 0xF00F = opaque blue, 0xFF00 = opaque red, 0xF0F0 = opaque green
    test_colors = [
        0x0000,  # 0: transparent
        0xFF00,  # 1: bright red
        0xF00F,  # 2: bright blue
        0xF0F0,  # 3: bright green
        0xFFFF,  # 4: white
        0xFF00,  # 5: red
        0xFF00,  # 6: red
        0xFF00,  # 7: red
        0xF00F,  # 8: blue
        0xF00F,  # 9: blue
        0xF0F0,  # 10: green
        0xF0F0,  # 11: green
        0xFFFF,  # 12: white
        0xFFFF,  # 13: white
        0xFF80,  # 14: orange
        0xF80F,  # 15: purple
    ]

    print(f"\n[inject] Writing test palette (red/blue/green) to bank {bank}...")

    # Build the write buffer — each palette entry is stored as a 32-bit PVR register
    buf = bytearray(64)
    for i, color in enumerate(test_colors):
        struct.pack_into('<I', buf, i * 4, color)

    write_mem(pid, bank_addr, bytes(buf))
    print(f"[inject] Written 64 bytes to 0x{bank_addr:X}")

    # Verify
    verify = read_mem(pid, bank_addr, 64)
    print(f"\n[inject] Verification read:")
    for i in range(16):
        val = struct.unpack_from('<I', verify, i * 4)[0]
        expected = test_colors[i]
        match = "OK" if (val & 0xFFFF) == expected else f"MISMATCH (expected 0x{expected:04X})"
        print(f"  [{i:2d}] 0x{val:08X} — {match}")

    print(f"\n[inject] Look at the game — P1's character should have crazy colors now!")
    print(f"[inject] If nothing changed, this address isn't PVR palette RAM.")
    print(f"\n[inject] Press Enter to restore original palette...")
    input()
    write_mem(pid, bank_addr, current)
    print("[inject] Original palette restored.")

if __name__ == '__main__':
    main()
