#!/usr/bin/env python3
"""
Cheat Engine-style palette finder.

Step 1: Snapshot all memory, find all 32-byte blocks that look like ARGB4444 palettes
Step 2: User changes character color in-game
Step 3: Re-read ONLY those candidate addresses, find which ones changed

This is much faster than a full diff because we pre-filter to palette-shaped data.

Usage:
    python3 tools/palette_diff.py <PID>
"""
import sys, struct, re, time, os

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

def read_maps(pid):
    regions = []
    with open(f'/proc/{pid}/maps', 'r') as f:
        for line in f:
            m = re.match(r'([0-9a-f]+)-([0-9a-f]+)\s+(\S+)\s+\S+\s+\S+\s+\S+\s*(.*)', line)
            if m:
                regions.append((int(m.group(1),16), int(m.group(2),16), m.group(3), m.group(4).strip()))
    return regions

def looks_like_palette(data, offset):
    """Check if 32 bytes at offset look like a 16-color ARGB4444 palette."""
    colors = [struct.unpack_from('<H', data, offset + i*2)[0] for i in range(16)]

    # Color 0 should be transparent or near-transparent
    if (colors[0] >> 12) > 1:  # alpha > 1
        return False

    # At least 6 colors should have alpha >= 0xE (opaque)
    opaque = sum(1 for c in colors[1:] if c != 0 and (c >> 12) >= 0xE)
    if opaque < 6:
        return False

    # Should have some color variety (not all the same)
    unique = len(set(colors[1:]))
    if unique < 3:
        return False

    return True

pid = int(sys.argv[1])
print(f"[palette-diff] PID: {pid}")

regions = read_maps(pid)
rw = [(s,e,p,path) for s,e,p,path in regions if 'rw' in p]

# Phase 1: find all palette-looking blocks
print(f"\n=== Phase 1: Finding palette-shaped data ===")
candidates = []  # (addr, data_32bytes)
total_scanned = 0

for start, end, perms, path in rw:
    size = end - start
    if size > 256 * 1024 * 1024 or size < 64:
        continue
    data = read_mem(pid, start, size)
    if not data or len(data) < 32:
        continue
    total_scanned += len(data)

    # Scan at 2-byte alignment (ARGB4444 is 16-bit)
    for off in range(0, len(data) - 32, 2):
        if looks_like_palette(data, off):
            addr = start + off
            chunk = data[off:off+32]
            candidates.append((addr, chunk))

    # Progress
    if total_scanned % (100*1024*1024) < size:
        print(f"  Scanned {total_scanned/1024/1024:.0f} MB, found {len(candidates)} candidates so far...")

print(f"\n  Total: {len(candidates)} palette-shaped blocks in {total_scanned/1024/1024:.0f} MB")

if not candidates:
    print("No palette candidates found!")
    sys.exit(1)

# Show a few examples
print(f"\n  First 5 candidates:")
for addr, chunk in candidates[:5]:
    colors = [struct.unpack_from('<H', chunk, i*2)[0] for i in range(16)]
    print(f"    0x{addr:016X}: {' '.join(f'{c:04X}' for c in colors)}")

print(f"\n{'='*60}")
print(f"  NOW CHANGE SENTINEL'S COLOR IN THE GAME!")
print(f"  Go to character select, pick Sentinel with a DIFFERENT button")
print(f"  (e.g., LK instead of LP)")
print(f"{'='*60}")
print(f"\n  Waiting 20 seconds...")
time.sleep(20)

# Phase 2: re-read only candidate addresses
print(f"\n=== Phase 2: Re-reading {len(candidates)} candidates ===")
changed = []
unchanged = 0

for addr, old_chunk in candidates:
    new_chunk = read_mem(pid, addr, 32)
    if new_chunk is None:
        continue
    if new_chunk != old_chunk:
        new_colors = [struct.unpack_from('<H', new_chunk, i*2)[0] for i in range(16)]
        old_colors = [struct.unpack_from('<H', old_chunk, i*2)[0] for i in range(16)]
        # Check the new data also looks like a palette (not random noise)
        n_opaque_new = sum(1 for c in new_colors[1:] if c != 0 and (c >> 12) >= 0xE)
        n_changed = sum(1 for a, b in zip(old_colors, new_colors) if a != b)
        changed.append((addr, old_colors, new_colors, n_changed, n_opaque_new))
    else:
        unchanged += 1

print(f"\n  {len(changed)} changed, {unchanged} unchanged")

if not changed:
    print("\n  No palette candidates changed! The emulator might not store")
    print("  live palettes as ARGB4444 in main memory (could be GPU-side).")
    sys.exit(0)

# Sort by number of colors changed (most = most likely the real palette)
changed.sort(key=lambda x: -x[3])

print(f"\n=== RESULTS — changed palette candidates ===")
for addr, old_c, new_c, n_changed, n_opaque in changed[:30]:
    print(f"\n  0x{addr:016X} ({n_changed}/16 changed, {n_opaque} opaque in new):")
    print(f"    old: {' '.join(f'{c:04X}' for c in old_c)}")
    print(f"    new: {' '.join(f'{c:04X}' for c in new_c)}")

# If we found good candidates, try a write test on the top one
if changed and changed[0][3] >= 4:
    best_addr = changed[0][0]
    print(f"\n=== Write test on best candidate 0x{best_addr:016X} ===")
    orig = read_mem(pid, best_addr, 32)
    red = bytearray(32)
    struct.pack_into('<H', red, 0, 0x0000)  # transparent
    for i in range(1, 16):
        struct.pack_into('<H', red, i*2, 0xFF00)  # red
    write_mem(pid, best_addr, bytes(red))
    print(f"  >>> WROTE RED — look at the game! (5 seconds)")
    time.sleep(5)
    write_mem(pid, best_addr, orig)
    print(f"  Restored.")
