#!/usr/bin/env python3
"""
extract_replica_gfx.py — Phase A LOCAL-ROM GFX cache extractor (render-replica).

Stages the RAW per-character GFX1/GFX2 segments — the EXACT bytes the body walker
reads from RAM — as web-servable files for the render-replica client to overlay into
its RAM image at node+0x15C (GFX1) / node+0x160 (GFX2), OVERRIDING whatever stale/
truncated GFX the server shipped. This is the local-ROM cache: the client holds the
complete, never-truncated art; the server's GFX is overridden.

WHY RAW (not the decoded atlas): the transpiled walker reads geometry from GFX2
(node+0x160) and the decoder reads pixel LZSS streams from GFX1 (node+0x15C). Both
expect the verbatim segment bytes. We do NOT decode here — we copy the segments the
emulator itself loads.

GROUND TRUTH (CONFIRMED 2026-06-13, this tool's --verify-ram gate vs _ryu_capture/
mc_ram_dump.bin): the dasm_PLDAT segment files are BYTE-IDENTICAL to RAM:
    PL34 GFX1 1064736B @0x0c420040, GFX2 71872B @0x0c523f60  -> 0 diff
    PL17 GFX1  964736B @0x0c810040, GFX2 65856B @0x0c8fb8c0  -> 0 diff
So overlaying these at node+0x15C/0x160 = the engine's own art, exactly.

char_id -> PLxx MAPPING (CONFIRMED): char_id (a byte at char-struct +0x001) IS the
PLxx index in HEX. RAM dump P1C1 char_id=0x34 had gfx1=0x0c420040 == disc PL34. So
char_id 0x34 -> PL34, char_id 0x17 -> PL17, char_id 0x00 -> PL00 (Ryu). The roster is
the contiguous PL00..PL3A set the emitter baker already encodes (re_kb 05_characters).

SOURCE: dasm_PLDAT/Output/PL{HEX}_DAT/PL{HEX}_DAT_GFX_DATA_00.BIN (GFX1)
                                       /PL{HEX}_DAT_GFX_DATA_01.BIN (GFX2)
        (dasm_PLDAT_v005a.py already splits each PLDAT into its segments; the disc is
         the MVC2 Dev Files / dev box 65.109.77.178 set — see project_full_asset_dataset_local.)

OUTPUT (operator-local, ROM-derived, GITIGNORED — web/render-replica/gfx/):
  PL{NN}_gfx1.bin   raw GFX1 segment  (NN = upper-hex char_id == PLxx index)
  PL{NN}_gfx2.bin   raw GFX2 segment
  manifest.json     { "<char_id_dec>": { pl, gfx1, gfx2, gfx1_bytes, gfx2_bytes } }
                    so the client fetches by char_id (decimal, the struct +0x001 byte).

USAGE:
  # full roster:
  python3 tools/extract_replica_gfx.py --src dasm_PLDAT/Output --out web/render-replica/gfx
  # verify against a live RAM dump (proves byte-identity for the active chars):
  python3 tools/extract_replica_gfx.py --src dasm_PLDAT/Output --out web/render-replica/gfx \
          --verify-ram _ryu_capture/mc_ram_dump.bin
"""
import argparse, json, os, struct

# The contiguous MVC2 roster: char_id 0x00..0x3A == PL00..PL3A (hex). char_id is the
# +0x001 byte AND the PLxx index; PL00 = Ryu.
ROSTER = list(range(0x00, 0x3B))   # 0..58 inclusive (59 chars)

CHAR_STRUCT_SLOTS = {              # for --verify-ram: name -> base (P1 region 0x8C26..)
    "P1C1": 0x8C268340, "P2C1": 0x8C2688E4, "P1C2": 0x8C268E88,
    "P2C2": 0x8C26942C, "P1C3": 0x8C2699D0, "P2C3": 0x8C269F74,
}
OFF_ACTIVE, OFF_CID, OFF_GFX1, OFF_GFX2 = 0x000, 0x001, 0x15C, 0x160
RAM_MASK = 0x00FFFFFF


def seg_paths(src, cid):
    pl = f"PL{cid:02X}"
    d = os.path.join(src, f"{pl}_DAT")
    return pl, (os.path.join(d, f"{pl}_DAT_GFX_DATA_00.BIN"),
                os.path.join(d, f"{pl}_DAT_GFX_DATA_01.BIN"))


def verify_ram(ram_path, src):
    """Prove the staged segments are byte-identical to a live RAM dump for whatever
    chars are active in that dump. Reads each char struct's char_id + node+0x15C/0x160,
    windows the dump at those bases for the disc segment length, and byte-compares."""
    ram = open(ram_path, "rb").read()

    def u8(a):  return ram[a & RAM_MASK]
    def u32(a): return (ram[a & RAM_MASK] | (ram[(a+1) & RAM_MASK] << 8)
                        | (ram[(a+2) & RAM_MASK] << 16) | (ram[(a+3) & RAM_MASK] << 24)) >> 0

    print(f"[verify] {ram_path} ({len(ram)} bytes)")
    seen = set()
    allok = True
    for name, base in CHAR_STRUCT_SLOTS.items():
        active = u8(base + OFF_ACTIVE)
        cid    = u8(base + OFF_CID)
        g1b    = u32(base + OFF_GFX1)
        g2b    = u32(base + OFF_GFX2)
        if cid in seen:
            continue
        seen.add(cid)
        pl, (p1, p2) = seg_paths(src, cid)
        if not (os.path.exists(p1) and os.path.exists(p2)):
            print(f"  {name} active={active} char_id=0x{cid:02X} -> {pl}: NO DISC SEGMENT (skip)")
            continue
        d1 = open(p1, "rb").read(); d2 = open(p2, "rb").read()
        o1 = g1b & RAM_MASK; o2 = g2b & RAM_MASK
        r1 = ram[o1:o1 + len(d1)]; r2 = ram[o2:o2 + len(d2)]
        eq1 = (bytes(r1) == d1); eq2 = (bytes(r2) == d2)
        allok = allok and eq1 and eq2
        print(f"  {name} active={active} char_id=0x{cid:02X} {pl} "
              f"gfx1@0x{g1b:08x} {'OK' if eq1 else 'MISMATCH'} ({len(d1)}B) | "
              f"gfx2@0x{g2b:08x} {'OK' if eq2 else 'MISMATCH'} ({len(d2)}B)")
    print(f"[verify] {'ALL BYTE-IDENTICAL' if allok else 'MISMATCH DETECTED'}")
    return allok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="dasm_PLDAT/Output",
                    help="dasm_PLDAT Output dir (per-char PLxx_DAT/ with the split segments)")
    ap.add_argument("--out", default="web/render-replica/gfx",
                    help="web-servable output dir (gitignored)")
    ap.add_argument("--verify-ram", default=None,
                    help="a live 16MB RAM dump (mc_ram_dump.bin) to prove byte-identity")
    ap.add_argument("--chars", default=None,
                    help="comma list of char_id hex to stage (default: full roster)")
    args = ap.parse_args()

    if args.verify_ram:
        verify_ram(args.verify_ram, args.src)

    os.makedirs(args.out, exist_ok=True)
    roster = ([int(x, 16) for x in args.chars.split(",")] if args.chars else ROSTER)

    manifest = {}
    staged = missing = 0
    total_bytes = 0
    for cid in roster:
        pl, (p1, p2) = seg_paths(args.src, cid)
        if not (os.path.exists(p1) and os.path.exists(p2)):
            missing += 1
            print(f"  {pl}: MISSING disc segment(s) — skip")
            continue
        d1 = open(p1, "rb").read(); d2 = open(p2, "rb").read()
        o1 = os.path.join(args.out, f"PL{cid:02X}_gfx1.bin")
        o2 = os.path.join(args.out, f"PL{cid:02X}_gfx2.bin")
        open(o1, "wb").write(d1); open(o2, "wb").write(d2)
        manifest[str(cid)] = {                       # key = DECIMAL char_id (the +0x001 byte)
            "pl": pl,
            "gfx1": f"PL{cid:02X}_gfx1.bin", "gfx2": f"PL{cid:02X}_gfx2.bin",
            "gfx1_bytes": len(d1), "gfx2_bytes": len(d2),
        }
        staged += 1
        total_bytes += len(d1) + len(d2)

    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=0)

    print(f"[stage] {staged} chars staged, {missing} missing -> {args.out}")
    print(f"[stage] total {total_bytes/1e6:.1f} MB (avg {total_bytes/max(staged,1)/1e6:.2f} MB/char)")
    print(f"[stage] manifest.json: char_id(dec) -> {{pl,gfx1,gfx2,bytes}}")


if __name__ == "__main__":
    main()
