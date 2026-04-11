#!/usr/bin/env python3
"""
Find the decompressed MVC2 ROM in the Marvel Collections process memory.
The ROM is 112,635,968 bytes and starts with "IBIS" magic.
Once found, we can use karttoon's palette offsets directly.

Usage:
    python3 tools/find_rom_in_mem.py <PID>
"""
import sys, struct, re, time

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

# karttoon's STEAM_PALETTE_OFFSETS — LP base offset for each character
# From mvc2_data/steam.py
STEAM_PALETTE_OFFSETS = {
    0x00: 0x82CC60,   # Ryu
    0x01: 0x89F620,   # Zangief
    0x02: 0x859260,   # Guile
    0x03: 0x99D960,   # Morrigan
    0x04: 0x7E0D60,   # Anakaris
    0x05: 0xAC7B60,   # Strider
    0x06: 0x8B8560,   # Cyclops
    0x07: 0xE32D20,   # Wolverine
    0x08: 0xA51360,   # Psylocke
    0x09: 0x8E0660,   # Iceman
    0x0A: 0x9EBB60,   # Rogue
    0x0B: 0x7F0960,   # Captain America
    0x0C: 0xA86560,   # Spider-Man
    0x0D: 0x8C9960,   # Hulk
    0x0E: 0xE07160,   # Venom
    0x0F: 0x83C960,   # Doctor Doom
    0x10: 0xDB9D60,   # Tron Bonne
    0x11: 0x90CB60,   # Jill
    0x12: 0x8DD960,   # Hayato
    0x13: 0x9C6860,   # Ruby Heart
    0x14: 0xA31A60,   # SonSon
    0x15: 0x7CF560,   # Amingo
    0x16: 0x977560,   # Marrow
    0x17: 0x7FFF60,   # Cable
    0x1B: 0x80E160,   # Chun-Li
    0x1C: 0x980660,   # Mega Man
    0x1D: 0x9D5760,   # Roll
    0x1E: 0x7BD460,   # Akuma
    0x1F: 0x7D0560,   # BB Hood
    0x20: 0x85DD60,   # Felicia
    0x21: 0x80FE60,   # Charlie
    0x22: 0xA60660,   # Sakura
    0x23: 0x824060,   # Dan
    0x24: 0x7FF960,   # Cammy
    0x25: 0x83A560,   # Dhalsim
    0x26: 0x98ED60,   # M.Bison
    0x27: 0x92ED60,   # Ken
    0x28: 0x86CF60,   # Gambit
    0x29: 0x929B60,   # Juggernaut
    0x2A: 0xAA0B60,   # Storm
    0x2B: 0xA12A60,   # Sabretooth
    0x2C: 0x960160,   # Magneto
    0x2D: 0xA95D60,   # Shuma-Gorath
    0x2E: 0xE20960,   # War Machine
    0x2F: 0xA70260,   # Silver Samurai
    0x30: 0xA06660,   # Omega Red
    0x31: 0xAB0260,   # Spiral
    0x32: 0x81A560,   # Colossus
    0x33: 0xE12D60,   # Iron Man
    0x34: 0xB26160,   # Sentinel
    0x35: 0x7E7260,   # Blackheart
    0x36: 0xDE8B60,   # Thanos
    0x37: 0x8F7B60,   # Jin
    0x38: 0x80A560,   # Captain Commando
    0x39: 0xE44C60,   # Bone Wolverine
    0x3A: 0xA3DD60,   # Servbot
}

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

def main():
    pid = int(sys.argv[1])
    print(f"[rom] PID: {pid}")

    regions = read_maps(pid)
    rw = [(s,e,p,path) for s,e,p,path in regions if 'r' in p]
    print(f"[rom] {len(rw)} readable regions")

    ROM_SIZE = 112_635_968  # expected decompressed ROM size
    IBIS_MAGIC = b'IBIS'

    print(f"\n=== Phase 1: Search for 'IBIS' magic header ===")

    rom_base = None
    for start, end, perms, path in rw:
        size = end - start
        if size < 1024:
            continue
        # Read first part of each large region looking for IBIS
        # Also scan inside large regions
        chunk = 16 * 1024 * 1024  # 16MB scan chunks
        for off in range(0, min(size, 512 * 1024 * 1024), chunk):
            read_size = min(chunk, size - off)
            data = read_mem(pid, start + off, read_size)
            if data is None:
                break
            idx = 0
            while True:
                idx = data.find(IBIS_MAGIC, idx)
                if idx == -1:
                    break
                addr = start + off + idx
                # Verify: read more data at this address to check if it's the ROM
                # The ROM should be ~107MB, check if region is large enough
                print(f"  IBIS found at 0x{addr:016X} (region: 0x{start:X}-0x{end:X} {path})")

                # Check if we can read a Ryu palette at the known offset
                ryu_off = STEAM_PALETTE_OFFSETS[0x00]  # Ryu LP base
                ryu_data = read_mem(pid, addr + ryu_off, 32)
                if ryu_data:
                    colors = [struct.unpack_from('<H', ryu_data, i*2)[0] for i in range(16)]
                    nonzero = sum(1 for c in colors if c != 0)
                    opaque = sum(1 for c in colors[1:] if c != 0 and (c >> 12) >= 0xE)
                    print(f"    Ryu palette at +0x{ryu_off:X}: {nonzero} nonzero, {opaque} opaque")
                    hex_str = ' '.join(f'{c:04X}' for c in colors)
                    print(f"    {hex_str}")

                    if opaque >= 5:
                        print(f"    *** LOOKS LIKE REAL PALETTE DATA! ***")
                        rom_base = addr
                        break

                # Also try Sentinel
                sent_off = STEAM_PALETTE_OFFSETS[0x34]  # Sentinel LP base
                sent_data = read_mem(pid, addr + sent_off, 32)
                if sent_data:
                    colors = [struct.unpack_from('<H', sent_data, i*2)[0] for i in range(16)]
                    opaque = sum(1 for c in colors[1:] if c != 0 and (c >> 12) >= 0xE)
                    hex_str = ' '.join(f'{c:04X}' for c in colors)
                    print(f"    Sentinel palette at +0x{sent_off:X}: {opaque} opaque")
                    print(f"    {hex_str}")

                    if opaque >= 5:
                        print(f"    *** LOOKS LIKE REAL PALETTE DATA! ***")
                        rom_base = addr
                        break

                idx += 1
            if rom_base:
                break
        if rom_base:
            break

    if not rom_base:
        print("\n[rom] IBIS header not found. Trying brute-force palette search...")
        # Search for Sentinel's palette directly using known ROM offset relationships
        # If we find Sentinel's palette, we can compute ROM base = found_addr - sentinel_offset
        print("[rom] Searching for Sentinel LP palette signature...")

        # Read Sentinel's palette from the actual arc file to get the reference bytes
        arc_path = None
        import glob
        steam_paths = glob.glob('/home/tris/snap/steam/common/.local/share/Steam/steamapps/common/MARVEL*/**/*.arc', recursive=True)
        if not steam_paths:
            steam_paths = glob.glob('/home/tris/.steam/steam/steamapps/common/MARVEL*/**/*.arc', recursive=True)
        print(f"[rom] Found arc files: {steam_paths}")

        if steam_paths:
            import zlib
            for arc in steam_paths:
                if 'game_50' not in arc.lower() and '50' not in arc:
                    continue
                print(f"[rom] Reading {arc}...")
                with open(arc, 'rb') as f:
                    arc_data = f.read()
                # Try decompressing
                try:
                    rom = zlib.decompress(arc_data)
                    print(f"[rom] Decompressed: {len(rom)} bytes")
                    if rom[:4] == b'IBIS':
                        print(f"[rom] IBIS header confirmed!")
                        # Read Sentinel palette from ROM
                        sent_off = STEAM_PALETTE_OFFSETS[0x34]
                        sent_pal = rom[sent_off:sent_off+32]
                        colors = [struct.unpack_from('<H', sent_pal, i*2)[0] for i in range(16)]
                        hex_str = ' '.join(f'{c:04X}' for c in colors)
                        print(f"[rom] Sentinel palette from ROM: {hex_str}")

                        # Now search process memory for this exact 32-byte sequence
                        print(f"[rom] Searching process memory for this 32-byte sequence...")
                        for start, end, perms, path in rw:
                            size = end - start
                            if size < 32 or size > 512*1024*1024:
                                continue
                            data = read_mem(pid, start, size)
                            if not data:
                                continue
                            fidx = data.find(sent_pal)
                            if fidx >= 0:
                                found_addr = start + fidx
                                computed_base = found_addr - sent_off
                                print(f"\n  *** FOUND Sentinel palette at 0x{found_addr:016X} ***")
                                print(f"      Computed ROM base: 0x{computed_base:016X}")
                                rom_base = computed_base
                                break
                        break
                except:
                    pass

    if not rom_base:
        print("\n[rom] Could not find ROM in process memory.")
        sys.exit(1)

    print(f"\n=== Phase 2: Verify ROM base 0x{rom_base:016X} ===")

    # Read multiple character palettes to confirm
    test_chars = [
        (0x34, "Sentinel"),
        (0x2C, "Magneto"),
        (0x32, "Colossus"),
        (0x00, "Ryu"),
        (0x2A, "Storm"),
    ]
    for cid, name in test_chars:
        if cid not in STEAM_PALETTE_OFFSETS:
            continue
        off = STEAM_PALETTE_OFFSETS[cid]
        data = read_mem(pid, rom_base + off, 32)
        if data:
            colors = [struct.unpack_from('<H', data, i*2)[0] for i in range(16)]
            hex_str = ' '.join(f'{c:04X}' for c in colors)
            opaque = sum(1 for c in colors[1:] if c != 0 and (c >> 12) >= 0xE)
            print(f"  {name:20s} (+0x{off:08X}): {opaque:2d}/15 opaque | {hex_str}")

    print(f"\n=== Phase 3: Write test — turning Sentinel RED ===")
    sent_off = STEAM_PALETTE_OFFSETS[0x34]
    sent_addr = rom_base + sent_off

    # Read original
    orig = read_mem(pid, sent_addr, 32)
    print(f"  Original Sentinel palette:")
    orig_colors = [struct.unpack_from('<H', orig, i*2)[0] for i in range(16)]
    print(f"  {' '.join(f'{c:04X}' for c in orig_colors)}")

    # Write all-red
    red = bytearray(32)
    red_colors = [0x0000] + [0xFF00] * 15
    for i, c in enumerate(red_colors):
        struct.pack_into('<H', red, i*2, c)

    write_mem(pid, sent_addr, bytes(red))
    print(f"  >>> WROTE RED to Sentinel LP palette — look at the game! (5 seconds)")
    time.sleep(5)

    write_mem(pid, sent_addr, orig)
    print(f"  Restored original palette.")

    print(f"\n=== RESULT ===")
    print(f"  ROM base in memory: 0x{rom_base:016X}")
    print(f"  Use karttoon's offsets directly: rom_base + STEAM_PALETTE_OFFSETS[char_id]")
    print(f"  Palette formula: base + (button_idx * 8 + slot) * 32")

if __name__ == '__main__':
    main()
