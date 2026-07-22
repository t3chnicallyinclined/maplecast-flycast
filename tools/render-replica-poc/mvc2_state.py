"""Parse the per-frame game-state dumps chain_drive.exe writes with CHDIR=dir.
Each state_%03d.bin = ram[0x268000 .. 0x28A000] (0x22000 bytes): the 6 char structs
(page 616 @0x268340, stride 0x5A4) + the global page (@0x289000). Byte-exact game-tick
state from the pure-local executor drive.
"""
import struct, glob, os

BASE = 0x268000
SLOTS = {  # name -> struct base (CLAUDE.md MVC2 memory map)
    'P1C1': 0x268340, 'P2C1': 0x2688E4, 'P1C2': 0x268E88,
    'P2C2': 0x26942C, 'P1C3': 0x2699D0, 'P2C3': 0x269F74,
}
# per-char offsets
O_ACTIVE, O_CID, O_POSX, O_POSY = 0x00, 0x01, 0x34, 0x38
O_SCRX, O_SCRY, O_FACING = 0xE0, 0xE4, 0x110
O_ANIMTMR, O_SPRITE, O_ANIMSTATE = 0x142, 0x144, 0x1D0
O_HEALTH, O_REDHP, O_PALETTE = 0x420, 0x424, 0x52D
# globals (absolute addrs)
G_SUBSTATE, G_INMATCH, G_ROUND, G_TIMER, G_STAGE = 0x289621, 0x289624, 0x28962B, 0x289630, 0x289638

def _u(d, addr, off, fmt):
    return struct.unpack_from(fmt, d, (addr - BASE) + off)[0]

def char(d, base):
    return dict(
        active=_u(d, base, O_ACTIVE, '<B'), cid=_u(d, base, O_CID, '<B'),
        x=_u(d, base, O_POSX, '<f'), y=_u(d, base, O_POSY, '<f'),
        facing=_u(d, base, O_FACING, '<B'),
        anim_tmr=_u(d, base, O_ANIMTMR, '<H'), sprite=_u(d, base, O_SPRITE, '<H'),
        anim=_u(d, base, O_ANIMSTATE, '<H'),
        hp=_u(d, base, O_HEALTH, '<B'), red=_u(d, base, O_REDHP, '<B'),
        pal=_u(d, base, O_PALETTE, '<B'),
    )

def globals_(d):
    return dict(
        sub=_u(d, G_SUBSTATE, 0, '<B'), in_match=_u(d, G_INMATCH, 0, '<B'),
        round=_u(d, G_ROUND, 0, '<B'), timer=_u(d, G_TIMER, 0, '<B'),
        stage=_u(d, G_STAGE, 0, '<B'),
    )

def load_frames(scdir):
    files = sorted(glob.glob(os.path.join(scdir, "state_*.bin")))
    return [open(f, "rb").read() for f in files]

def active_slots(d):
    return [(n, b) for n, b in SLOTS.items() if char(d, b)['active']]

if __name__ == "__main__":
    import sys
    frames = load_frames(sys.argv[1] if len(sys.argv) > 1 else "_st")
    if not frames:
        print("no frames"); raise SystemExit
    d0 = frames[0]
    print(f"{len(frames)} frames | globals f0: {globals_(d0)}")
    act = active_slots(d0)
    print("active:", [f"{n}(cid=0x{char(d0,b)['cid']:02x})" for n, b in act])
    # motion summary
    for n, b in act:
        xs = [char(d, b)['x'] for d in frames]
        ys = [char(d, b)['y'] for d in frames]
        hps = [char(d, b)['hp'] for d in frames]
        sids = set(char(d, b)['sprite'] for d in frames)
        print(f"  {n}: x[{min(xs):.1f},{max(xs):.1f}] y[{min(ys):.1f},{max(ys):.1f}] "
              f"hp[{min(hps)},{max(hps)}] sprites={len(sids)}")
    tmrs = set(globals_(d)['timer'] for d in frames)
    print(f"  timer values seen: {sorted(tmrs)}")
