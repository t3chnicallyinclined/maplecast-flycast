# ROM-Asset Client — Build-Out Plan (post-verification)

> **Companion to [ROM-ASSET-CLIENT.md](ROM-ASSET-CLIENT.md)** (the research/verification
> handoff) and [BAKE-HARNESS-PLAN.md](BAKE-HARNESS-PLAN.md) (the phased bake plan).
> The thesis is **verified** (Ryu idle = 100% pixel-stable from `(char_id, sprite_id)`);
> this doc is the forward plan to turn that into a working client, plus the
> architecture decision, two research findings (PVR2 transparency, ModNao), and
> the bake gap that blocks a client-usable atlas.
>
> Created 2026-06-04 on branch `feat/rom-asset-probe`. Nothing here is built yet.

---

## 1. The architecture question: full client or "253 B + a smaller TA/VRAM diff"?

**Decision: defer it. Prove the character layer renders from state first, then choose
from measured data.** Reasoning:

- The TA/VRAM diff does **NOT shrink automatically** by adding a client-side sprite
  layer. The server still captures and ships the full TA buffer + VRAM diffs,
  characters included. The stream only gets smaller if the **server stops emitting
  the character layer** (strips character polys from the TA command list + stops
  shipping their sprite VRAM pages). That is real surgery and it directly threatens
  the **byte-perfect determinism guarantee** (the six-bugs invariant in
  [ARCHITECTURE.md](ARCHITECTURE.md#%EF%B8%8F-the-wire-is-deterministic-and-byte-perfect-commit-466d72d54)).
  There is no free lunch — you save bandwidth by *removing* character data
  server-side, not by *adding* a sprite overlay on an unchanged stream.

- A **full client** (stream only ~253 B, render everything locally) needs, in
  rising order of effort:
  - **Characters** — ≈proven (the bake).
  - **HUD** — trivial; health / red-health / meter / combo / timer / stage are
    *already* in the 253-byte state ([maplecast_gamestate.cpp:190](../core/network/maplecast_gamestate.cpp#L190)). Just draw them.
  - **Projectiles / special-move artifacts** — assets likely bakeable (character-owned
    `PLxx_DAT`), but they are **separate game objects** not in the 6 tracked slots
    ([maplecast_gamestate.cpp:97](../core/network/maplecast_gamestate.cpp#L97)). Needs
    RAM RE of MVC2's active-object table → a variable-length object section in GSTA.
  - **Stages** — 3D NaomiLib scenes, animated, camera-driven (`camera_x/y` is in
    state). Big rendering project; bandwidth payoff is small (stages are fairly
    static and compress well in the TA stream).
  - **Effects / super flashes / hit sparks** — separate object system; needs both
    object-table RE (state) and the EFKY asset decode (ModNao, §3).

- **The number we don't have:** what fraction of the live ~4 Mbps is the character
  layer? If characters are ~70 % of the stream, stripping them server-side (hybrid)
  is compelling; if ~30 %, maybe not. **Measure before committing** (Phase 3).

**Near-term move that commits to neither and risks nothing in production:** render
characters from atlas + GSTA *in parallel* with the live TA mirror and **pixel-diff
them** (handoff P4→P5). Zero server change, proves the thesis end-to-end, and
produces the bandwidth-fraction number that makes the full-vs-hybrid call.

---

## 2. Research finding — "disable transparency to kill DC slowdown"

**Mechanism: confirmed and sound. The specific MVC2 modder anecdote: plausible, not
found documented.** PVR2 (CLX2) is a tile-based deferred renderer: opaque pixels get
free hidden-surface removal, but translucent polygons defeat it — they need
back-to-front blending, so the GPU runs a **multi-pass per-tile autosort** at roughly
**2× opaque cost**, and translucent overdraw burns fillrate. The Dreamcast has half
the VRAM and bandwidth of the arcade NAOMI, so it is the more fillrate-starved
target. "Disabling transparency" = reclassifying translucent polys to **punch-through
(alpha-test)** or opaque, skipping the sort pass. MVC2 sprites have hard edges, so
punch-through would look nearly identical at far lower cost.

**Relevance to us:** it confirms what MapleCast already bakes in — **MVC2 submits
characters on the *translucent* list** (CLAUDE.md: "characters are regular translucent
textured polygons"), which is exactly why the bake isolates `drawTrans=true,
drawOpaque/drawPunch=false`. But it is **mostly orthogonal** to the ROM-asset client:
we don't need a no-transparency mod to bake (we already isolate by list type), and the
slowdown is a gameplay property the server emulates and the deterministic state already
encodes (the frame counter just advances slower). Where it *could* matter later: a
low-slowdown modded ROM could be a nicer server ROM (changes the wire, fine if both
ends match the same ROM hash). **Not a blocker; do not chase now.**

---

## 3. Research finding — ModNao (github.com/rob2d/modnao)

A browser **3D NaomiLib model + PVR/VQ texture** tool (Next.js / React / Three.js,
TypeScript). Parses `STGxxPOL.BIN` / `TEXxx.BIN`, decodes PVR textures
(RGB565 / ARGB4444 / ARGB1555 / VQ-compressed, Morton-twiddled), and exports **PNG
textures + glTF models**.

- **Does NOT help characters.** MVC2 fighters are 2D `PLxx_DAT` sprites; ModNao has
  zero RLE/sprite code and explicitly punts the sprite path to a separate linked tool.
  The bake harness owns buckets 1–2.
- **Does help the parts we had no decoder for:** **stages (`STGxx`), UI (`DCxx`/`DMxx`),
  and effects (`EFKY`, "mvc2-special-effects")** — i.e. the stage layer and the
  super-flash / hit-spark bucket.
- **Caveat — asset half only.** ModNao decodes the *graphics*; the *state* half (when/
  where each effect spawns, stage animation timing) still needs RAM RE — same problem
  as projectiles, and stage animation isn't in the 253 bytes.
- **Reuse:** its parse layer is plain TS with no Three.js dependency → forkable into our
  WebGPU app. License is restrictive (non-commercial / no-AI-training) — fork the
  parsers deliberately, not the app.

ModNao is therefore the **asset decoder for the full-client stage + effect layers**,
to be pulled in only when/if we pursue a true full-scene client (a separate track from
the character work below).

---

## 3a. Codec status — what's actually solved, and the PalMod dead-end (2026-06-04)

We investigated whether MVC2's sprite GFX codec is solved well enough to decode sprites
from the **user's own ROM** (the legally-clean end-state: ship the 253 bytes + code,
zero copyrighted assets). Findings, including a wrong turn worth recording so nobody
repeats it:

**What IS solved / verified:**
- `char_id` → file: `PL{char_id:02X}DAT` (HEX). Verified by sight: 0=Ryu=PL00,
  23/0x17=Cable=PL17, 35/0x23=Dan=PL23. ⚠️ an earlier note's "Cable=PL23" is wrong —
  PL23 is Dan; Cable is PL17.
- `sprite_id` (RAM 0x144) is the flat sprite index; the community **indexed rip**
  (`PLxxDAT/<sprite_id>.png`, palette-indexed) gives decoded pixels per sprite_id. This
  is enough to **build and test the client now** (rip as the atlas / stand-in for what a
  ROM-decode would produce).

**The PalMod dead-end (tested, disproved):** PalMod (github.com/Preppy/palmod) has
`RLEData::RLEDecodeImg`/`BitMaskRLEDecodeImg`, which *looked* like the MVC2 GFX codec. A
decode test of PalMod's `img2020.dat` disproved it:
- `img2020.dat` is **15,039 images, 99.9% raw-deflate (zlib)**; only **8** use the RLE
  path. It's keyed by PalMod `unit`/`imgId`, stores **composite preview sheets** (e.g.
  Ryu = 458×183 with baked-in "RYU" text), and the "Ryu" in the shared `CPS2` section is
  the Street-Fighter Ryu, not MVC2's.
- PalMod is a **palette editor**: it loads *palettes* from the user's game file and
  overlays them on these **pre-extracted** sprites. The extraction was done by the
  author's offline pipeline, which is **not in the repo**. `RLEData` is PalMod's legacy
  *storage* codec, **not** MVC2's ROM codec. So PalMod does **not** give us a
  decode-from-user-ROM path.

**What's still OPEN:** an open decoder for MVC2's `PLxx_DAT` GFX pixel codec that we can
run on the user's ROM. `dasm_PLDAT` decodes PLxx_DAT *structure* (offset table, EXTRAS,
palette) but not the pixels. The rip's existence proves a decoder exists *somewhere* —
**pin down how the rips were made** (instrumented-emulator VRAM dump? a custom RLE
decoder?). Until then, the ROM-decode shipping path is unproven; the **bake** and the
**rip** remain the ways to *get* sprites. (If we do find/port a decoder: PalMod's no-license
status is moot for it, but for any borrowed code, default copyright = clean-room or ask.)

---

## 4. The bake gap that blocks a client-usable atlas

[bake.mjs](../web/webgpu/bake.mjs) today is a *verification* harness, not an *export*
harness. It captures the tight alpha-bbox crop (`firstImg`, [bake.mjs:131](../web/webgpu/bake.mjs#L131))
and keys on `slot|char_id|sprite_id` ([bake.mjs:141](../web/webgpu/bake.mjs#L141)), but:

1. **It throws away the anchor.** A client cannot position a sprite without the offset
   from the reported `screen_x/y` to the crop's top-left (`dx = minx − cx`,
   `dy = miny − cy`). This is the #1 thing to add.
2. **It doesn't fold in `facing`.** Captured in the slot but not the key. Handle by
   horizontal flip at draw time (record which facing was captured so the flip is
   correct).
3. **It doesn't track `palette`.** One palette is captured per sprite; the live
   skin-swap system (PVR palette banks, see CLAUDE.md) means a client must apply the
   palette from state at draw time, not bake one in.

So **Phase 0 is making the bake emit a client-usable atlas (PNG + JSON), not just a
montage.**

---

## 5. Phased plan (testable on the existing webgpu-test page)

The renderer ([pvr2-renderer.mjs:174](../web/webgpu/pvr2-renderer.mjs#L174) `renderFrame`,
`alphaMode:'premultiplied'`, DBG list toggles at [lines 287–349](../web/webgpu/pvr2-renderer.mjs#L287))
and the GSTA intake ([webgpu-test.html:780](../web/webgpu-test.html#L780)) already exist.
`?ws=ws://localhost:7200` ([webgpu-test.html:759](../web/webgpu-test.html#L759)) connects
the page straight to a local mirror server.

| Phase | What | Risk | Verify |
|---|---|---|---|
| **0** | Anchor capture in bake.mjs (`dx,dy`); record captured `facing`; export **atlas PNG + JSON** (`char → sprite_id → {x,y,w,h,dx,dy,facing}`) alongside the existing montage/CSV | none | atlas JSON downloads; manifest has anchor cols |
| **1** | New **"Sprite Client" toggle** in webgpu-test.html: bypass the TA→renderer path; on each GSTA, draw `sprite[char_id][sprite_id]` as a textured quad at `screen_x+dx, screen_y+dy`, flipped per `facing`, palette applied from state | none (additive) | load Ryu atlas → idle Ryu renders from state alone, no TA |
| **2** | **Side-by-side diff** (handoff P5): TA-mirror vs sprite-client from the *same* live stream; per-frame character-region match % | none | Ryu idle matches in tolerance; motion mismatch quantified |
| **3** | Coverage + **the decision number**: script Ryu full moveset (input-injection / `.mcrec`), bake full atlas; instrument server to report **character-attributable TA+VRAM bytes** | low | per-char fidelity %; character bandwidth fraction |
| **+** | (parallel, easy win) **HUD from state** — health/meter/combo/timer drawn from the 253 bytes | none | HUD renders with no TA |
| **later** | **ModNao track** — fork the NaomiLib/PVR parsers, decode one stage + the EFKY effects → assess full-scene feasibility | n/a | one stage renders from decoded assets |

### Testing constraints (be honest about the loop)

The live pixel test needs a running mirror server + an actual match (ROM + the
`local-play` setup, which the handoff describes on the Windows box). The ROM is **not**
in the repo and must not be. So: **Phases 0–2 and the HUD are buildable without a live
server; the live bake/diff is operator-driven.** Lookup/positioning can be smoke-tested
offline by replaying a captured atlas against a recorded GSTA log.

---

## 6. Open RE questions (the keystones)

- **MVC2 active-object table** — where the engine keeps the list of live non-character
  objects (projectiles, effects). This is the keystone for buckets 2–3: without it,
  GSTA can't carry per-object `sprite_id`/position, and no atlas helps.
- **Effect sorting in the TA lists** — can effects be cleanly separated from characters
  in the command stream (both are translucent, so a list-type cut won't do it)? Bears
  on whether a "characters-from-state + residual-from-TA" hybrid is even extractable.
- **Stage animation state** — not in the 253 bytes today; what minimal additions would
  let a client drive a ModNao-decoded stage.

---

## Related
- [ROM-ASSET-CLIENT.md](ROM-ASSET-CLIENT.md) — research + pixel-level verification (the why)
- [BAKE-HARNESS-PLAN.md](BAKE-HARNESS-PLAN.md) — the phased bake plan this updates
- [STREAMING-OPTIONS.md](STREAMING-OPTIONS.md) — Option 6 (this), bandwidth comparison
- [ARCHITECTURE.md](ARCHITECTURE.md) — wire format, the byte-perfect determinism guarantee
- [WEBGPU-RENDERER.md](WEBGPU-RENDERER.md) — the renderer the bake/client build on
