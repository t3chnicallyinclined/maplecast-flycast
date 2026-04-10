#!/usr/bin/env python3
"""
Import MVC2 community skins from PNG files into SurrealDB.

Extracts ARGB4444 palette data from skin PNGs (8-bit palettized images),
stores character ID, name, author/pack credit, palette hex, and thumbnail path.

Source: https://github.com/karttoon/mvc2-skins
All credit to the original skin creators and the MVC2 modding community.

Usage:
    python3 tools/import_skins.py [--db-url http://localhost:8000] [--dry-run]
"""

import os
import sys
import json
import struct
import argparse
import hashlib
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("pip install Pillow")
    sys.exit(1)

try:
    import requests
except ImportError:
    print("pip install requests")
    sys.exit(1)

# MVC2 character ID mapping (matches the game's internal IDs)
CHAR_IDS = {
    "Ryu": 0x00, "Zangief": 0x01, "Guile": 0x02, "Morrigan": 0x03,
    "Anakaris": 0x04, "Strider": 0x05, "Cyclops": 0x06, "Wolverine": 0x07,
    "Psylocke": 0x08, "Iceman": 0x09, "Rogue": 0x0A, "Captain_America": 0x0B,
    "Spider-Man": 0x0C, "Hulk": 0x0D, "Venom": 0x0E, "Doctor_Doom": 0x0F,
    "Tron_Bonne": 0x10, "Jill": 0x11, "Hayato": 0x12, "Ruby_Heart": 0x13,
    "SonSon": 0x14, "Amingo": 0x15, "Marrow": 0x16, "Cable": 0x17,
    "Chun-Li": 0x1B, "Mega_Man": 0x1C, "Roll": 0x1D, "Akuma": 0x1E,
    "BB_Hood": 0x1F, "Felicia": 0x20, "Charlie_Nash": 0x21, "Sakura": 0x22,
    "Dan": 0x23, "Cammy": 0x24, "Dhalsim": 0x25, "M_Bison": 0x26,
    "Ken": 0x27, "Gambit": 0x28, "Juggernaut": 0x29, "Storm": 0x2A,
    "Sabretooth": 0x2B, "Magneto": 0x2C, "Shuma-Gorath": 0x2D,
    "War_Machine": 0x2E, "Silver_Samurai": 0x2F, "Omega_Red": 0x30,
    "Spiral": 0x31, "Colossus": 0x32, "Iron_Man": 0x33, "Sentinel": 0x34,
    "Blackheart": 0x35, "Thanos": 0x36, "Jin": 0x37,
    "Captain_Commando": 0x38, "Bone_Wolverine": 0x39, "Wolverine_Bone_Claw": 0x39, "Servbot": 0x3A,
}

# Reverse: directory name → char_id
DIR_TO_ID = {}
for name, cid in CHAR_IDS.items():
    DIR_TO_ID[name] = cid
    # Handle variations
    DIR_TO_ID[name.replace("-", "_")] = cid
    DIR_TO_ID[name.replace("_", " ")] = cid


def extract_palette(png_path):
    """Extract 16-color ARGB4444 palette from a palettized PNG."""
    try:
        img = Image.open(png_path)
    except Exception as e:
        return None, str(e)

    if img.mode != 'P':
        return None, f"not palettized (mode={img.mode})"

    pal = img.getpalette()
    if not pal or len(pal) < 48:  # need at least 16 RGB triples
        return None, "palette too short"

    # Convert first 16 colors to ARGB4444
    colors = []
    for i in range(16):
        r, g, b = pal[i*3], pal[i*3+1], pal[i*3+2]
        a = 0 if i == 0 else 15  # index 0 = transparent
        r4 = r >> 4
        g4 = g >> 4
        b4 = b >> 4
        c16 = (a << 12) | (r4 << 8) | (g4 << 4) | b4
        colors.append(c16)

    # Build hex string (LE u16 pairs)
    hex_str = ''.join(f'{c:04x}' for c in colors)
    return colors, hex_str


def parse_filename(char_dir, filename):
    """Parse skin filename to extract hash and author/pack name."""
    base = filename.replace('.png', '')

    # Handle multi-underscore character names (BB_Hood, Tron_Bonne, etc.)
    # The format is: CharDirName_hash_OptionalAuthor.png
    # But CharDirName might contain underscores, so we strip the known prefix
    prefix = char_dir + '_'
    if base.startswith(prefix):
        rest = base[len(prefix):]
    else:
        rest = base

    parts = rest.split('_', 1)
    skin_hash = parts[0] if parts else ''
    author = parts[1].replace('-', ' ') if len(parts) > 1 else 'Community'

    return skin_hash, author


def import_to_surrealdb(skins, db_url, ns="maplecast", db="mvc2", db_pass="nobd_arcade_2026"):
    """Import skin records to SurrealDB."""
    headers = {
        "Accept": "application/json",
        "Content-Type": "application/json",
        "surreal-ns": ns,
        "surreal-db": db,
    }

    # Create the skin table
    print(f"\nImporting {len(skins)} skins to {db_url}...")

    # Batch insert in chunks of 100
    batch_size = 100
    imported = 0
    errors = 0

    for i in range(0, len(skins), batch_size):
        batch = skins[i:i+batch_size]
        # Build SurrealQL INSERT
        queries = []
        for s in batch:
            record_id = f"skin:{s['char_dir']}_{s['hash']}"
            queries.append(
                f"CREATE {record_id} SET "
                f"char_id = {s['char_id']}, "
                f"char_name = '{s['char_name']}', "
                f"char_dir = '{s['char_dir']}', "
                f"author = '{s['author'].replace(chr(39), '')}', "
                f"hash = '{s['hash']}', "
                f"palette_hex = '{s['palette_hex']}', "
                f"colors = {json.dumps(s['colors'])}, "
                f"filename = '{s['filename']}', "
                f"thumb_path = '/skins/{s['char_dir']}/{s['filename']}', "
                f"source = 'https://github.com/karttoon/mvc2-skins', "
                f"credit = 'Community skin by {s['author'].replace(chr(39), '')}. All credit to original creators.'"
                f";"
            )

        query = "\n".join(queries)
        try:
            resp = requests.post(
                f"{db_url}/sql",
                data=query,
                headers=headers,
                auth=("root", db_pass),
            )
            if resp.status_code == 200:
                imported += len(batch)
            else:
                errors += len(batch)
                if errors <= 3:
                    print(f"  Error batch {i}: {resp.status_code} {resp.text[:200]}")
        except Exception as e:
            errors += len(batch)
            if errors <= 3:
                print(f"  Error: {e}")

        if (i + batch_size) % 500 == 0 or i + batch_size >= len(skins):
            print(f"  {imported}/{len(skins)} imported, {errors} errors")

    return imported, errors


def main():
    parser = argparse.ArgumentParser(description="Import MVC2 skins to SurrealDB")
    parser.add_argument("--skin-dir", default="tools/mvc2-skins/skins",
                       help="Path to skin PNGs directory")
    parser.add_argument("--db-url", default="http://66.55.128.93:8000",
                       help="SurrealDB URL")
    parser.add_argument("--db-pass", default="nobd_arcade_2026",
                       help="SurrealDB password")
    parser.add_argument("--dry-run", action="store_true",
                       help="Extract palettes but don't import to DB")
    parser.add_argument("--limit", type=int, default=0,
                       help="Limit number of skins to import (0=all)")
    args = parser.parse_args()

    skin_dir = Path(args.skin_dir)
    if not skin_dir.exists():
        print(f"Skin directory not found: {skin_dir}")
        sys.exit(1)

    print("=" * 60)
    print("MVC2 Community Skin Importer")
    print("Source: https://github.com/karttoon/mvc2-skins")
    print("All credit to the original skin creators and the")
    print("MVC2 modding community (PalMod, karttoon, Preppy, etc.)")
    print("=" * 60)

    skins = []
    skipped = 0

    for char_dir in sorted(os.listdir(skin_dir)):
        char_path = skin_dir / char_dir
        if not char_path.is_dir():
            continue

        char_id = DIR_TO_ID.get(char_dir, -1)
        if char_id < 0:
            print(f"  WARNING: unknown character directory '{char_dir}', skipping")
            continue

        char_name = char_dir.replace('_', ' ')

        for fname in sorted(os.listdir(char_path)):
            if not fname.endswith('.png'):
                continue

            if args.limit and len(skins) >= args.limit:
                break

            skin_hash, author = parse_filename(char_dir, fname)
            colors, palette_hex = extract_palette(char_path / fname)

            if colors is None:
                skipped += 1
                continue

            skins.append({
                'char_id': char_id,
                'char_name': char_name,
                'char_dir': char_dir,
                'hash': skin_hash,
                'author': author,
                'colors': colors,
                'palette_hex': palette_hex,
                'filename': fname,
            })

        if args.limit and len(skins) >= args.limit:
            break

    print(f"\nExtracted {len(skins)} palettes, skipped {skipped}")

    # Show sample
    if skins:
        s = skins[0]
        print(f"\nSample: {s['char_name']} by {s['author']}")
        print(f"  Colors: {[f'0x{c:04X}' for c in s['colors'][:4]]}...")
        print(f"  Hex: {s['palette_hex'][:32]}...")

    if args.dry_run:
        print("\n[DRY RUN] Skipping database import")
        # Save to JSON for inspection
        with open("tools/skins_export.json", "w") as f:
            json.dump(skins[:10], f, indent=2)
        print(f"Saved sample to tools/skins_export.json")
        return

    imported, errors = import_to_surrealdb(skins, args.db_url, db_pass=args.db_pass)
    print(f"\nDone: {imported} imported, {errors} errors, {skipped} skipped")
    print(f"\nQuery skins: curl -s {args.db_url}/sql -H 'NS: maplecast' -H 'DB: mvc2' "
          f"-d 'SELECT * FROM skin WHERE char_name = \"Sentinel\" LIMIT 5;'")


if __name__ == "__main__":
    main()
