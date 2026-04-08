# Naomi MVC2 Spike — `naomi-mvc2-spike` branch

**Branch base:** `wasm-determinism` @ a28fa8090 (overlord PLAYERS card), then a cleanup pass landing in-flight uncommitted work as commits before the spike begins.
**Status:** Phase 0 complete — ROMs staged and CRC-verified, branch cleaned up, headless binary built green (27 MB stripped, zero forbidden libs). Phase 1 (boot the cart) is next.

**Branch base note:** Initially planned to branch off `maplecast`, but the headless server source code (norend wiring, `MAPLECAST_HEADLESS_BUILD` define, CMake compile-out, the entire shipped Phase 1-5 work) lives on `wasm-determinism` and was never merged back to `maplecast`. The 27 MB production binary on the VPS was built from `wasm-determinism`. Branching off `maplecast` would have required cherry-picking ~10-20 infrastructure commits, which is a fork by another name. We branched off `wasm-determinism` HEAD instead, then committed a small pile of in-flight uncommitted work that committed source already depended on (input latch Phase A/B, the SIGUSR1 force-save broadcast, the dead-code purge, INPUT-LATCH.md, DEPLOYMENT.md). One stash (`stash@{0}`, web/collector WIP) remains parked — it's a different feature pile (overlord SPA / queue / king.html UI work) and doesn't belong on the spike branch.

**Goal:** Validate that the headless flycast can boot Naomi MVC2 (USA M2 cart) and produce a byte-perfect TA mirror wire, as a precondition to migrating nobd.net production from Dreamcast MVC2 (mvc2.gdi) to the arcade original.

The full plan and rationale lives in a local plan file (`floating-hatching-dove.md`) — not in the public repo. This doc is the public-safe summary.

## What this branch is for

The production VPS at `nobd.net` runs the **Dreamcast** port of MVC2 (mvc2.gdi). The user wants to evaluate switching to the **Naomi arcade original** (mvsc2u, USA M2 cart) for:

1. Authenticity ("arcade in the cloud" stops being a half-truth)
2. Tighter input timing (Naomi cancel windows match what stick players expect from the cab)
3. ~2,520 lines of GD-ROM emulation that can be deleted post-migration
4. Enables the GPT essay's stripping plan (see [floating-hatching-dove.md])

This branch is the **spike** — a 2-week parallel exploration that does NOT touch the DC production path. The DC `maplecast-headless.service` on the VPS continues running unchanged. Naomi will run on a parallel `maplecast-headless-naomi.service` once the spike is validated.

## Phase 0 — DONE

**ROMs staged at `~/roms/mvc2_naomi/`:**

| File | Size | Notes |
|---|---|---|
| `mvsc2u.zip` | 81 MB | USA M2 cart, 15 ROM blobs, all 15 CRCs verified against [naomi_roms.cpp:2587-2601](../core/hw/naomi/naomi_roms.cpp#L2587-L2601) |
| `naomi.zip` | 3 MB | All 4 regional Naomi BIOSes (JP/USA/EU/KR) + multiregion hack, all CRCs verified against [naomi_roms.cpp:90-142](../core/hw/naomi/naomi_roms.cpp#L90-L142) |

**CRC verification (all ✅):**
```
epr-23062a.ic22  96038276  (boot ROM, USA M2)
mpr-23048..23061  → 14 mask ROMs, all verified
epr-21576h.ic27  d4895685  (Japan BIOS)
epr-21577h.ic27  fdf17452  (USA BIOS)
epr-21578h.ic27  7b452946  (Export BIOS)
```

**How Flycast finds them:** [naomi_cart.cpp:80-127](../core/hw/naomi/naomi_cart.cpp#L80-L127) opens by **CRC32 first**, filename second. The MAME silkscreen filenames inside the zip (`mpr-23048.ic17s`) differ from Flycast's database expectations (`mpr-23048.ic1`), but the CRC-first lookup makes that irrelevant. We verified every blob's CRC matches.

## What changes during the spike

**Files that may need to change** (gated on `settings.platform.isNaomi()`):
- [core/network/maplecast_gamestate.cpp](../core/network/maplecast_gamestate.cpp) — the 253-byte RAM autopsy. DC addresses (0x8C268340 etc.) will not be valid on Naomi. **This is the long pole** — Phase 4 of the spike.
- [deploy/systemd/maplecast-headless-naomi.service](../deploy/systemd/maplecast-headless-naomi.service) — new parallel systemd unit
- [deploy/systemd/maplecast-headless-naomi.env](../deploy/systemd/maplecast-headless-naomi.env) — env file pointing at `~/roms/mvc2_naomi/mvsc2u.zip`

**Files that should NOT change** (the spike is read-only on these unless we hit a real bug):
- [core/network/maplecast_mirror.cpp](../core/network/maplecast_mirror.cpp) — TA mirror is platform-agnostic (verified: zero `settings.platform` references)
- [core/network/maplecast_input_server.cpp](../core/network/maplecast_input_server.cpp) — `kcode[]` writes work for both DC and JVS paths
- [core/network/maplecast_compress.h](../core/network/maplecast_compress.h) — wire envelope is unchanged
- `relay/` — Rust relay is platform-agnostic
- `packages/renderer/` — browser WASM renderer is platform-agnostic

## Spike phases

- [x] **Phase 0** — ROM sourcing, staging, CRC verification, branch creation
- [ ] **Phase 1** — Boot the cart locally with the unmodified headless binary (GO/NO-GO)
- [ ] **Phase 2** — TA mirror determinism rig (`MAPLECAST_DUMP_TA=1`) on the Naomi build, byte-cmp against itself first, then against a GPU mirror client
- [ ] **Phase 3** — Input path smoke test (browser gamepad → JVS → game)
- [ ] **Phase 4** — RAM autopsy on Naomi MVC2 to rebuild the 253-byte gamestate map (the long pole)
- [ ] **Phase 5** — Side-by-side soak test (24h) on a separate VPS port
- [ ] **Phase 6** — Production cutover decision (only if all of the above pass)
- [ ] **Phase 7** — Post-migration cleanup (the GPT essay's strip plan)

## Test recipe (from the plan, ready to run)

```bash
# Phase 1 — boot the cart, no source changes
MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1 MAPLECAST_HEADLESS=1 \
  MAPLECAST_PORT=7130 MAPLECAST_SERVER_PORT=7230 \
  ./build-headless/flycast ~/roms/mvc2_naomi/mvsc2u.zip

# Phase 2 — determinism rig
# Terminal 1
MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1 MAPLECAST_DUMP_TA=1 \
  MAPLECAST_PORT=7130 MAPLECAST_SERVER_PORT=7230 \
  ./build-headless/flycast ~/roms/mvc2_naomi/mvsc2u.zip

# Terminal 2 — GPU mirror client
MAPLECAST=1 MAPLECAST_MIRROR_CLIENT=1 \
  MAPLECAST_SERVER_HOST=127.0.0.1 MAPLECAST_SERVER_PORT=7230 \
  MAPLECAST_DUMP_TA=1 MAPLECAST_PORT=7131 \
  ./build/flycast ~/roms/mvc2_naomi/mvsc2u.zip

# After 60s, byte-cmp dumps per ARCHITECTURE.md:436-443
```

## What "spike succeeded" means

All of:
1. Naomi cart boots to attract mode in headless mode
2. Determinism rig: 0 differ over 60s of attract mode
3. Input smoke test: all 6 buttons reach the game via JVS path
4. RAM autopsy: at least the timer + HP bars + character IDs read correctly through the status JSON

## What "spike failed, abort" means

Any of:
1. The cart doesn't boot (deeper Flycast issue, not solvable in spike scope)
2. Determinism rig shows non-zero differ (mirror has hidden DC-isms we missed)
3. RAM autopsy takes more than a week (schedule risk too high)
4. JVS input path has a structural latency problem the spike can't fix

If we abort, the DC build on `maplecast` is untouched and production keeps running. The branch and the lessons learned stay in the repo for the next attempt.
