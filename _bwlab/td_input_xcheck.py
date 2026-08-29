#!/usr/bin/env python3
"""TDW2 floor study — input-entropy floor (from .mcrec) + high-variance byte cross-check vs RE map."""
import numpy as np, struct, glob, zlib, math, os

# ---------- (1) INPUT ENTROPY FLOOR from a real match .mcrec ----------
rec=sorted(glob.glob('recordings/*.mcrec'))
# pick a mid-sized one
rec=[r for r in rec if os.path.getsize(r)>4_000_000]
path=rec[len(rec)//2]
b=open(path,'rb').read()
assert b[:5]==b'MCREC'
# header layout (replay_writer.cpp start()): 8 magic +4 ver +4 flycast +16 match +16 srv
# +8 start +8 dur +32 rom +64 p1 +64 p2 +3 p1c +3 p2c +1 win +40 reserved = 271
off=271
raw_size=struct.unpack_from('<Q',b,off)[0]; off+=8
off+=raw_size            # embedded savestate
off+=4                   # spg_jitter
# input log: 16B entries [frame u64][seqAndSlot u32][buttons u16][lt u8][rt u8] until "MCEND"
end=b.find(b'MCEND',off)
log=b[off:end]
n=len(log)//16
E=np.frombuffer(log[:n*16],dtype=np.uint8).reshape(n,16)
frames=E[:, :8].copy().view(np.uint64).reshape(n)
seqslot=E[:,8:12].copy().view(np.uint32).reshape(n)
buttons=E[:,12:14].copy().view(np.uint16).reshape(n)
slot=(seqslot & 0xFF)
print(f"=== INPUT ENTROPY FLOOR  ({os.path.basename(path)}) ===")
print(f"  {n} input entries, savestate={raw_size} bytes, frame span {int(frames.min())}..{int(frames.max())}")
# collapse to per-frame per-slot latched input (last write wins per (frame,slot))
uslots=np.unique(slot)
for sl in uslots:
    m=slot==sl
    fr=frames[m]; bt=buttons[m]; lt=E[m,14]; rt=E[m,15]
    nf=int(fr.max()-fr.min()+1) if len(fr) else 0
    # raw per-frame stream = buttons(2)+lt(1)+rt(1) = 4 B/frame
    raw_bpf=4.0
    # entropy of the button stream: order-0 over 4-byte words + delta stream
    stream=np.stack([bt.view(np.uint8)[0::1] if False else (bt&0xFF).astype(np.uint8),
                     (bt>>8).astype(np.uint8), lt, rt],1).tobytes()
    comp=len(zlib.compress(stream,9))
    # button-change events: frames where buttons differ from previous
    chg=int((np.diff(bt.astype(np.int32))!=0).sum())
    print(f"  slot {sl}: {len(fr)} samples over ~{nf} frames | raw 4B/f | "
          f"zlib(button+analog stream)={comp} B total = {comp/max(1,len(fr)):.3f} B/frame | "
          f"button-change frames={chg} ({100*chg/max(1,len(bt)-1):.1f}%)")
print("  -> MVC2 latches input once/frame (re_kb/75); the irreducible per-player wire is the")
print("     button bitfield delta: a few bits most frames, ~0.1-0.5 B/frame compressed.")

# ---------- (2) HIGH-VARIANCE BYTE CROSS-CHECK vs RE'd offsets ----------
print("\n=== HIGH-VARIANCE CHAR-STRUCT BYTE COLUMNS vs RE'd MEMORY MAP ===")
C=np.load('_bwlab/_td_fx5_charst.npy')
F=C.shape[0]; STRIDE=0x5A4
SLOTS=['P1C1','P2C1','P1C2','P2C2','P1C3','P2C3']
# known field offsets -> name (from CLAUDE.md + pl_mem.asm)
known={0x00:'active',0x01:'character_id',0x34:'pos_x',0x35:'pos_x',0x36:'pos_x',0x37:'pos_x',
       0x38:'pos_y',0x39:'pos_y',0x3a:'pos_y',0x3b:'pos_y',0x50:'xscale',0x51:'xscale',0x52:'xscale',0x53:'xscale',
       0x54:'yscale',0x55:'yscale',0x56:'yscale',0x57:'yscale',
       0xE0:'screen_x',0xE1:'screen_x',0xE2:'screen_x',0xE3:'screen_x',0xE4:'screen_y',0xE5:'screen_y',0xE6:'screen_y',0xE7:'screen_y',
       0x110:'facing',0x142:'anim_timer',0x143:'anim_timer',0x144:'sprite_id',0x145:'sprite_id',
       0x1D0:'animation_state',0x1D1:'animation_state',0x420:'health',0x424:'red_health',0x52D:'palette',
       0x12e:'hit_flash',0x151:'RenderExtra',0x25:'pl_palid',0xDC:'render_accum',0xDD:'render_accum',0xDE:'render_accum',0xDF:'render_accum'}
diffcount=(C[1:]!=C[:-1]).sum(0)          # per byte-col, #frames it changed
# focus on slot 1 (P2C1, active). Report the changing offsets within a struct, mapped.
for s in [0,1]:
    base=s*STRIDE
    off_changed=[(o, int(diffcount[base+o])) for o in range(STRIDE) if diffcount[base+o]>0]
    off_changed.sort(key=lambda x:-x[1])
    print(f"\n {SLOTS[s]}: {len(off_changed)} changing byte-offsets. Top 30 by change-frequency:")
    unmapped=[]
    for o,c in off_changed[:30]:
        nm=known.get(o,'??')
        if nm=='??': unmapped.append(o)
        print(f"   +0x{o:03X} changed {c:4d}/{F-1} frames ({100*c/(F-1):5.1f}%)  {nm}")
    allun=sorted(set(o for o,_ in off_changed if o not in known))
    print(f"   UNMAPPED changing offsets ({len(allun)}): "+", ".join(f"0x{o:03X}" for o in allun[:40]))
