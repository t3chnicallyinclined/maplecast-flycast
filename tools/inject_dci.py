#!/usr/bin/env python3
"""Inject a DCI save file into a Flycast VMU .bin image."""
import struct
import sys
import shutil

def inject_dci(vmu_path, dci_path, output_path):
    with open(vmu_path, 'rb') as f:
        vmu = bytearray(f.read())
    with open(dci_path, 'rb') as f:
        dci = f.read()

    assert len(vmu) == 131072, f"VMU must be 128KB, got {len(vmu)}"
    assert len(dci) >= 32, f"DCI too small: {len(dci)}"

    BLOCK = 512

    # Parse DCI header (32 bytes = directory entry)
    dir_entry = dci[:32]
    block_data = dci[32:]
    file_blocks = struct.unpack_from('<H', dir_entry, 0x18)[0]
    fname = dir_entry[4:16]
    print(f"DCI: filename={fname}, blocks={file_blocks}, data={len(block_data)} bytes")
    assert file_blocks * BLOCK == len(block_data), "DCI size mismatch"

    # Read FAT from VMU (block 254)
    fat_off = 254 * BLOCK
    fat = []
    for i in range(256):
        fat.append(struct.unpack_from('<H', vmu, fat_off + i * 2)[0])

    # Find free blocks (0xFFFC = free), allocate from top down (DC convention)
    free_blocks = []
    for i in range(199, -1, -1):
        if fat[i] == 0xFFFC:
            free_blocks.append(i)
        if len(free_blocks) >= file_blocks:
            break

    assert len(free_blocks) >= file_blocks, f"Not enough free blocks: need {file_blocks}, have {len(free_blocks)}"
    print(f"Allocating blocks: {free_blocks[:file_blocks]}")

    # Write data blocks to VMU — byte-swap each uint32 word
    # DCI stores data with swapped byte order within each 4-byte word
    for i in range(file_blocks):
        blk = free_blocks[i]
        src = block_data[i * BLOCK:(i + 1) * BLOCK]
        # Byte-swap each 4-byte word
        swapped = bytearray(BLOCK)
        for w in range(0, BLOCK, 4):
            swapped[w+0] = src[w+3]
            swapped[w+1] = src[w+2]
            swapped[w+2] = src[w+1]
            swapped[w+3] = src[w+0]
        vmu[blk * BLOCK:blk * BLOCK + BLOCK] = swapped

    # Update FAT chain
    for i in range(file_blocks - 1):
        fat[free_blocks[i]] = free_blocks[i + 1]
    fat[free_blocks[file_blocks - 1]] = 0xFFFA  # end of chain

    # Write FAT back
    for i in range(256):
        struct.pack_into('<H', vmu, fat_off + i * 2, fat[i])

    # Find empty directory entry (block 253, going down)
    dir_start = 253
    found = False
    for b in range(dir_start, dir_start - 13, -1):
        for e in range(16):
            entry_off = b * BLOCK + e * 32
            if vmu[entry_off] == 0:  # empty entry
                # Write directory entry from DCI header
                vmu[entry_off:entry_off + 32] = dir_entry
                # Update first block pointer to our allocated block
                struct.pack_into('<H', vmu, entry_off + 0x02, free_blocks[0])
                print(f"Directory entry at block {b}, entry {e}, first_block={free_blocks[0]}")
                found = True
                break
        if found:
            break

    assert found, "No free directory entry"

    with open(output_path, 'wb') as f:
        f.write(vmu)
    print(f"Written: {output_path} ({len(vmu)} bytes)")

if __name__ == '__main__':
    # Use the clean Flycast-formatted VMU (A2) as base
    base_vmu = "C:/Users/trist/projects/GP2040-CE/VMU/vmu_save_A2.bin"
    dci_file = "C:/Users/trist/projects/GP2040-CE/VMU/marvel-vs-capcom-2.21565.dci"
    output = "C:/Users/trist/projects/GP2040-CE/VMU/T1212N_vmu_save_A1.bin"

    inject_dci(base_vmu, dci_file, output)
