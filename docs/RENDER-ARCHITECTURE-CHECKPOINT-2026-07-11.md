# Render Architecture — CHECKPOINT (2026-07-11)

The stable, shipping render config after the wire-thinning + stage campaign. This is the DEFAULT
on https://nobd.net/webgpu-test.html. Supersedes the "strip everything / bake the stage" detour.

## The one-line principle

**Strip only the DYNAMIC content that actually costs bandwidth (character bodies); ship the
STATIC/cheap content LIVE and pixel-perfect (stage, effects).** Static content is ~free on the
wire (the zstd streaming window dedupes bitwise-identical frames to nothing), so stripping it saves
~0 while forcing a stale bake to maintain — not worth it. Bodies move every frame → they cost wire →
that's where the strip + local render earns its keep.

## What renders how (the split)

| Content | Nature | How it renders | Wire cost |
|---|---|---|---|
| **Stage / background** | static (bitwise-identical every frame, `_bwlab/STAGE-SHARE-REPORT.md`) | rides the wire TA + VRAM, drawn by the client PVR2 renderer from the parsed wire frame | ~0 (zstd dedup) — PIXEL-PERFECT |
| **Character bodies** | dynamic | server char-strips the para5 body quads (banks {82,83,88,89}); client draws them via **render_frame** (transpiled SH4) fed by the folded STM2 body state, spliced in by `_bodyMerge` | thin (STM2 delta) — BYTE-EXACT (re_kb 189544592) |
| **Effects / projectiles / supers** | dynamic | ride the wire TA, drawn by the client | modest |
| **HUD** | wire | rides the wire TA | modest |

## Prod env (149.28.44.118, /etc/maplecast/headless.env)

```
MAPLECAST_ZSTREAM=1 / ZSTREAM_LEVEL=9 / ZSTREAM_SOA=1 / ZSTREAM_RESET=600   # ZCS2 streaming-zstd wire
MAPLECAST_STAGESTRIP=0   # OFF — stage rides the wire pixel-perfect (static -> ~0 cost)
MAPLECAST_CHARSTRIP=1    # ON  — strip body quads; render_frame draws them locally, byte-exact
MAPLECAST_STATE_MERGE=1  # body state folded into the main ZCS2 wire as the STM2 trailer (one socket)
MAPLECAST_VCACHE=1       # content-addressed VRAM page cache
MAPLECAST_NO_SCENE_SYNC=1# request-driven full-VRAM SYNC broadcast gated OFF (proven redundant)
# MAPLECAST_CHARSTRIP_PAGES OFF (the VRAM page-strip; only helps 9% + glitches — not enabled)
# MAPLECAST_WIREMON OFF (diagnostic, off after the campaign)
```

Client defaults (webgpu-test.html): `statemerge` + `fxdecode` DEFAULT ON. `bodysrc=wasm` (render_frame,
NOT the sprite machine — re_kb/74 kept render_frame). No URL params needed — the bare link IS this config.

## Measured state (2026-07-11)

- **Bandwidth ~3 Mbps gameplay, spikes ~6 Mbps triple super.**
- The ~3 Mbps is dominated by the **CHARSTRIP TA-delta inflation** (removing body quads shifts every
  remaining TA byte → the byte-run delta re-encodes the shifted tail). This is the #1 remaining
  optimization: replace the server char-strip with a **client-side body-quad skip** (keep the TA
  byte-stable, small delta ~0.6 Mbps; client filters body quads by bank + draws render_frame local).
- Super spike (~6 MB decompressed) = the effect render-STATE (efxtmpl scale arenas + rectab), a genuine
  render_frame floor (game-sim-filled, client can't regenerate). Kill it via the [[project_prebaked_super_effects_hybrid]] (pre-baked super sprites) if desired.

## Known glitches (deferred, not blocking)

- Minor body glitching (e.g. Cape z-order) — the render_frame body path's residual (see re_kb).
- (Nothing stage-side — stage is pixel-perfect from the wire now.)

## Campaign resolutions that got here

- STM2 size-tolerant delta (was keyframing every frame → 360KB flat) + KEY-defer (killed the 59KB super reseed spike).
- Super-spike root cause = 84% render-STATE (efxtmpl/rectab), genuine render_frame floor.
- Sprite machine A/B-rejected (re_kb/74) — render_frame is the drawer.
- Stage: floor was a `_buildFromTA` MARGIN=800 cull bug (ground-plane near corners project past the
  viewport), NOT missing data — but moot because STAGESTRIP=0 (stage from wire) is the right call.
- fillBGP-before-\_bodyMerge coupling fix: fillBGP rebuilt g.vertexData from P._u8 (no body verts) +
  bumped first[]; running AFTER _bodyMerge dangled the body indices → stretched/invisible bodies when
  STAGESTRIP off. Reordered → decoupled. This unlocked STAGESTRIP=0 + local bodies.

Full campaign record: docs/HANDOFF-WIRE-THINNING-2026-07-11.md. Memory: [[project_render_replica_live_deployed]], [[project_charselect_precache_thesis]].
