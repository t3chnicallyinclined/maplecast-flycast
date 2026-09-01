# enhanced/ — AI sprite-enhancement test bench

Working area for testing HD sprite enhancement (upscale + generative redraw) on MvC2 rip
sprites. **Gitignored** — everything here is rip-derived / AI-derivative art, never commit.

## Layout
- `inputs/` — source sprites to feed (`cable89_original.png` = clean rip crop, `cable89_esrgan4x.png` = Real-ESRGAN 4×)
- `results/` — comparisons already generated:
  - `esrgan_models_compare.png` — orig vs Lanczos vs Real-ESRGAN anime_6B vs x4plus
  - `esrgan_zoom_headtorso.png` — head/torso close-up (anime_6B is the winner for sprites)
  - `anime6B_hd_sprite.png` — a single 4× anime_6B sprite
  - `qwen_redraw_cable89.png` — Qwen-Image-Edit redraw (gorgeous but drifts from the original)
  - `compare_orig_esrgan_qwen.png` — the three-way: faithful upscale vs free redraw
  - `anchoring_filmstrip.png` — proves the atlas anchors hold across poses
- `out/` — output of `gen_test.py` runs
- `gen_test.py` — parameterized fal.ai image-edit tester

## Key finding so far
- **Real-ESRGAN (anime_6B)** = faithful: same pose/proportions/colors, transparent, animation-safe. Local, free.
- **Qwen-Image-Edit** = stunning HD *redraw* but reinterprets the character (face/anatomy/gun change, loses alpha) → not drop-in for a 681-frame animation without structure-locking.

## Run a test (swap model + prompt)
```bash
cd ~/projects/maplecast-flycast
# fal key from the forgily logo scripts (never echoed):
export FAL_KEY=$(grep -hoE '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}:[0-9a-f]{20,}' \
     ~/projects/forgily-creations/design/logo_archive/generate_v24_logos.py | head -1)

# default (Qwen-Image-Edit)
MODEL=fal-ai/qwen-image-edit LABEL=qwen \
PROMPT="Redraw as crisp HD hand-painted art, keep EXACT pose/proportions/colors, plain white bg" \
INPUT=enhanced/inputs/cable89_original.png python3 enhanced/gen_test.py

# try another model
MODEL=fal-ai/flux-kontext LABEL=flux_kontext PROMPT="..." python3 enhanced/gen_test.py
```
Env knobs: `MODEL PROMPT INPUT STEPS GUIDANCE LABEL OUTDIR`. Each run writes
`out/<label>.png` + `out/<label>_compare.png` (input vs result).

Notes: fal requires inputs ≥256×256 (the script auto-enlarges with NEAREST). Models cost
~$0.02–0.04/image. For the moveset, the open question is **frame-to-frame consistency** —
test the same model on several consecutive frames before trusting it on an animation.
