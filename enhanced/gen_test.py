#!/usr/bin/env python3
"""
AI sprite-enhancement test bench — send a sprite through any fal.ai image-edit model
and save the result + an input/output side-by-side. Swap model & prompt via env vars.

Usage (key comes from the forgily fal key — never echo it):
  cd ~/projects/maplecast-flycast
  export FAL_KEY=$(grep -hoE '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}:[0-9a-f]{20,}' \
       ~/projects/forgily-creations/design/logo_archive/generate_v24_logos.py | head -1)
  # (optional) export HF_TOKEN=$(grep -E '^HF_TOKEN=' ~/projects/forgily/.env | cut -d= -f2- | tr -d '"')

  MODEL=fal-ai/qwen-image-edit \
  PROMPT="Redraw as crisp HD hand-painted art, keep exact pose/proportions/colors, plain white bg" \
  INPUT=enhanced/inputs/cable89_original.png \
  python3 enhanced/gen_test.py

Env knobs: MODEL, PROMPT, INPUT, STEPS, GUIDANCE, LABEL, OUTDIR
Try other models:  fal-ai/flux-kontext  ·  fal-ai/flux-pro/kontext  ·  fal-ai/gemini-flash-edit  etc.
"""
import os, sys, time, json, base64, io, re, urllib.request, urllib.error
from PIL import Image, ImageDraw, ImageFont

MODEL    = os.environ.get("MODEL", "fal-ai/qwen-image-edit")
PROMPT   = os.environ.get("PROMPT", "Redraw this 2D fighting-game character sprite as crisp high-resolution "
                                    "hand-painted digital art. Keep the EXACT same pose, body proportions, "
                                    "costume and colors. Sharp clean edges, no pixelation. Plain white background.")
INPUT    = os.environ.get("INPUT", "enhanced/inputs/cable89_original.png")
STEPS    = int(os.environ.get("STEPS", "35"))
GUIDANCE = float(os.environ.get("GUIDANCE", "7.0"))
LABEL    = os.environ.get("LABEL", MODEL.split("/")[-1])
OUTDIR   = os.environ.get("OUTDIR", "enhanced/out")
FAL_KEY  = os.environ.get("FAL_KEY", "").strip()
HF_TOKEN = os.environ.get("HF_TOKEN", "").strip()
os.makedirs(OUTDIR, exist_ok=True)
if not FAL_KEY and not HF_TOKEN:
    print("[!] set FAL_KEY (preferred) or HF_TOKEN — see header"); sys.exit(1)

ENDPOINT = f"https://queue.fal.run/{MODEL}"
def auth(): return f"Key {FAL_KEY}" if FAL_KEY else f"Bearer {HF_TOKEN}"
def _req(url, data=None):
    h = {"Authorization": auth()}
    if data is not None: h["Content-Type"] = "application/json"; data = json.dumps(data).encode()
    return urllib.request.urlopen(urllib.request.Request(url, data=data, headers=h,
                                  method="POST" if data is not None else "GET"), timeout=120)

# --- input: load (RGBA→white), enlarge to >=256 (fal minimum), data-url ---
src = Image.open(INPUT).convert("RGBA")
white = Image.new("RGBA", src.size, (255,255,255,255)); white.alpha_composite(src)
inp = white.convert("RGB")
sc = max(1, -(-int(os.environ.get("TARGET","256")) // min(inp.size)))
inp = inp.resize((inp.size[0]*sc, inp.size[1]*sc), Image.NEAREST)
buf = io.BytesIO(); inp.save(buf, "PNG")
data_url = "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()
print(f"[in] {INPUT} {src.size} → {inp.size} | model={MODEL} | label={LABEL}")
print(f"[prompt] {PROMPT[:90]}…")

payload = {"image_url": data_url, "prompt": PROMPT,
           "num_inference_steps": STEPS, "guidance_scale": GUIDANCE, "enable_safety_checker": False}
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

try:
    r = _req(ENDPOINT, payload)
except urllib.error.HTTPError as e:
    print(f"[!] submit {e.code}: {e.read()[:500].decode('utf-8','replace')}"); sys.exit(1)
body = r.read(); ct = r.headers.get("Content-Type","")
out = body if ct.startswith("image/") else None
if out is None:
    j = json.loads(body); iu = find_img(j); su = j.get("status_url"); ru = j.get("response_url")
    if not iu and su:
        for _ in range(90):
            time.sleep(2)
            st = json.loads(_req(su).read()); print("[poll]", st.get("status"))
            if str(st.get("status")).upper() in ("COMPLETED","SUCCEEDED","OK"):
                try: iu = find_img(json.loads(_req(ru or su).read()))
                except urllib.error.HTTPError as e: print(f"[!] result {e.code}: {e.read()[:500].decode('utf-8','replace')}")
                break
            if str(st.get("status")).upper() in ("FAILED","ERROR"): print("[!] failed", st); break
    if not iu: print("[!] no image:", json.dumps(j)[:300]); sys.exit(1)
    out = _req(iu).read()

res = Image.open(io.BytesIO(out)).convert("RGBA")
stamp = LABEL  # caller can vary; keep deterministic name per label
res.save(f"{OUTDIR}/{re.sub(r'[^a-z0-9]+','_',LABEL.lower())}.png")
print(f"[ok] {res.size} → {OUTDIR}/{re.sub(r'[^a-z0-9]+','_',LABEL.lower())}.png")

# side-by-side: ORIGINAL | <model> result
H=560
def fit(img,nn=False): s=H/img.size[1]; return img.resize((max(1,int(img.size[0]*s)),H), Image.NEAREST if nn else Image.LANCZOS)
panels=[("INPUT",fit(src,True)),(LABEL,fit(res))]
try: font=ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",20)
except: font=ImageFont.load_default()
GAP=14;LAB=30; W=sum(p.size[0] for _,p in panels)+GAP*(len(panels)+1)
c=Image.new("RGB",(W,H+LAB+GAP*2),(24,24,27)); d=ImageDraw.Draw(c); x=GAP
for lab,img in panels:
    bg=Image.new("RGB",img.size,(70,70,74))
    if img.mode=="RGBA": bg.paste(img,(0,0),img)
    else: bg.paste(img,(0,0))
    c.paste(bg,(x,LAB+GAP)); d.text((x+4,6),lab,fill=(240,240,240),font=font); x+=img.size[0]+GAP
c.save(f"{OUTDIR}/{re.sub(r'[^a-z0-9]+','_',LABEL.lower())}_compare.png")
print(f"[done] {OUTDIR}/{re.sub(r'[^a-z0-9]+','_',LABEL.lower())}_compare.png")
