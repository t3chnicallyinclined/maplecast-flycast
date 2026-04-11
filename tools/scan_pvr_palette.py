#!/usr/bin/env python3
"""
Scan a running process's memory for MVC2 PVR palette RAM.

We know the PVR palette layout: 1024 entries x 4 bytes = 4096 bytes.
Banks are at offsets: P1C1=bank16 (offset 0x80), P2C1=bank24 (offset 0xC0), etc.

Strategy: search for known default palette signatures. MVC2's default palettes
for common characters are well-known ARGB4444 values. We scan for a 32-byte
(16-color) palette we know, then verify surrounding banks match expected layout.

Usage:
    sudo python3 tools/scan_pvr_palette.py <PID>
"""

import sys
import struct
import os
import re

def read_maps(pid):
    """Parse /proc/<pid>/maps, return list of (start, end, perms, path)."""
    regions = []
    with open(f'/proc/{pid}/maps', 'r') as f:
        for line in f:
            m = re.match(r'([0-9a-f]+)-([0-9a-f]+)\s+(\S+)\s+\S+\s+\S+\s+\S+\s*(.*)', line)
            if m:
                start = int(m.group(1), 16)
                end = int(m.group(2), 16)
                perms = m.group(3)
                path = m.group(4).strip()
                regions.append((start, end, perms, path))
    return regions

def read_mem(pid, addr, size):
    """Read `size` bytes from process `pid` at virtual address `addr`."""
    try:
        with open(f'/proc/{pid}/mem', 'rb') as f:
            f.seek(addr)
            return f.read(size)
    except (OSError, ValueError):
        return None

def scan_for_pattern(pid, pattern, regions):
    """Scan readable regions for `pattern`, return list of addresses."""
    hits = []
    for start, end, perms, path in regions:
        if 'r' not in perms:
            continue
        size = end - start
        # Skip very large mappings (GPU VRAM, etc) to keep scan fast
        if size > 512 * 1024 * 1024:
            continue
        # Skip non-interesting mappings
        if '[vvar]' in path or '[vsyscall]' in path:
            continue

        # Read in chunks
        chunk_size = 4 * 1024 * 1024  # 4MB chunks
        offset = 0
        while offset < size:
            read_size = min(chunk_size, size - offset)
            data = read_mem(pid, start + offset, read_size)
            if data is None:
                break
            # Search for pattern
            idx = 0
            while True:
                idx = data.find(pattern, idx)
                if idx == -1:
                    break
                addr = start + offset + idx
                hits.append((addr, path))
                idx += 1
            offset += read_size - len(pattern) + 1  # overlap for boundary matches
    return hits


# MVC2 default Ryu palette (P1 side, color 1) — first 16 ARGB4444 entries (32 bytes)
# These are the stock palette values from the Naomi/DC ROM.
# We'll try multiple approaches.

# Approach 1: scan for the PVR palette RAM structure itself.
# PVR palette RAM is 1024 x 32-bit entries = 4096 bytes at register offset 0x1000.
# On the DC/Naomi, palette entries are written via pvr_WriteReg at offsets 0x1000-0x1FFC.
# In an emulator, this is typically a contiguous 4KB array.

# Approach 2: scan for known DC RAM patterns.
# The character struct at 0x8C268340 has known offsets. If the emulator maps
# DC RAM contiguously, we can find it by searching for known byte patterns
# at known relative offsets.

# Let's scan for the DC RAM base by looking for the MVC2 ings:
# At 0x8C289624 there's an in_match flag (should be non-zero during match)
# At 0x8C289621 there's match_sub_state
# At 0x8C28962B there's round_counter
# At 0x8C289638 there's stage_id (0-15 typically)
#
# More reliable: character struct headers. P1C1 at 0x268340 relative to DC RAM base.
# active(u8) + character_id(u8) at +0x000 and +0x001.

def main():
    if len(sys.argv) < 2:
        print(f"Usage: sudo {sys.argv[0]} <PID>")
        sys.exit(1)

    pid = int(sys.argv[1])
    print(f"[scan] Target PID: {pid}")
    print(f"[scan] Reading memory maps...")

    regions = read_maps(pid)
    rw_regions = [(s, e, p, path) for s, e, p, path in regions if 'rw' in p]
    print(f"[scan] {len(regions)} total regions, {len(rw_regions)} read-write")

    # Total RW memory
    total_rw = sum(e - s for s, e, _, _ in rw_regions)
    print(f"[scan] Total RW memory: {total_rw / 1024 / 1024:.1f} MB")

    # DC has 16MB main RAM. The character structs are at known offsets from 0x8C000000.
    # In the emulator's address space, DC RAM is a contiguous 16MB block.
    # We'll search for the game timer pattern at offset 0x289630 (relative to DC RAM base 0x8C000000).
    # DC RAM offset = address - 0x8C000000, so 0x289630 is at offset 0x289630 within the 16MB block.

    # Strategy: find candidate 16MB-aligned blocks, read character struct offsets, validate.
    # Or: brute force scan for a known multi-byte signature at a known relative offset.

    # Let's look for the character ID bytes. In a match, P1C1.active should be 1 and
    # P1C1.character_id should be a valid char (0-58). P2C1 is at +0x5A4 from P1C1.
    # If both are active with valid IDs, we likely found DC RAM.

    P1C1_OFF = 0x268340  # relative to DC RAM base (0x8C000000 - 0x8C000000 = 0)
    P2C1_OFF = 0x2688E4
    MATCH_FLAG_OFF = 0x289624
    FRAME_CTR_OFF = 0x3496B0  # frame counter

    print(f"\n[scan] === Phase 1: Searching for DC RAM base ===")
    print(f"[scan] Looking for active character structs at known offsets...")

    candidates = []
    for start, end, perms, path in rw_regions:
        size = end - start
        # DC RAM is 16MB, so the region must be at least that big
        if size < 16 * 1024 * 1024:
            continue
        # Try every 4KB-aligned offset within this region where DC RAM could start
        # But that's too many. Instead, try the region start itself and a few alignments.
        # Most emulators mmap a single 16MB block for DC RAM.
        # Check if the character struct offsets land within this region.
        for base_off in range(0, min(size - 16*1024*1024, 256*1024*1024), 4096):
            base = start + base_off
            if base + FRAME_CTR_OFF + 4 > end:
                break

            # Quick check: read P1C1 active + char_id
            data = read_mem(pid, base + P1C1_OFF, 2)
            if data is None:
                continue
            p1_active, p1_char = data[0], data[1]
            if p1_active != 1 or p1_char > 58:
                continue

            # Check P2C1
            data = read_mem(pid, base + P2C1_OFF, 2)
            if data is None:
                continue
            p2_active, p2_char = data[0], data[1]
            if p2_active != 1 or p2_char > 58:
                continue

            # Check match flag
            data = read_mem(pid, base + MATCH_FLAG_OFF, 1)
            if data is None:
                continue
            in_match = data[0]

            # Check frame counter (should be non-zero and changing)
            data = read_mem(pid, base + FRAME_CTR_OFF, 4)
            if data is None:
                continue
            frame_ctr = struct.unpack('<I', data)[0]

            char_names = [
                "Ryu","Zangief","Guile","Morrigan","Anakaris","Strider",
                "Cyclops","Wolverine","Psylocke","Iceman","Rogue","Captain America",
                "Spider-Man","Hulk","Venom","Doctor Doom","Tron Bonne","Jill",
                "Hayato","Ruby Heart","SonSon","Amingo","Marrow","Cable",
                "Abyss1","Abyss2","Abyss3","Chun-Li","Mega Man","Roll",
                "Akuma","BB Hood","Felicia","Charlie","Sakura","Dan",
                "Cammy","Dhalsim","M.Bison","Ken","Gambit","Juggernaut",
                "Storm","Sabretooth","Magneto","Shuma-Gorath","War Machine",
                "Silver Samurai","Omega Red","Spiral","Colossus","Iron Man",
                "Sentinel","Blackheart","Thanos","Jin","Captain Commando",
                "Bone Wolverine","Servbot"
            ]

            p1_name = char_names[p1_char] if p1_char < len(char_names) else f"?{p1_char}"
            p2_name = char_names[p2_char] if p2_char < len(char_names) else f"?{p2_char}"

            print(f"\n  *** CANDIDATE DC RAM BASE: 0x{base:016X} ***")
            print(f"      Region: 0x{start:016X}-0x{end:016X} ({size//1024//1024}MB) {path}")
            print(f"      P1C1: active={p1_active} char={p1_name}({p1_char})")
            print(f"      P2C1: active={p2_active} char={p2_name}({p2_char})")
            print(f"      in_match={in_match} frame_ctr={frame_ctr}")

            # Read all 6 character slots
            bases = [0x268340, 0x2688E4, 0x268E88, 0x26942C, 0x2699D0, 0x269F74]
            slot_names = ["P1C1","P2C1","P1C2","P2C2","P1C3","P2C3"]
            print(f"      --- All slots ---")
            for i, (off, sn) in enumerate(zip(bases, slot_names)):
                d = read_mem(pid, base + off, 0x530)
                if d:
                    active = d[0]
                    cid = d[1]
                    health = d[0x420]
                    palette = d[0x52D]
                    cn = char_names[cid] if cid < len(char_names) else f"?{cid}"
                    print(f"        {sn}: active={active} char={cn}({cid}) health={health} palette={palette}")

            candidates.append(base)

            # Don't keep scanning every 4KB, jump ahead
            break

    if not candidates:
        print("\n[scan] No DC RAM base found via character struct scan.")
        print("[scan] The emulator may use a different memory layout.")
        sys.exit(1)

    dc_base = candidates[0]
    print(f"\n[scan] === Phase 2: Searching for PVR palette RAM ===")

    # In Flycast, PVR palette RAM is in the VRAM mapping.
    # PVR registers start at 0xA05F8000 (SH4 P2 area).
    # Palette RAM is at PVR register offset 0x1000 (so 0xA05F9000).
    # But in the emulator, it's likely a separate array.
    #
    # Flycast stores palette RAM in pvr_regs[] or a dedicated array.
    # Let's search for it by looking at known default palette values.
    #
    # Alternative: look for the VRAM block. DC VRAM is 8MB starting at 0xA4000000.
    # In the emulator this is another contiguous allocation.
    # PVR palette RAM is NOT in VRAM — it's in the PVR register space.
    #
    # Let's try: scan all RW memory for a 4KB block where entries match
    # what we'd expect for PVR palette RAM during a match.
    # During a match, banks 16-56 should have non-zero palette data,
    # while bank 0 might be empty or have UI palettes.

    # Actually, let's just read the palette byte from the character struct
    # and search for that palette's data in memory. We know the palette index
    # and can cross-reference with our SurrealDB skin data.

    # Simpler approach: we know palette RAM is 4096 bytes (1024 x 4B).
    # Banks 16, 24, 32, 40, 48, 56 should have data during a match.
    # Bank N starts at offset N*16*4 = N*64 bytes within palette RAM.
    # So bank 16 starts at byte 1024 (0x400).
    # Let's search for 4KB blocks where:
    #   - Bytes 0x400-0x41F (bank 16, 16 entries) are non-zero
    #   - Bytes 0x600-0x61F (bank 24) are non-zero
    #   - Entry 0 of each bank (transparent) has alpha=0 (top nibble of high byte = 0)

    print(f"[scan] Scanning {total_rw / 1024 / 1024:.1f} MB of RW memory for PVR palette patterns...")

    pvr_hits = []
    for start, end, perms, path in rw_regions:
        if 'r' not in perms:
            continue
        size = end - start
        if size > 512 * 1024 * 1024:
            continue
        if size < 4096:
            continue

        # Read entire region
        data = read_mem(pid, start, size)
        if data is None or len(data) < 4096:
            continue

        # Scan for palette RAM signature
        # PVR palette: 1024 entries x 4 bytes = 4096 bytes
        # Bank 16 at offset 0x400, bank 24 at offset 0x600
        for off in range(0, len(data) - 4096, 4):
            # Quick check: bank 16 first entry (P1C1 palette color 0 = transparent)
            # ARGB4444: transparent = 0x0000
            b16_0 = struct.unpack_from('<H', data, off + 0x400)[0]
            # Color 0 should be transparent (0x0000) or have alpha=0
            if (b16_0 >> 12) != 0:
                continue

            # Bank 16 should have some non-zero colors after index 0
            b16_nonzero = False
            for i in range(1, 16):
                val = struct.unpack_from('<H', data, off + 0x400 + i*4)[0]
                if val != 0:
                    b16_nonzero = True
                    break
            if not b16_nonzero:
                continue

            # Same check for bank 24 (P2C1)
            b24_0 = struct.unpack_from('<H', data, off + 0x600)[0]
            if (b24_0 >> 12) != 0:
                continue
            b24_nonzero = False
            for i in range(1, 16):
                val = struct.unpack_from('<H', data, off + 0x600 + i*4)[0]
                if val != 0:
                    b24_nonzero = True
                    break
            if not b24_nonzero:
                continue

            # Promising! Read all banks and validate
            addr = start + off
            print(f"\n  *** CANDIDATE PVR PALETTE RAM: 0x{addr:016X} ***")
            print(f"      Region: {path}")

            bank_offsets = {
                'P1C1': 16, 'P2C1': 24, 'P1C2': 32,
                'P2C2': 40, 'P1C3': 48, 'P2C3': 56
            }
            valid_banks = 0
            for name, bank in bank_offsets.items():
                bank_start = off + bank * 16 * 4  # Each entry is 4 bytes (32-bit PVR reg)
                colors = []
                for i in range(16):
                    # PVR palette entries are 32-bit but only low 16 bits matter (ARGB4444)
                    val = struct.unpack_from('<I', data, bank_start + i*4)[0]
                    colors.append(val & 0xFFFF)
                nonzero = sum(1 for c in colors[1:] if c != 0)
                hex_str = ' '.join(f'{c:04X}' for c in colors)
                print(f"      {name} (bank {bank:2d}): {nonzero}/15 nonzero | {hex_str}")
                if nonzero > 0:
                    valid_banks += 1

            if valid_banks >= 2:
                pvr_hits.append(addr)
                print(f"      ** {valid_banks}/6 banks have data — LIKELY PVR PALETTE RAM **")

            # Don't flood output
            if len(pvr_hits) > 10:
                break

        if len(pvr_hits) > 10:
            break

    print(f"\n[scan] === Summary ===")
    print(f"[scan] DC RAM base candidates: {len(candidates)}")
    for c in candidates:
        print(f"  0x{c:016X}")
    print(f"[scan] PVR palette RAM candidates: {len(pvr_hits)}")
    for p in pvr_hits:
        print(f"  0x{p:016X}")

    if candidates and pvr_hits:
        print(f"\n[scan] Ready to inject! Use these addresses to write ARGB4444 palettes.")
        print(f"[scan] Example: write 16 colors (32 bytes) to PVR_BASE + bank*64")

if __name__ == '__main__':
    main()
