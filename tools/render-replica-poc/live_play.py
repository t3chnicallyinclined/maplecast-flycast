#!/usr/bin/env python3
"""LIVE PLAYABLE MvC2 — the executor IS the engine, no flycast. A persistent engine_loop.exe runs
the byte-exact tick + render_frame; this window feeds it keyboard input and renders its output with
the real sprite decoder. Move: WASD / arrows. Attacks: J=LP K=HP U=LK I=HK, Space=A1 L=A2, Enter=Start.

Run: python live_play.py [_ram_f90.bin]
"""
import subprocess, struct, sys, math
import tkinter as tk
from PIL import Image, ImageTk
import mvc2_decode as D

RAM = sys.argv[1] if len(sys.argv) > 1 else "_ram_f90.bin"
ram = bytearray(open(RAM, "rb").read())          # resident sprite data for decode_body (from the seed)
eng = subprocess.Popen(["./engine_loop.exe", RAM], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

# tk keysym -> CPS2 active-high bit
KEY = {'Up':0x2000,'w':0x2000, 'Down':0x1000,'s':0x1000, 'Left':0x0800,'a':0x0800, 'Right':0x0400,'d':0x0400,
       'j':0x0200,'k':0x0100,'u':0x0040,'i':0x0020, 'space':0x0080,'l':0x0010, 'Return':0x8000}
pressed = set()

root = tk.Tk(); root.title("MvC2 — transpiled SH-4 executor, NO flycast")
CW, CH = 720, 560
canvas = tk.Canvas(root, width=CW, height=CH, bg="#0c0e14", highlightthickness=0); canvas.pack()
hud = tk.Label(root, text="", fg="#8b96ac", bg="#0c0e14", font=("Consolas", 10)); hud.pack(fill="x")
root.configure(bg="#0c0e14")
root.bind('<KeyPress>',   lambda e: pressed.add(e.keysym))
root.bind('<KeyRelease>', lambda e: pressed.discard(e.keysym))

QFMT = "<5I10f6If"; cache = {}
def tile(qd, sd, cr, ie):
    key = (qd['gfx1'], qd['sel'], (qd['tcw']>>21)&0x3F, tuple(sd), tuple(cr), qd['mirror'])
    if key in cache: return cache[key]
    r = D.decode_body(qd, sd, ie, ram, cr); img = None
    if r is not None and any(r[0][3::4]):
        img = Image.frombytes("RGBA", (r[1], r[2]), bytes(r[0])).transpose(Image.FLIP_TOP_BOTTOM)
        if qd['mirror']: img = img.transpose(Image.FLIP_LEFT_RIGHT)
    cache[key] = img; return img

def tx(x): return int((x - 150) * 1.05)          # fixed view fit (coords ~200..680 x, 160..520 y)
def ty(y): return int((y - 135) * 1.05)

state = {'img': None, 'fps_t': 0, 'frames': 0}
import time
def frame():
    cur = 0
    for ks in pressed: cur |= KEY.get(ks, 0)
    try:
        eng.stdin.write(struct.pack('<H', cur)); eng.stdin.flush()
        n = struct.unpack('<I', eng.stdout.read(4))[0]
        quads = eng.stdout.read(n*88); sd = eng.stdout.read(n*4); crb = eng.stdout.read(n*8); ie = eng.stdout.read(n)
    except Exception:
        root.destroy(); return
    img = Image.new("RGBA", (CW, CH), (12, 14, 20, 255))
    items = []
    for i in range(n):
        q = struct.unpack_from(QFMT, quads, i*88)
        pts = [(q[5],q[6]),(q[7],q[8]),(q[9],q[10]),(q[11],q[12])]
        if not all(math.isfinite(p[0]) and math.isfinite(p[1]) for p in pts): continue
        qd = dict(tcw=q[3], sel=q[15], gfx1=q[16], mirror=q[17], u1=q[13], tsp=q[2])
        items.append((q[21], pts, qd, list(sd[i*4:i*4+4]), list(struct.unpack_from('<2i', crb, i*8)), ie[i]))
    drew = 0
    for z, pts, qd, s, c, e in sorted(items, key=lambda t: t[0]):
        t = tile(qd, s, c, e)
        if t is None: continue
        sp = [(tx(x), ty(y)) for x, y in pts]
        x0 = min(p[0] for p in sp); y0 = min(p[1] for p in sp)
        x1 = max(p[0] for p in sp); y1 = max(p[1] for p in sp)
        img.alpha_composite(t.resize((max(1,x1-x0), max(1,y1-y0)), Image.NEAREST), (x0, y0)); drew += 1
    state['img'] = ImageTk.PhotoImage(img.convert("RGB"))
    canvas.create_image(0, 0, anchor="nw", image=state['img'])
    state['frames'] += 1; now = time.time()
    if now - state['fps_t'] >= 0.5:
        fps = state['frames'] / (now - state['fps_t']); state['frames'] = 0; state['fps_t'] = now
        hud.config(text=f"  executor engine (no flycast) — {drew} tiles — {fps:.0f} fps      WASD move · JKUI attack · Space/L assist")
    root.after(16, frame)

state['fps_t'] = time.time()
frame()
root.mainloop()
