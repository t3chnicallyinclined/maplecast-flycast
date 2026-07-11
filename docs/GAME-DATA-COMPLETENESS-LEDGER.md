# MVC2 Game-Data Completeness Ledger

> **Purpose (2026-07-10):** MapleCast is a *platform* (competitive client, training tool, match-data
> pipeline), not just a video stream. "Not a render pixel" does **NOT** mean "not important." This
> ledger catalogs the FULL per-frame data surface MVC2 exposes, what we currently use, and what is
> untapped — so no future agent drops data that has product value. **Do not delete a data source
> because the renderer doesn't consume it.** Companion to the render gates in `tools/render-replica-poc/`.

## The per-part GFX2 cell-record flags-u16 (r11+0x4) — full accounting (re_kb: record_flag_axes)

| bit(s) | meaning | consumer | our use | product value |
|---|---|---|---|---|
| `0x8000` | **HORIZONTAL flip** (X reflect + texU) | body walkers loc_8c0344d4/loc_8c0348c8 | **RENDER — fix in progress** | pixels |
| `0x4000` | **VERTICAL flip** (Y reflect + texV) | same | **RENDER — fix in progress** | pixels |
| `0x0010/0x20/0x40/0x80` (low byte `0xFF`) | **hitbox_data selector** ((flags&0xFF)*8 → char+0x170) | loc_8c0341b4 @9739 | not yet | **HITBOX VIEWER** (training) — BUILD THIS |
| `(flags & 0x3FF)>>4` | **hit-flash pattern** (with char+0x12d/0x12e) | loc_8c033e90 @9232 | not yet | pixels (flash) — verify via palette path |
| `0x0100–0x2000` (~0.03%) | rare gameplay/authoring | not read by render walkers | not yet | investigate if any is visible |

## Beyond the record flags — the data surface (sources + status)

| data | where | status | product value |
|---|---|---|---|
| **Hitboxes** | char+0x16C hitbox_pattern_table, +0x170 hitbox_data; anotak per-move geometry (refs/anotak); record low byte selects | **UNTAPPED** | **HITBOX VIEWER toggle** — user-requested feature, we have the data |
| **Frame data** (startup/active/recovery) | anotak attack records 0x1C (docs/MVC2-FRAMEDATA-FIELDS.md); anim cells 0x14 (duration/ender/render_extra/hitbox_group per skin-studio anim catalog) | partial (catalog decoded) | **FRAME-DATA OVERLAY** (training) |
| **Hit-flash** | char+0x12e (real field, re_kb), char+0x40 char_pal_effect; record 0x3FF>>4 selects pattern | not reproduced | pixels — palette subsystem, verify vs legacy |
| **Super / meter state** | globals 0x8C289646/648 fill, 0x28964A/B level; char+0x200-0x206 buffs (speed/flight/armor/dmg/def) | partial (meter shipped in read-set) | analysis, HUD |
| **Combo / damage** | 0x8C289670/672 combo; health +0x420, red +0x424 | captured (status broadcast) | stats, leaderboards |
| **Move / anim state** | +0x1D0 animation_state, +0x1E9 special_move, +0x158 anim_group; SPL dispatch 0x8C289BD8+slot*0x80; S_PLxx code (marvelous2) | partial | move detection, AI dataset |
| **RNG** | 0x8C16BC2C RngVal | not used | determinism / replay |
| **Camera** | cam M1·M2 (re_kb/39) | render (stage) | — |

## Standing rule for agents
When RE surfaces a field/flag/record, record it here even if the renderer ignores it. Tag: RENDER
(pixels), HITBOX, FRAMEDATA, STATE (gameplay/analysis), or NON-VISIBLE. "Safely ignorable by
render_frame" is scoped to render_frame's geometry emit — it is NEVER a statement about product value.
Cross-ref: docs/MATCH-DATA-PLATFORM.md (the full-state-capture vision), docs/COMPETITIVE-CLIENT.md
(HUD/combo-trainer), tools/re_kb (the RE graph), the render gates
(_zz_catalog_carve_gate / _zz_flag_coverage / _zz_anim_flag_audit / _zz_rf_engine_diff).

## Immediate roadmap items born from this ledger
1. **Hitbox viewer** (user-requested) — decode record-flag-selected hitbox_data (char+0x170) + anotak
   geometry, draw as a toggle overlay in webgpu-test.html / the command center. Data is IN the wire
   (char struct ships in the read-set).
2. **Frame-data overlay** — anim-cell duration/ender + anotak attack records → startup/active/recovery.
3. **Hit-flash palette** — reproduce char+0x12e hit-state palette effect, gate vs legacy.
