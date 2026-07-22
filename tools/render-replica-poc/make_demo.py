#!/usr/bin/env python3
"""Rasterize the PURE-LOCAL executor drive into a visible PNG/MP4 sequence.

The thin SH-4 game-tick executor (gen_tick_all.c) advances a 16 MB MvC2 RAM image one
frame at a time with NO flycast and NO server; render_frame (transpiled MvC2 render walk)
turns each driven frame into sprite-tile quads. We have geometry but no textures offline,
so each tile is filled flat, colored by its owning body (gfx1) — you see the two fighters
as animating silhouettes of correctly-placed sprite tiles, plus a live HUD (health, chars,
frame) read from the byte-exact game-tick state. This is the executor BEING the game.

Inputs (from chain_drive.exe): _sc/scene_%03d.bin (quads) + _st/state_%03d.bin (game state).
Usage: python make_demo.py [scdir=_sc] [stdir=_st] [outdir=_demo]
"""
import struct, glob, os, sys, math
from PIL import Image, ImageDraw, ImageFont
import mvc2_state as S

SCDIR = sys.argv[1] if len(sys.argv) > 1 else "_sc"
STDIR = sys.argv[2] if len(sys.argv) > 2 else "_st"
OUT   = sys.argv[3] if len(sys.argv) > 3 else "_demo"
os.makedirs(OUT, exist_ok=True)

QFMT = "<5I10f6If"; QSZ = struct.calcsize(QFMT)   # SceneQuad, 88 bytes

# MvC2 character id -> name (partial; unknown ids show the hex)
CHARS = {0x00:"Ryu",0x01:"Zangief",0x02:"Morrigan",0x0d:"Cable",0x17:"Sentinel",
         0x2c:"Cyclops",0x1a:"Magneto",0x1b:"Psylocke",0x20:"Storm",0x0a:"Wolverine",
         0x33:"Iron Man",0x2f:"Spiral",0x3a:"Cammy",0x1f:"Rogue"}

def load_quads(fn):
    d = open(fn, "rb").read()
    out = []
    for i in range(len(d)//QSZ):
        v = struct.unpack_from(QFMT, d, i*QSZ)
        ax,ay,bx,by,cx,cy,dx,dy = v[5:13]
        gfx1 = v[16]; z = v[21]
        pts = [(ax,ay),(bx,by),(cx,cy),(dx,dy)]
        if all(math.isfinite(p[0]) and math.isfinite(p[1]) for p in pts):
            out.append((pts, gfx1, z))
    return out

scenes = [load_quads(f) for f in sorted(glob.glob(os.path.join(SCDIR, "scene_*.bin")))]
states = S.load_frames(STDIR) if os.path.isdir(STDIR) else []
N = len(scenes)
print(f"{N} scene frames, {len(states)} state frames")

# global bbox of all finite quad points -> fit into the canvas
xs = [p[0] for sc in scenes for (pts,_,_) in sc for p in pts]
ys = [p[1] for sc in scenes for (pts,_,_) in sc for p in pts]
minx,maxx,miny,maxy = min(xs),max(xs),min(ys),max(ys)
CW,CH = 960,720
pad = 70
sx = (CW-2*pad)/(maxx-minx); sy = (CH-2*pad-90)/(maxy-miny); s = min(sx,sy)
def tx(x): return pad + (x-minx)*s
def ty(y): return 90 + pad + (y-miny)*s   # +90 leaves a HUD band on top

# distinct owning bodies -> vivid colors (P-side inferred by x-centroid)
gset = sorted({g for sc in scenes for (_,g,_) in sc})
PAL = [(80,200,255),(255,120,90),(150,255,140),(255,210,80),(220,140,255),(255,255,255)]
gcol = {g: PAL[i % len(PAL)] for i,g in enumerate(gset)}

try: font = ImageFont.truetype("arialbd.ttf", 22); fsm = ImageFont.truetype("arial.ttf", 15)
except: font = ImageFont.load_default(); fsm = font

BG = (14,16,22)
for fi in range(N):
    img = Image.new("RGB", (CW,CH), BG)
    ov  = Image.new("RGBA",(CW,CH),(0,0,0,0))
    dr  = ImageDraw.Draw(ov, "RGBA")
    # floor line
    dr.line([(pad,ty(maxy)),(CW-pad,ty(maxy))], fill=(60,66,80,255), width=2)
    # quads: painter order = farther (smaller z=1/w) first
    for pts,g,z in sorted(scenes[fi], key=lambda q:q[2]):
        poly = [(tx(x),ty(y)) for x,y in pts]
        r,gc,b = gcol[g]
        dr.polygon(poly, fill=(r,gc,b,70), outline=(r,gc,b,180))
    base = Image.alpha_composite(img.convert("RGBA"), ov).convert("RGB")
    dr2 = ImageDraw.Draw(base)
    # HUD — fighting-game layout: P1 bar top-left, P2 bar top-right, title centered below.
    P1COL, P2COL = (255,120,90), (80,200,255)
    if fi < len(states):
        st = states[fi]
        p1 = [(n,b) for n,b in S.active_slots(st) if n.startswith("P1")]
        p2 = [(n,b) for n,b in S.active_slots(st) if n.startswith("P2")]
        BW = 340; BH = 18
        def bar(x0, y0, c, col, right=False):
            hp=max(0,min(144,c['hp'])); red=max(0,min(144,c['red']))
            dr2.rectangle([x0,y0,x0+BW,y0+BH], fill=(30,32,40), outline=(90,96,110), width=1)
            if right:
                dr2.rectangle([x0+BW-int(BW*red/144),y0,x0+BW,y0+BH], fill=(120,40,40))
                dr2.rectangle([x0+BW-int(BW*hp/144),y0,x0+BW,y0+BH], fill=col)
            else:
                dr2.rectangle([x0,y0,x0+int(BW*red/144),y0+BH], fill=(120,40,40))
                dr2.rectangle([x0,y0,x0+int(BW*hp/144),y0+BH], fill=col)
        if p1:
            c=S.char(st,p1[0][1]); nm=CHARS.get(c['cid'],f"char{c['cid']:02x}")
            bar(pad,18,c,P1COL,right=False); dr2.text((pad,40),f"P1  {nm}",font=fsm,fill=P1COL)
        if p2:
            c=S.char(st,p2[0][1]); nm=CHARS.get(c['cid'],f"char{c['cid']:02x}")
            x0=CW-pad-BW; bar(x0,18,c,P2COL,right=True)
            w=dr2.textlength(f"{nm}  P2",font=fsm); dr2.text((CW-pad-w,40),f"{nm}  P2",font=fsm,fill=P2COL)
    # centered title band
    t1="PURE-LOCAL MvC2  —  transpiled SH-4 executor"
    t2=f"no flycast · no server · render_frame over the driven RAM     frame {fi:03d}/{N-1}   {len(scenes[fi])} quads"
    w1=dr2.textlength(t1,font=font); dr2.text(((CW-w1)/2,62),t1,font=font,fill=(235,240,255))
    w2=dr2.textlength(t2,font=fsm);  dr2.text(((CW-w2)/2,88),t2,font=fsm,fill=(150,170,200))
    base.save(os.path.join(OUT, f"f{fi:03d}.png"))

print(f"wrote {N} frames -> {OUT}/f*.png")
