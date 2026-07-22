#!/usr/bin/env python3
"""Textured pure-local demo: composite REAL MvC2 sprite tiles (decoded straight from the RAM
image via the decode_body port) onto each frame of the executor drive. Geometry comes from
chain_drive's render_frame (SCDIR dump, PROJCARRY); textures + palette from mvc2_decode.

Usage: python make_demo_tex.py <ram.bin=_ram_f90.bin> [scdir=_sc] [stdir=_st] [outdir=_demotex]
"""
import struct, glob, os, sys, math
from PIL import Image, ImageDraw, ImageFont
import mvc2_decode as D
import mvc2_state as S

RAM   = sys.argv[1] if len(sys.argv) > 1 else "_ram_f90.bin"
SCDIR = sys.argv[2] if len(sys.argv) > 2 else "_sc"
STDIR = sys.argv[3] if len(sys.argv) > 3 else "_st"
OUT   = sys.argv[4] if len(sys.argv) > 4 else "_demotex"
os.makedirs(OUT, exist_ok=True)
ram = bytearray(open(RAM, "rb").read())
QFMT = "<5I10f6If"

# map each rendered body's GFX1 (node+0x15C) -> (cid, health-slot base) so the HUD labels the
# bar under each on-screen character correctly (P1/P2 side ≠ screen side).
def _r32(a):
    a &= 0xFFFFFF
    return ram[a]|ram[a+1]<<8|ram[a+2]<<16|ram[a+3]<<24 if a+4<=len(ram) else 0
GFX1_SLOT = {}   # gfx1 base -> char-struct base
for _b in S.SLOTS.values():
    g = _r32(_b + 0x15C)
    if g:
        GFX1_SLOT[g] = _b

# authoritative char_id -> name (tools/find_rom_in_mem.py STEAM_PALETTE_OFFSETS)
CHARS = {0x04:"Anakaris",0x05:"Strider",0x06:"Cyclops",0x07:"Wolverine",0x08:"Psylocke",
         0x09:"Iceman",0x0a:"Rogue",0x0b:"Captain America",0x0c:"Spider-Man",0x0d:"Hulk",
         0x0e:"Venom",0x0f:"Doctor Doom",0x10:"Tron Bonne",0x11:"Jill",0x12:"Hayato",
         0x13:"Ruby Heart",0x14:"SonSon",0x15:"Amingo",0x16:"Marrow",0x17:"Cable",
         0x1b:"Chun-Li",0x1c:"Mega Man",0x1d:"Roll",0x1e:"Akuma",0x1f:"BB Hood",
         0x20:"Felicia",0x21:"Charlie",0x22:"Sakura",0x23:"Dan",0x24:"Cammy",0x25:"Dhalsim",
         0x26:"M.Bison",0x27:"Ken",0x28:"Gambit",0x29:"Juggernaut",0x2a:"Storm",
         0x2b:"Sabretooth",0x2c:"Magneto",0x2d:"Shuma-Gorath",0x2e:"War Machine",
         0x2f:"Silver Samurai",0x30:"Omega Red",0x31:"Spiral",0x32:"Colossus",
         0x33:"Iron Man",0x34:"Sentinel",0x35:"Blackheart",0x36:"Thanos",0x37:"Jin",0x38:"Captain Commando"}
# per-body flat fallback colors (when a quad's palette isn't base-resident: effects/hit-flash)
FLATPAL=[(80,120,200),(200,120,80),(120,200,140),(200,200,90),(180,120,200)]

def load_scene(fn):
    d = open(fn, "rb").read()
    n = struct.unpack_from("<I", d, 0)[0]
    qb=4; sdb=qb+n*88; crb=sdb+n*4; ieb=crb+n*8
    out=[]
    for i in range(n):
        q=struct.unpack_from(QFMT,d,qb+i*88)
        pts=[(q[5],q[6]),(q[7],q[8]),(q[9],q[10]),(q[11],q[12])]
        if not all(math.isfinite(p[0]) and math.isfinite(p[1]) for p in pts): continue
        qd=dict(pcw=q[0],tcw=q[3],sel=q[15],gfx1=q[16],mirror=q[17],mirror_v=q[18],u1=q[13],tsp=q[2],z=q[21])
        sd=list(d[sdb+i*4:sdb+i*4+4]); cr=list(struct.unpack_from("<2i",d,crb+i*8)); ie=d[ieb+i]
        out.append((pts,qd,sd,cr,ie))
    return out

scenes=[load_scene(f) for f in sorted(glob.glob(os.path.join(SCDIR,"scene_*.bin")))]
states=S.load_frames(STDIR) if os.path.isdir(STDIR) else []
N=len(scenes)
print(f"{N} frames")

# auto-fit transform over all finite quad points
xs=[p[0] for sc in scenes for (pts,_,_,_,_) in sc for p in pts]
ys=[p[1] for sc in scenes for (pts,_,_,_,_) in sc for p in pts]
minx,maxx,miny,maxy=min(xs),max(xs),min(ys),max(ys)
CW,CH=960,720; pad=70; topband=104
s=min((CW-2*pad)/(maxx-minx),(CH-topband-2*pad)/(maxy-miny))
def tx(x): return pad+(x-minx)*s
def ty(y): return topband+pad+(y-miny)*s

# decode cache: key (gfx1,sel,palsel,srcdesc,colrow,mirror) -> PIL RGBA tile
cache={}
def tile_for(qd,sd,cr,ie):
    key=(qd['gfx1'],qd['sel'],(qd['tcw']>>21)&0x3F,tuple(sd),tuple(cr),qd['mirror'],qd['mirror_v'])
    if key in cache: return cache[key]
    r=D.decode_body(qd,sd,ie,ram,cr)
    img=None
    if r is not None:
        rgba,w,h=r
        if any(rgba[3::4]):
            img=Image.frombytes("RGBA",(w,h),bytes(rgba))
            if qd['mirror']: img=img.transpose(Image.FLIP_LEFT_RIGHT)
            if qd['mirror_v']: img=img.transpose(Image.FLIP_TOP_BOTTOM)
    cache[key]=img
    return img

try: font=ImageFont.truetype("arialbd.ttf",22); fsm=ImageFont.truetype("arial.ttf",15)
except: font=fsm=ImageFont.load_default()
BG=(12,14,20)

for fi in range(N):
    img=Image.new("RGBA",(CW,CH),BG+(255,))
    dr=ImageDraw.Draw(img)
    dr.line([(pad,ty(maxy)),(CW-pad,ty(maxy))],fill=(56,62,78,255),width=2)
    drew=0
    for pts,qd,sd,cr,ie in sorted(scenes[fi],key=lambda t:t[1]['z']):
        t=tile_for(qd,sd,cr,ie)
        if t is None: continue
        sp=[(tx(x),ty(y)) for x,y in pts]
        bx0=int(min(p[0] for p in sp)); by0=int(min(p[1] for p in sp))
        bx1=int(max(p[0] for p in sp)); by1=int(max(p[1] for p in sp))
        w=max(1,bx1-bx0); h=max(1,by1-by0)
        img.alpha_composite(t.resize((w,h),Image.NEAREST),(bx0,by0)); drew+=1
    base=img.convert("RGB"); dr2=ImageDraw.Draw(base)
    # HUD — one bar per on-screen body, placed on the SIDE that body renders (via gfx1->slot).
    LCOL,RCOL=(255,120,90),(80,200,255)
    # centroid screen-x per rendered gfx1 -> left/right ordering
    cent={}
    for pts,qd,_,_,_ in scenes[fi]:
        cx=sum(p[0] for p in pts)/4.0
        cent.setdefault(qd['gfx1'],[]).append(cx)
    bodies=sorted(((sum(v)/len(v),g) for g,v in cent.items() if g in GFX1_SLOT))
    BW,BH=340,18
    def bar(x0,c,col,right):
        hp=max(0,min(144,c['hp'])); red=max(0,min(144,c['red']))
        dr2.rectangle([x0,18,x0+BW,18+BH],fill=(28,30,38),outline=(88,94,108),width=1)
        if right:
            dr2.rectangle([x0+BW-int(BW*red/144),18,x0+BW,18+BH],fill=(120,40,40))
            dr2.rectangle([x0+BW-int(BW*hp/144),18,x0+BW,18+BH],fill=col)
        else:
            dr2.rectangle([x0,18,x0+int(BW*red/144),18+BH],fill=(120,40,40))
            dr2.rectangle([x0,18,x0+int(BW*hp/144),18+BH],fill=col)
    if fi<len(states):
        st=states[fi]
        for idx,(_,g) in enumerate(bodies[:2]):
            b=GFX1_SLOT[g]; c=S.char(st,b); nm=CHARS.get(c['cid'],f"char{c['cid']:02x}")
            on_left = (idx==0)
            col = LCOL if on_left else RCOL
            if on_left:
                bar(pad,c,col,False); dr2.text((pad,40),nm,font=fsm,fill=col)
            else:
                x0=CW-pad-BW; bar(x0,c,col,True)
                w=dr2.textlength(nm,font=fsm); dr2.text((CW-pad-w,40),nm,font=fsm,fill=col)
    t1="PURE-LOCAL MvC2  —  transpiled SH-4 executor + real sprites"
    t2=f"no flycast · no server · textures decoded from RAM     frame {fi:03d}/{N-1}   {drew} tiles"
    w1=dr2.textlength(t1,font=font); dr2.text(((CW-w1)/2,62),t1,font=font,fill=(235,240,255))
    w2=dr2.textlength(t2,font=fsm);  dr2.text(((CW-w2)/2,88),t2,font=fsm,fill=(150,170,200))
    base.save(os.path.join(OUT,f"f{fi:03d}.png"))
    if fi%20==0: print(f"  f{fi}: {drew} tiles")
print(f"wrote {N} frames -> {OUT} (cache {len(cache)} tiles)")
