# Sprite HD Upscaler (Real-ESRGAN, GPU)

Faithful HD upscale of MvC2 character sprites — **no restyle, no redraw**, pure
super-resolution, so it stays pixel-faithful and **animation-safe** (zero frame-to-frame
drift). Runs the Real-ESRGAN family (default `anime_6B`) via [spandrel], auto-using your
GPU when CUDA is available (same output as CPU, ~10–50× faster — a full character is seconds).

[spandrel]: https://github.com/chaiNNer-org/spandrel

It takes a per-character sprite atlas (`PLxx.json` + `PLxx.png`), upscales every sprite,
and repacks a **GPU-safe HD atlas** with identical game-space anchors (`dx/dy/wG/hG`) — so
it drops straight into the MapleCast sprite client and renders at the same on-screen size,
just HD.

---

## ⬇️ What you need to copy to your local (3090) machine

1. **The tool itself** → comes with `git pull` (this folder is committed). Nothing to copy.
2. **The sprite source** → `atlas/chars/` (the built per-character rip atlases —
   `PL00.json/png` … `PL3A.json/png`, **59 characters, ~95 MB**).
   **This is NOT in git** (rip-derived art, gitignored), so copy it over manually, e.g.:
   ```bash
   # from the machine that has the repo today, to your local box:
   rsync -av  user@thisbox:/home/tris/projects/maplecast-flycast/atlas/chars/  \
              ~/projects/maplecast-flycast/atlas/chars/
   # (or scp -r the atlas/chars folder)
   ```
   Put it at `<repo>/atlas/chars/` (or anywhere — you pass the path to `--atlas`).
3. **Model weights** → **auto-downloaded** by the script on first run (needs internet on the
   local box). Nothing to copy.

That's it: `git pull` for the code + `atlas/chars/` for the sprites.

> Alternative source (only if you don't have `atlas/chars/`): the raw rip folders
> `MvC2_SpriteFolders_Patched/<Character>/` (partial roster) → use `--folder` mode.

---

## Setup (local GPU box)

```bash
git pull                                   # gets this tools/sprite-upscaler/ folder
cd tools/sprite-upscaler
python -m venv .venv && source .venv/bin/activate

# 1) CUDA build of torch (match your CUDA; cu124 shown):
pip install torch torchvision --index-url https://download.pytorch.org/whl/cu124
# 2) the rest:
pip install -r requirements.txt
```
Verify the GPU is seen:
```bash
python -c "import torch; print(torch.cuda.is_available(), torch.cuda.get_device_name(0))"
# -> True NVIDIA GeForce RTX 3090
```

## Run

**One character** (atlas mode — recommended):
```bash
python upscale.py --model anime_6B \
  --atlas ../../atlas/chars/PL32.json \    # PL32 = Colossus
  --out out/ --keep-masters
```
Output:
- `out/PL32.png` + `out/PL32.json` — GPU-safe HD atlas (default 2×, fits the 8192px texture cap)
- `out/masters/<id>.png` — full-res 4× individual sprites (with `--keep-masters`)

**Whole roster** (loop all 59):
```bash
for f in ../../atlas/chars/PL*.json; do
  python upscale.py --model anime_6B --atlas "$f" --out out/
done
```
On a 3090 each character is a few seconds → the full roster in ~a few minutes.

## Options
| flag | default | meaning |
|---|---|---|
| `--model` | `anime_6B` | `anime_6B` (clean line-art, best for sprites), `4x-AnimeSharp` (sharper), `x4plus`, `4x-UltraSharp`, or a path to any ESRGAN `.pth`. Auto-downloads named ones. |
| `--scale-out` | `2` | final atlas scale. Model upscales 4×; `2` keeps the atlas ≤8192px (WebGPU texture cap). Use `4` only with a higher device limit or multi-page. |
| `--keep-masters` | off | also write the full 4× individual PNGs |
| `--device` | `auto` | `auto` / `cuda` / `cpu` |
| `--folder` | — | folder-of-PNGs mode instead of `--atlas` (rip dumps) |

## Use the result
Drop the HD atlas into the sprite client (it loads `PLxx.{json,png}` by character id):
```bash
cp out/PL32.{png,json} ../../web/test-atlas/chars/
```
or `scp` to prod's `/var/www/maplecast/test-atlas/chars/`. Colossus = `cid 0x32`.

> ⚠️ The HD atlases + masters are rip-derived art — keep them **gitignored, never commit**
> (only this tool's code lives in git). Models are cached under `out/_models/`.

## Models
`anime_6B` is the default (smooth clean line-art). `4x-AnimeSharp` is crisper. Browse hundreds
more at https://openmodeldb.info — any ESRGAN-arch `.pth` loads via spandrel; add a URL to the
`MODELS` dict in `upscale.py` or pass the `.pth` path directly to `--model`.
