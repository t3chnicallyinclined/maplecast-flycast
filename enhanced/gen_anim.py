#!/usr/bin/env python3
"""Pencil-restyle an animation sequence via fal.ai, assemble GIF + filmstrip.
Env: FAL_KEY MODEL PROMPT FRAMES(glob) OUT TARGET SEED STEPS"""
import os, sys, time, json, base64, io, glob, urllib.request, urllib.error
from PIL import Image
FAL_KEY = os.environ["FAL_KEY"].strip()
MODEL   = os.environ.get("MODEL", "fal-ai/flux-kontext")
PROMPT  = os.environ["PROMPT"]
FRAMES  = sorted(glob.glob(os.environ["FRAMES"]))
OUT     = os.environ.get("OUT", "enhanced/out/anim")
TARGET  = int(os.environ.get("TARGET", "768"))
SEED    = int(os.environ.get("SEED", "42"))
STEPS   = int(os.environ.get("STEPS", "30"))
GUIDANCE= float(os.environ.get("GUIDANCE", "3.5"))
os.makedirs(OUT, exist_ok=True)
ENDPOINT = f"https://queue.fal.run/{MODEL}"

def _req(url, data=None):
    h = {"Authorization": f"Key {FAL_KEY}"}
    if data is not None: h["Content-Type"]="application/json"; data=json.dumps(data).encode()
    return urllib.request.urlopen(urllib.request.Request(url, data=data, headers=h,
                                  method="POST" if data is not None else "GET"), timeout=120)
def find_img(o):
    if isinstance(o, dict):
        for k in ("images","image"):
            if k in o:
                v=o[k]; v=v[0] if isinstance(v,list) and v else v
                if isinstance(v,dict) and "url" in v: return v["url"]
                if isinstance(v,str): return v
        for v in o.values():
            u=find_img(v)
            if u: return u
    return None

loaded=[]
for f in FRAMES:
    try:
        im=Image.open(f).convert("RGBA")
        if im.getbbox(): loaded.append((f, im))
    except Exception as e: print("skip", f, e)
if not loaded: print("no usable frames"); sys.exit(1)
bbs=[im.getbbox() for _,im in loaded]
U=(min(b[0] for b in bbs), min(b[1] for b in bbs), max(b[2] for b in bbs), max(b[3] for b in bbs))
print(f"{len(loaded)} frames | union bbox {U}")

outs=[]
for idx,(f,im) in enumerate(loaded):
    crop=im.crop(U)
    white=Image.new("RGBA",crop.size,(255,255,255,255)); white.alpha_composite(crop); rgb=white.convert("RGB")
    sc=max(1, -(-TARGET//min(rgb.size))); rgb=rgb.resize((rgb.size[0]*sc, rgb.size[1]*sc), Image.NEAREST)
    buf=io.BytesIO(); rgb.save(buf,"PNG")
    payload={"image_url":"data:image/png;base64,"+base64.b64encode(buf.getvalue()).decode(),
             "prompt":PROMPT,"num_inference_steps":STEPS,"guidance_scale":GUIDANCE,"seed":SEED,"enable_safety_checker":False}
    print(f"[{idx+1}/{len(loaded)}] {os.path.basename(f)} …", flush=True)
    iu=None
    for attempt in range(3):
        try:
            j=json.loads(_req(ENDPOINT,payload).read())
            iu=find_img(j); su=j.get("status_url"); ru=j.get("response_url")
            if not iu and su:
                for _ in range(90):
                    time.sleep(2); st=json.loads(_req(su).read()); s=str(st.get("status")).upper()
                    if s in ("COMPLETED","SUCCEEDED","OK"): iu=find_img(json.loads(_req(ru or su).read())); break
                    if s in ("FAILED","ERROR"): print("  failed", st); break
            if iu: break
        except Exception as e: print(f"  attempt {attempt+1}: {repr(e)[:110]}", flush=True); time.sleep(3)
    if not iu: print("  no image (gave up)"); continue
    res=Image.open(io.BytesIO(_req(iu).read())).convert("RGB")
    res.save(f"{OUT}/f{idx:02d}.png"); outs.append(res); print("  ok", res.size, flush=True)

if not outs: print("no outputs"); sys.exit(1)
W,H=outs[0].size; norm=[o.resize((W,H)) for o in outs]
norm[0].save(f"{OUT}/anim.gif", save_all=True, append_images=norm[1:], duration=130, loop=0)
fh=320; cw=int(W*fh/H); strip=Image.new("RGB",(cw*len(norm),fh),(255,255,255)); x=0
for o in norm: strip.paste(o.resize((cw,fh)),(x,0)); x+=cw
strip.save(f"{OUT}/filmstrip.png")
print(f"[done] GIF {OUT}/anim.gif ({len(norm)} frames) | filmstrip {OUT}/filmstrip.png")
