#!/usr/bin/env python3
"""
Deep scan of Marvel Collections process memory.

Phase 1: Find DC RAM by scanning for character struct patterns.
Phase 2: Once we have DC RAM base, read the palette byte from char structs
         and search for the actual palette data in VRAM/PVR regs.
Phase 3: Search for PVR palette RAM by looking at all plausible layouts:
         - 2 bytes per entry (raw ARGB4444, 16 entries = 32 bytes per palette)
         - 4 bytes per entry (32-bit PVR regs, 16 entries = 64 bytes per palette)

Usage:
    python3 tools/scan_dcram_deep.py <PID>
"""

import sys
import struct
import os
import re
import time

CHAR_NAMES = [
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

def read_maps(pid):
    regions = []
    with open(f'/proc/{pid}/maps', 'r') as f:
        for line in f:
            m = re.match(r'([0-9a-f]+)-([0-9a-f]+)\s+(\S+)\s+\S+\s+\S+\s+\S+\s*(.*)', line)
            if m:
                regions.append((int(m.group(1),16), int(m.group(2),16), m.group(3), m.group(4).strip()))
    return regions

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

def validate_dc_base(pid, base):
    """Score a candidate DC RAM base. Higher = more likely."""
    P1C1 = 0x268340; P2C1 = 0x2688E4; P1C2 = 0x268E88
    P2C2 = 0x26942C; P1C3 = 0x2699D0; P2C3 = 0x269F74
    MATCH = 0x289624; FRAME = 0x3496B0

    slots = [P1C1, P2C1, P1C2, P2C2, P1C3, P2C3]
    names = ["P1C1","P2C1","P1C2","P2C2","P1C3","P2C3"]
    score = 0
    info = {}

    for i, (off, name) in enumerate(zip(slots, names)):
        d = read_mem(pid, base + off, 0x530)
        if not d or len(d) < 0x530:
            return 0, {}
        active = d[0]
        cid = d[1]
        health = d[0x420]
        pal = d[0x52D]

        info[name] = {'active': active, 'cid': cid, 'health': health, 'palette': pal}

        # Point characters: only first slot per player is active=1
        if i < 2:  # P1C1, P2C1 — should be active
            if active == 1 and cid < 59:
                score += 10
            elif active == 0 and cid < 59:
                score += 3  # might be between rounds
        else:  # assist chars — active depends on game state
            if cid < 59:
                score += 2
            if active in (0, 1):
                score += 1

        # Palette should be 0-5 typically (default palette index)
        if pal < 6:
            score += 2

        # Health 0-144 is valid range
        if health <= 144:
            score += 1

    # Match flag
    d = read_mem(pid, base + MATCH, 1)
    if d:
        info['in_match'] = d[0]
        if d[0] in (1, 2, 3, 4, 5, 6, 7, 8):
            score += 5

    # Frame counter — should be large and non-zero
    d = read_mem(pid, base + FRAME, 4)
    if d:
        fc = struct.unpack('<I', d)[0]
        info['frame_ctr'] = fc
        if 1000 < fc < 0xFFFFFFFF:
            score += 3

    return score, info

def main():
    pid = int(sys.argv[1])
    print(f"[deep] PID: {pid}")
    regions = read_maps(pid)
    rw = [(s,e,p,path) for s,e,p,path in regions if 'rw' in p]
    print(f"[deep] {len(rw)} RW regions")

    # === Phase 1: Find DC RAM ===
    print(f"\n=== Phase 1: DC RAM scan (exhaustive) ===")
    best_score = 0
    best_base = 0
    best_info = {}

    for start, end, perms, path in rw:
        size = end - start
        if size < 16 * 1024 * 1024:
            continue
        # Try aligned offsets
        for align in range(0, min(size - 16*1024*1024 + 1, 512*1024*1024), 4096):
            base = start + align
            score, info = validate_dc_base(pid, base)
            if score > best_score:
                best_score = score
                best_base = base
                best_info = info
                if score > 30:
                    print(f"  STRONG HIT: 0x{base:016X} score={score}")
                    for k, v in info.items():
                        if isinstance(v, dict):
                            cn = CHAR_NAMES[v['cid']] if v['cid'] < len(CHAR_NAMES) else f"?{v['cid']}"
                            print(f"    {k}: active={v['active']} char={cn}({v['cid']}) hp={v['health']} pal={v['palette']}")
                        else:
                            print(f"    {k}={v}")

            # If we got a great score, stop scanning this region
            if score > 40:
                break
        if best_score > 40:
            break

    if best_score < 15:
        print(f"[deep] No confident DC RAM found (best score={best_score})")
        sys.exit(1)

    dc_base = best_base
    print(f"\n[deep] DC RAM base: 0x{dc_base:016X} (score={best_score})")

    # === Phase 2: Read VRAM area ===
    # DC VRAM is mapped at 0xA4000000 in SH4 space = offset 0x4000000 from area1 start.
    # But in physical memory, VRAM is at 0x04000000 (area1). In the emulator, it's
    # typically a SEPARATE 8MB allocation, NOT contiguous with main RAM.
    # PVR registers at 0x005F8000 (A0 area mirror), palette at offset 0x1000 from PVR base.
    #
    # In flycast source: pvr_regs is a separate array. palette RAM is at
    # &pvr_regs[0x1000/4] through &pvr_regs[0x1FFC/4] — 1024 entries of u32.
    #
    # The Digital Eclipse emulator might store these differently. Let's look for
    # the VRAM 8MB block near DC RAM.

    print(f"\n=== Phase 2: Search for VRAM / PVR registers ===")

    # Strategy: the character struct has a palette index (0x52D). For each character,
    # the game writes palette data to PVR registers. The palette data consists of
    # ARGB4444 colors derived from the character's texture data in VRAM.
    #
    # Let's search for PVR register space. In DC hardware:
    #   PVR regs: 0x005F8000 - 0x005F9FFF (8KB)
    #   Palette:  0x005F9000 - 0x005F9FFF (4KB, 1024 x 32-bit)
    #
    # These are at offset 0x5F8000 from the SH4 area0 base (0xA0000000).
    # In physical space that's 0x005F8000.
    # In an emulator, could be: dc_base - 0x8C000000 + 0x005F8000 = dc_base - 0x8BA08000
    # But that assumes contiguous mapping which is unlikely.
    #
    # Better: just search ALL memory for the palette signature now that we know
    # which characters are in the match. We can look for 16 consecutive ARGB4444
    # values where index 0 is transparent and others match expected character colors.

    # Let's try: the emulator might store PVR palette as 2-byte entries (ARGB4444 packed).
    # Total = 1024 * 2 = 2048 bytes. Or as 4-byte entries = 4096 bytes.
    # Or the emulator converts palettes on-the-fly and stores them in a GPU-friendly
    # format (RGBA8888, 4 bytes each = 4096 bytes total, but different encoding).

    # New strategy: dump a known area of DC RAM where palette DATA lives in the ROM.
    # The texture palette data for each character is loaded into VRAM from the ROM.
    # We can read a unique sequence from DC RAM (character textures are in VRAM),
    # then search process memory for that same sequence.

    # Actually — let's try the simplest thing: search for 8MB regions that could be VRAM.
    # VRAM has texture data, so it should have high entropy. PVR regs are a separate
    # smaller region.

    # Let's search for the PVR register signature: the PVR has known initial values.
    # PVR ID register at offset 0x0000 = 0x17FD11DB (DC) or similar for Naomi.
    # Revision at 0x0004 = 0x00000011.
    # But these might not be in the Fighting Collection's emulator memory if it
    # doesn't emulate the full PVR reg space.

    # Let's try brute force: read all writable memory in 2MB chunks, search for
    # ARGB4444 palette patterns with the right stride.

    # APPROACH: For MVC2, the game writes palette data. During a match, bank 16
    # (P1C1) will have 16 ARGB4444 colors where color[0]=0x0000 and several others
    # are nonzero with alpha >= 0xF (opaque). We search for this pattern at both
    # 2-byte stride and 4-byte stride.

    print("[deep] Searching all RW memory for ARGB4444 palette blocks...")
    print("[deep] Looking for 16 consecutive ARGB4444 values at bank 16 offset...")

    pvr_candidates = []

    for start, end, perms, path in rw:
        if 'r' not in perms:
            continue
        size = end - start
        if size > 512 * 1024 * 1024 or size < 2048:
            continue

        data = read_mem(pid, start, size)
        if not data:
            continue

        # Try 2-byte stride (packed ARGB4444)
        # Bank 16 starts at offset 16*16*2 = 512 bytes from palette start
        # So palette start = candidate - 512
        for off in range(0, len(data) - 2048, 2):
            # Read 16 consecutive u16 values
            colors = [struct.unpack_from('<H', data, off + i*2)[0] for i in range(16)]

            # Check: color[0] should be 0x0000 (transparent)
            if colors[0] != 0x0000:
                continue

            # At least 8 colors should be nonzero with alpha nibble >= 0xE
            opaque_count = sum(1 for c in colors[1:] if c != 0 and (c >> 12) >= 0xE)
            if opaque_count < 8:
                continue

            # This looks like a palette! Where would palette RAM start?
            # If this is bank 16, palette base = off - 16*16*2 = off - 512
            pal_base_off = off - 512
            if pal_base_off < 0:
                continue
            pal_base_addr = start + pal_base_off

            # Verify bank 24 (P2C1) at pal_base + 24*16*2 = pal_base + 768
            b24_off = pal_base_off + 24 * 16 * 2
            if b24_off + 32 > len(data):
                continue
            b24_colors = [struct.unpack_from('<H', data, b24_off + i*2)[0] for i in range(16)]
            b24_opaque = sum(1 for c in b24_colors[1:] if c != 0 and (c >> 12) >= 0xE)

            if b24_colors[0] == 0x0000 and b24_opaque >= 5:
                print(f"\n  *** 2-BYTE STRIDE HIT: palette base 0x{pal_base_addr:016X} ***")
                print(f"      Region: 0x{start:X}-0x{end:X} {path}")
                print(f"      Bank 16 (P1C1): {opaque_count}/15 opaque")
                hex16 = ' '.join(f'{c:04X}' for c in colors)
                print(f"        {hex16}")
                print(f"      Bank 24 (P2C1): {b24_opaque}/15 opaque")
                hex24 = ' '.join(f'{c:04X}' for c in b24_colors)
                print(f"        {hex24}")
                pvr_candidates.append(('2byte', pal_base_addr, 2))
                if len(pvr_candidates) > 5:
                    break

        # Try 4-byte stride (32-bit PVR regs, low 16 bits = ARGB4444)
        # Bank 16 at offset 16*16*4 = 1024
        for off in range(0, len(data) - 4096, 4):
            colors = [struct.unpack_from('<I', data, off + i*4)[0] & 0xFFFF for i in range(16)]
            # Also check high 16 bits are zero (PVR reg style)
            high_ok = all(struct.unpack_from('<I', data, off + i*4)[0] >> 16 == 0 for i in range(16))

            if colors[0] != 0x0000:
                continue
            opaque_count = sum(1 for c in colors[1:] if c != 0 and (c >> 12) >= 0xE)
            if opaque_count < 8:
                continue
            if not high_ok:
                continue

            pal_base_off = off - 16 * 16 * 4
            if pal_base_off < 0:
                continue
            pal_base_addr = start + pal_base_off

            b24_off = pal_base_off + 24 * 16 * 4
            if b24_off + 64 > len(data):
                continue
            b24_colors = [struct.unpack_from('<I', data, b24_off + i*4)[0] & 0xFFFF for i in range(16)]
            b24_opaque = sum(1 for c in b24_colors[1:] if c != 0 and (c >> 12) >= 0xE)
            b24_high_ok = all(struct.unpack_from('<I', data, b24_off + i*4)[0] >> 16 == 0 for i in range(16))

            if b24_colors[0] == 0x0000 and b24_opaque >= 5 and b24_high_ok:
                print(f"\n  *** 4-BYTE STRIDE HIT: palette base 0x{pal_base_addr:016X} ***")
                print(f"      Region: 0x{start:X}-0x{end:X} {path}")
                print(f"      Bank 16 (P1C1): {opaque_count}/15 opaque")
                hex16 = ' '.join(f'{c:04X}' for c in colors)
                print(f"        {hex16}")
                print(f"      Bank 24 (P2C1): {b24_opaque}/15 opaque")
                hex24 = ' '.join(f'{c:04X}' for c in b24_colors)
                print(f"        {hex24}")
                pvr_candidates.append(('4byte', pal_base_addr, 4))
                if len(pvr_candidates) > 5:
                    break

        if len(pvr_candidates) > 5:
            break

    print(f"\n=== Summary ===")
    print(f"DC RAM: 0x{dc_base:016X} (score {best_score})")
    print(f"PVR palette candidates: {len(pvr_candidates)}")
    for stride_name, addr, stride in pvr_candidates:
        print(f"  0x{addr:016X} ({stride_name}, entry size={stride})")

    # If we found candidates, try a write test on the first one
    if pvr_candidates:
        stride_name, pal_base, stride = pvr_candidates[0]
        bank = 16
        bank_addr = pal_base + bank * 16 * stride
        print(f"\n=== Write test on first candidate ===")
        print(f"Writing bright red palette to bank 16 at 0x{bank_addr:X} ({stride_name})...")

        # Build test palette
        test_colors = [0x0000, 0xFF00, 0xFF00, 0xFF00, 0xFF00, 0xFF00, 0xFF00, 0xFF00,
                       0xFF00, 0xFF00, 0xFF00, 0xFF00, 0xFF00, 0xFF00, 0xFF00, 0xFF00]

        # Read original
        orig = read_mem(pid, bank_addr, 16 * stride)

        # Write test
        buf = bytearray(16 * stride)
        for i, c in enumerate(test_colors):
            if stride == 2:
                struct.pack_into('<H', buf, i * 2, c)
            else:
                struct.pack_into('<I', buf, i * 4, c)
        write_mem(pid, bank_addr, bytes(buf))
        print(f"Written! Check if P1's character turned RED.")
        print(f"Sleeping 5 seconds then restoring...")
        time.sleep(5)
        write_mem(pid, bank_addr, orig)
        print(f"Restored original palette.")

if __name__ == '__main__':
    main()
