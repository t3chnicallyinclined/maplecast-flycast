#!/usr/bin/env python3
"""
Faithful HD sprite upscaler for MapleCast (Real-ESRGAN family via spandrel).
No restyle, no redraw — pure super-resolution, so it's animation-safe (zero drift).
GPU-accelerated automatically when CUDA is available (identical output to CPU, ~10–50× faster).

Two input modes:
  --atlas PLxx.json   Upscale every sprite in a per-character atlas (PLxx.json + sibling PLxx.png).
                      Preserves the game-space anchors (dx/dy/wG/hG) and repacks a GPU-safe HD
                      atlas that drops straight into the sprite client (renders at the same
                      on-screen size, just higher-res). RECOMMENDED.
  --folder DIR        Upscale every *.png in a folder of individual rip sprites (e.g. the
                      MvC2_SpriteFolders dump). Handles the corrupt 'tZXS' PNG chunk + treats
                      palette index 0 as transparent. Derives anchors from the 800px rip canvas.

Output (in --out):
  PLxx.png + PLxx.json   GPU-safe HD atlas (default 2× — fits the 8192px WebGPU texture cap)
  masters/<id>.png       full-res 4× individual sprites (with --keep-masters)

The sprite SOURCE is copyrighted rip art and is NOT in the repo — supply it locally.
"""
import argparse, json, os, io, struct, zlib, time, urllib.request
import numpy as np
from PIL import Image

# ESRGAN-family weights (all load via spandrel). Add OpenModelDB models here as needed.
MODELS = {
    "anime_6B":      "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.2.4/RealESRGAN_x4plus_anime_6B.pth",
    "x4plus":        "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth",
    "4x-AnimeSharp": "https://huggingface.co/Kim2091/AnimeSharp/resolve/main/4x-AnimeSharp.pth",
    "4x-UltraSharp": "https://huggingface.co/lokCX/4x-Ultrasharp/resolve/main/4x-UltraSharp.pth",
}
_CRIT = {b"IHDR", b"PLTE", b"IDAT", b"IEND", b"tRNS"}


def fetch_model(name, mdir):
    os.makedirs(mdir, exist_ok=True)
    p = os.path.join(mdir, f"{name}.pth")
    if os.path.exists(p) and os.path.getsize(p) > 1_000_000:
        return p
    if name not in MODELS:
        raise SystemExit(f"unknown model '{name}'. known: {list(MODELS)} (or pass a .pth path to --model)")
    print(f"[model] downloading {name} …")
    urllib.request.urlretrieve(MODELS[name], p)
    return p


def load_png_robust(fn):
    """Strip corrupt/unknown ancillary chunks so PIL can read the rip PNGs."""
    d = open(fn, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        return Image.open(fn)
    keep = [d[:8]]; i = 8
    while i < len(d):
        ln = struct.unpack(">I", d[i:i+4])[0]; t = d[i+4:i+8]
        ok = struct.unpack(">I", d[i+8+ln:i+12+ln])[0] == (zlib.crc32(d[i+4:i+8+ln]) & 0xFFFFFFFF)
        if t in _CRIT and ok:
            keep.append(d[i:i+12+ln])
        i += 12 + ln
        if t == b"IEND":
            break
    return Image.open(io.BytesIO(b"".join(keep)))


def make_upscaler(model_path, device):
    import torch
    from spandrel import ModelLoader, ImageModelDescriptor
    m = ModelLoader().load_from_file(model_path)
    assert isinstance(m, ImageModelDescriptor), "not an image model"
    m.eval().to(device)
    scale = m.scale

    def up(rgba):  # PIL RGBA -> PIL RGBA upscaled by `scale` (alpha via Lanczos)
        a = np.array(rgba)
        rgb = a[..., :3].astype(np.float32) / 255.0
        al = a[..., 3] if a.shape[-1] == 4 else np.full(a.shape[:2], 255, np.uint8)
        with torch.no_grad():
            o = m(torch.from_numpy(rgb).permute(2, 0, 1).unsqueeze(0).to(device))
        hd = (o.squeeze(0).permute(1, 2, 0).clamp(0, 1).cpu().numpy() * 255).astype(np.uint8)
        ah = np.array(Image.fromarray(al).resize((hd.shape[1], hd.shape[0]), Image.LANCZOS))
        return Image.fromarray(np.dstack([hd, ah]), "RGBA")
    return up, scale


def shelf_pack(items, maxw, pad=2):
    """items: dict key->PIL RGBA. Returns (atlas, {key:(x,y)}, (W,H))."""
    order = sorted(items, key=lambda k: -items[k].size[1])
    x = y = rowh = 0; place = {}
    for k in order:
        w, h = items[k].size
        if x + w + pad > maxw:
            x = 0; y += rowh + pad; rowh = 0
        place[k] = (x, y); x += w + pad; rowh = max(rowh, h)
    W, H = maxw, y + rowh + pad
    atlas = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    for k, (px, py) in place.items():
        atlas.paste(items[k], (px, py))
    return atlas, place, (W, H)


def main():
    ap = argparse.ArgumentParser(description="Faithful HD sprite upscaler (Real-ESRGAN / spandrel)")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--atlas", help="per-character atlas json (PLxx.json with sibling PLxx.png)")
    src.add_argument("--folder", help="folder of individual rip sprite PNGs")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--model", default="anime_6B", help=f"model name {list(MODELS)} or a .pth path")
    ap.add_argument("--scale-out", type=int, default=2, choices=[1, 2, 4],
                    help="final atlas scale (model upscales 4x; 2 keeps the atlas <=8192px GPU cap)")
    ap.add_argument("--max-atlas", type=int, default=8192, help="max atlas dimension (WebGPU default cap)")
    ap.add_argument("--keep-masters", action="store_true", help="also save the full 4x individual sprites")
    ap.add_argument("--device", default="auto", choices=["auto", "cuda", "cpu"])
    ap.add_argument("--origin-x", type=int, default=400, help="folder mode: rip-canvas anchor X")
    ap.add_argument("--origin-y", type=int, default=408, help="folder mode: rip-canvas anchor Y (feet)")
    args = ap.parse_args()

    import torch
    dev = ("cuda" if torch.cuda.is_available() else "cpu") if args.device == "auto" else args.device
    print(f"[device] {dev}" + (f" ({torch.cuda.get_device_name(0)})" if dev == "cuda" else ""))
    mpath = args.model if os.path.exists(args.model) else fetch_model(args.model, os.path.join(args.out, "_models"))
    up, scale = make_upscaler(mpath, dev)
    print(f"[model] {os.path.basename(mpath)} (x{scale})")
    os.makedirs(args.out, exist_ok=True)
    mdir = os.path.join(args.out, "masters")
    if args.keep_masters:
        os.makedirs(mdir, exist_ok=True)

    def downscale(im):
        if args.scale_out == scale:
            return im
        f = args.scale_out / scale
        return im.resize((max(1, int(im.size[0] * f)), max(1, int(im.size[1] * f))), Image.LANCZOS)

    t0 = time.time(); out_imgs = {}; recs = {}
    if args.atlas:
        J = json.load(open(args.atlas))
        png = Image.open(os.path.splitext(args.atlas)[0] + ".png").convert("RGBA")
        name = os.path.splitext(os.path.basename(args.atlas))[0]
        items = list(J["sprites"].items())
        for n, (sid, r) in enumerate(items):
            crop = png.crop((r["x"], r["y"], r["x"] + r["w"], r["y"] + r["h"]))
            hd = up(crop)
            if args.keep_masters:
                hd.save(os.path.join(mdir, f"{sid}.png"))
            o = downscale(hd); out_imgs[sid] = o
            nr = dict(r); nr["w"], nr["h"] = o.size  # x,y set after packing; keep dx/dy/wG/hG/facing
            recs[sid] = nr
            if (n + 1) % 100 == 0:
                print(f"  {n+1}/{len(items)} ({time.time()-t0:.0f}s)")
        meta = {k: J[k] for k in ("screenW", "screenH", "name", "pal128") if k in J}
    else:  # folder mode
        import glob
        files = sorted(glob.glob(os.path.join(args.folder, "*.png")))
        name = os.path.basename(os.path.normpath(args.folder))
        for n, fn in enumerate(files):
            im = load_png_robust(fn)
            if im.mode == "P":
                idx = np.array(im); pal = np.array(im.getpalette()).reshape(-1, 3)
                a = np.dstack([pal[idx].astype(np.uint8), np.where(idx == 0, 0, 255).astype(np.uint8)])
            else:
                a = np.array(im.convert("RGBA"))
            ys, xs = np.where(a[..., 3] > 16)
            if len(xs) == 0:
                continue
            x0, y0, x1, y1 = xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
            sid = os.path.splitext(os.path.basename(fn))[0]
            hd = up(Image.fromarray(a[y0:y1, x0:x1], "RGBA"))
            if args.keep_masters:
                hd.save(os.path.join(mdir, f"{sid}.png"))
            o = downscale(hd); out_imgs[sid] = o
            recs[sid] = {"w": o.size[0], "h": o.size[1], "wG": int(x1 - x0), "hG": int(y1 - y0),
                         "dx": int(x0 - args.origin_x), "dy": int(y0 - args.origin_y), "facing": 0}
            if (n + 1) % 100 == 0:
                print(f"  {n+1}/{len(files)} ({time.time()-t0:.0f}s)")
        meta = {"screenW": 640, "screenH": 480, "name": name}

    atlas, place, (W, H) = shelf_pack(out_imgs, args.max_atlas)
    if H > args.max_atlas:
        print(f"[WARN] atlas {W}x{H} exceeds {args.max_atlas}px — won't upload as one GPU texture. "
              f"Use --scale-out {max(1, args.scale_out//2)} or split into pages.")
    for sid, (px, py) in place.items():
        recs[sid]["x"], recs[sid]["y"] = px, py
    atlas.save(os.path.join(args.out, f"{name}.png"))
    json.dump({**meta, "sprites": recs}, open(os.path.join(args.out, f"{name}.json"), "w"))
    print(f"[done] {len(recs)} sprites in {time.time()-t0:.0f}s on {dev}")
    print(f"       HD atlas: {args.out}/{name}.png  ({W}x{H})  +  {name}.json")
    if args.keep_masters:
        print(f"       4x masters: {mdir}/")


if __name__ == "__main__":
    main()
