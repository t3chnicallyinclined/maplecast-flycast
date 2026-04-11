# MVC2 Skin System — How MapleCast Changes Character Colors Live

## What this does

MapleCast lets spectators and players change any character's color palette in real-time during a live MVC2 match. The change is visible to everyone watching — every browser viewer, every native client. No ROM hacking, no texture replacement, no restart required. One click and the character turns pink, or gold, or whatever the community skin defines.

This document explains exactly how that works, from pixel to wire.

## Background: how MVC2 renders character colors

MVC2 runs on Dreamcast/Naomi hardware that uses **paletted textures**. Character sprites are not stored as full-color images. Instead, each pixel in a sprite is an **index** (0-15) into a 16-color **palette**. The palette itself is a small table of 16 ARGB4444 colors (32 bytes total) stored in the PVR GPU's **palette RAM**.

This is why palette swaps are so cheap — you don't touch the texture data at all. You change 32 bytes of palette and every pixel that references those colors changes instantly.

### ARGB4444 format

Each color is 16 bits (2 bytes), little-endian:

```
Bit layout:  AAAA RRRR GGGG BBBB
             ╰─┘  ╰─┘  ╰─┘  ╰─┘
              α    red  grn  blu    (each 0-15, multiply by 17 for 0-255)
```

- `0x0000` = fully transparent (palette index 0 is always this)
- `0xFFFF` = opaque white
- `0xFF00` = opaque red
- `0xF00F` = opaque blue

### PVR palette RAM layout

The PVR GPU has **1024 palette entries**, each stored as a 32-bit register (only the low 16 bits matter). Total: 4096 bytes.

These 1024 entries are divided into **64 banks of 16 colors each**. MVC2 assigns specific banks to each character slot using a formula:

```
bank = 16 × (char_pair + 1) + (8 × player_side)
```

| Slot | Pair | Side | Bank | PVR entry range | Byte offset in palette RAM |
|------|------|------|------|-----------------|---------------------------|
| P1 Character 1 | 0 | 0 | 16 | 256-271 | 0x400 |
| P2 Character 1 | 0 | 1 | 24 | 384-399 | 0x600 |
| P1 Character 2 | 1 | 0 | 32 | 512-527 | 0x800 |
| P2 Character 2 | 1 | 1 | 40 | 640-655 | 0xA00 |
| P1 Character 3 | 2 | 0 | 48 | 768-783 | 0xC00 |
| P2 Character 3 | 2 | 1 | 56 | 896-911 | 0xE00 |

To write P1's first character's palette, you write 16 ARGB4444 values starting at PVR palette entry 256 (bank 16 × 16 entries per bank).

## How the headless server makes this work

### The norend trick

The headless flycast server has **no GPU**. It runs in `norend` mode — no OpenGL, no Vulkan, no rendering at all. This means PVR palette RAM is always empty. The game's CPU writes palette data through normal Dreamcast hardware emulation, but since nothing renders, it doesn't matter.

Here's the trick: **our palette writes are the only thing in PVR palette RAM that matters**. We write our custom colors, and they get shipped to every viewer through the normal TA mirror stream. There's no GPU fighting us for palette ownership because there is no GPU.

### The override loop

Every frame (60 times per second), the server runs this sequence:

```
1.  Game CPU runs one frame of MVC2
2.  Game writes its default palette to PVR registers (norend, goes nowhere)
3.  applyPaletteOverrides() runs ← OUR CODE
    └─ Writes all active skin palettes to PVR palette RAM
    └─ Toggles entry 1023 to force the page dirty
4.  Memory diff scan runs
    └─ Detects PVR palette page changed (our writes + the toggle)
    └─ Packages it as a PVR dirty page (region 3) in the delta frame
5.  Delta frame ships to relay → all browsers/clients
6.  Viewers apply the PVR page diff → their local palette RAM updates
7.  Next render uses the new palette → character colors change
```

The key insight: `applyPaletteOverrides()` runs **after** the game writes its defaults but **before** the diff scan. Our writes overwrite whatever the game put there. And because we do this every frame, we always win — even across scene transitions, round resets, or super move palette flashes.

### Forcing the dirty page

After the first frame, the PVR shadow copy matches our override (both have our custom colors). The diff scan would see no change and stop shipping the palette page. To prevent this, we toggle entry 1023 (an unused palette slot) with an incrementing counter every frame:

```cpp
static u32 _tick = 0;
pvr_WriteReg(PALETTE_RAM_START_addr + 1023 * 4, ++_tick & 0xF);
```

This is a 1-microsecond operation that ensures the palette page is always in the delta frame. Cost: 4 extra bytes per frame in the wire format.

### Persistence via upsert

Palette overrides are stored in a `std::vector<PaletteOverride>`, keyed by `startIndex`. When a new skin is applied for the same character slot, it **replaces** the existing override (upsert). This prevents duplicates from stacking up.

```cpp
struct PaletteOverride {
    bool active = false;
    int startIndex = 0;           // PVR palette entry (0-1023)
    std::vector<u16> colors;      // 16 ARGB4444 values
};
```

A mutex protects the vector since the WS handler thread and the emulator publish thread both access it.

## Wire path: browser → server → everyone

### Sending a skin change

The browser's skin picker sends a JSON message over the control WebSocket (`wss://nobd.net/play` → nginx → flycast port 7210):

```json
{
  "cmd": "palette_write",
  "index": 256,
  "colors": [0, 64764, 63312, 48048, 30702, 12338, 8226, 64764, 60080, 48048, 4114, 8226, 30702, 30702, 4114, 0],
  "persist": true
}
```

- `index`: starting PVR palette entry (bank × 16). For P1C1, bank 16 → index 256.
- `colors`: array of 16 ARGB4444 values as integers.
- `persist`: if true, re-applied every frame. If false, one-shot write (game's next palette write overwrites it).

### Server handler

```
WS message received
  → Parse JSON, find "cmd":"palette_write"
  → Immediate write: pvr_WriteReg() for each color
  → If persist: upsert into _palOverrides vector
  → Next frame: applyPaletteOverrides() re-applies
  → Diff scan captures the change
  → Delta frame ships to all viewers
```

### Clearing skins

```json
{"cmd": "palette_clear"}
```

Empties the `_palOverrides` vector. Next frame, the game's default palette writes take effect (they were always happening, just being overwritten by us). Characters revert to stock colors within one frame.

## Where the skin data comes from

### Community skins database

5,202 community-created MVC2 color palettes stored in SurrealDB:

- **Namespace**: `maplecast`, **Database**: `mvc2`, **Table**: `skin`
- **Source**: [github.com/karttoon/mvc2-skins](https://github.com/karttoon/mvc2-skins) (PNG sprite sheets)
- **Import tool**: `tools/import_skins.py` extracts ARGB4444 palettes from the PNG sprite sheets
- **Fields**: `char_id`, `char_name`, `char_dir`, `author`, `palette_hex`, `colors[]`, `credit`

Every skin is credited to its original creator. The skin picker shows "skin by [author]" on the game overlay.

### How palettes are extracted from PNGs

The community skins repo contains PNG sprite sheets where each character variation uses a different set of 16 colors. The import script:

1. Reads the PNG, scans for unique colors
2. Converts each color from RGB888 to ARGB4444 (quantize to 4 bits per channel, set alpha to 0xF)
3. First color (background) becomes 0x0000 (transparent)
4. Stores the 16-color palette as both a hex string and integer array

## What this doesn't cover (yet)

### Multi-palette characters

Some characters use more than one 16-color palette:

- **Sentinel**: main body + head/limbs may use adjacent banks
- **Storm**: glow/lightning effects cycle through animation palettes
- **Blackheart**: demon effects use separate palette entries
- **Projectiles**: fireballs, beams often have their own palette bank

The current system overrides **one bank per character slot** — the primary sprite palette. Effect palettes and secondary palettes are not targeted. This matches the community skin data (which is also single-palette per character).

Full multi-palette skinning would require mapping each character's complete palette usage across all banks — a per-character research effort.

### Server-side vs client-side

Currently, all skin changes go through the **server**. The server writes to PVR palette RAM, and the change propagates to every viewer through the TA mirror stream. This means:

- Everyone sees the same skin (spectators included)
- Only players in a slot can change skins (the skin picker is gated)
- Latency is one frame (16ms) from click to visual change for the person who applied it, and one network round-trip for others

A **client-side** palette override is also possible (write to local PALETTE_RAM before rendering), but this would only be visible to that one viewer. The native mirror client (`MAPLECAST_MIRROR_CLIENT=1`) supports this path via `maplecast_palette::applyClientOverrides()`.

## Applying this to other emulators

The technique works on **any emulator running the MVC2 Naomi/DC ROM**, because the palette format is a property of the ROM, not the emulator. The 32-byte ARGB4444 palettes from SurrealDB are byte-identical regardless of platform.

What changes per emulator:

| Component | Our flycast fork | Other emulator |
|-----------|-----------------|----------------|
| Where PVR palette RAM lives | `pvr_WriteReg()` C++ API | Need to find in process memory |
| How to write to it | Direct function call (same process) | `/proc/<pid>/mem` write, DLL injection, or Cheat Engine |
| How changes propagate | TA mirror stream (built-in) | Screenshot/capture only (local) |
| Bank formula | Same | Same (it's the ROM's formula) |
| Palette data format | Same | Same (ARGB4444) |

The hard part for other emulators is finding PVR palette RAM in their process memory. See `tools/scan_pvr_palette.py` for a memory scanner that searches a running process for the palette signature.
