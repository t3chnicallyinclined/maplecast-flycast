#!/usr/bin/env python3
"""v4_to_gstarec.py <in.v3> <out.gstarec> — render from WORLD coords (unimpeachable, shared
frame of reference) with a simple centered camera. Exactly ONE fighter per side: the non-parked
(|wx| < 1300), alive slot, preferring the one flagged active (+0x004). Camera = midpoint of the
two, so relative spacing is exact by construction. screen_y = 434 - world_y (ground line proven).
"""
import sys, struct

SR = 25
PARK = 1300.0          # benched partners park at |world_x| ~= 1386.7
GROUND_SY = 434.0      # proven: grounded fighters read screen_y 434 (emulator ground 433.4)

raw = open(sys.argv[1], "rb").read()
assert raw[:8] == b"V3SYNC02", raw[:8]
frames = []
off = 8
while off + 4 + 6 * SR <= len(raw):
    fc = struct.unpack_from("<I", raw, off)[0]; off += 4
    slots = []
    for i in range(6):
        act, cid, sid, hp, red, fac, wx, wy, sx, sy = struct.unpack_from("<BBHHHBffff", raw, off)
        off += SR
        slots.append(dict(act=act, cid=cid, sid=sid, hp=hp, red=red, fac=fac, wx=wx, wy=wy))
    frames.append((fc, slots))
print(f"{len(frames)} frames", file=sys.stderr)

def pick_side(slots, parity):
    """the fighting character on one side: alive, not parked; prefer the active-flagged one."""
    cands = [i for i in range(parity, 6, 2)
             if slots[i]["hp"] > 0 and abs(slots[i]["wx"]) < PARK and not (slots[i]["wx"] == 0.0 and slots[i]["wy"] == 0.0)]
    if not cands:
        return None
    act = [i for i in cands if slots[i]["act"] == 1]
    return (act or cands)[0]

out = open(sys.argv[2], "wb")
out.write(b"GSTAREC1")
fc0 = frames[0][0] if frames else 0
written = 0
both = 0
for fc, slots in frames:
    a = pick_side(slots, 0)
    b = pick_side(slots, 1)
    live = [i for i in (a, b) if i is not None]
    if not live:
        continue
    if len(live) == 2:
        both += 1
    cam = sum(slots[i]["wx"] for i in live) / len(live)      # centered on the fight
    pay = bytearray(380)
    pay[0:4] = b"GSTA"; pay[4] = 1
    struct.pack_into("<I", pay, 25, fc - fc0)
    for i in range(6):
        s = slots[i]; o = 29 + i * 57
        on = 1 if i in live else 0
        pay[o + 0] = on
        pay[o + 1] = s["cid"] & 0xFF
        pay[o + 2] = 1 if s["fac"] else 0
        pay[o + 3] = min(s["hp"], 144)
        pay[o + 4] = min(s["red"], 144)
        struct.pack_into("<f", pay, o + 8, s["wx"])
        struct.pack_into("<f", pay, o + 16, 320.0 + (s["wx"] - cam))   # world delta == screen px (1:1)
        struct.pack_into("<f", pay, o + 20, GROUND_SY - s["wy"])
        struct.pack_into("<H", pay, o + 32, s["sid"] & 0x7FFF)
        struct.pack_into("<f", pay, o + 38, 1.0); struct.pack_into("<f", pay, o + 42, 1.0)
        pay[o + 49] = 3 if on else 0xFF
    out.write(struct.pack("<dBI", (fc - fc0) / 60.0, 2, len(pay)))
    out.write(pay)
    written += 1
out.close()
print(f"wrote {written} frames ({both} with BOTH fighters) -> {sys.argv[2]}", file=sys.stderr)
