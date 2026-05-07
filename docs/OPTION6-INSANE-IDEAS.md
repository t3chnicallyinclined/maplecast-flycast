# MapleCast — Insane Ideas Edition [ARCHIVED RESEARCH]

> **🪦 OUTCOME (2026-05-07): Idea pile preserved as research log. None of these were implemented under Option 6 — the project pivoted to latency-first work (geographic distribution + Tier 1 server tweaks + tournament-mode env var). See [OPTIMIZATION-PLAN.md](OPTIMIZATION-PLAN.md) for the path actually taken.**
>
> **Status of each Big Three idea:**
> - **#1 JIT Sprite Atlas + CAS** — not implemented. Would still be the right path if bandwidth becomes a concern. Per-draw-call hashing (Pivot B from MASTER-PLAN) is the smaller-step version. ~1-2 weeks.
> - **#2 Browser-Side SH4 Prediction (rollback streaming)** — **promoted to OPTIMIZATION-PLAN.md item #6** as the moonshot. The native client equivalent (run a flycast SH4 alongside the live mirror, predict locally, snap on miss) is what would take input-to-pixel toward 0ms. Estimated 2-4 weeks. **Most valuable idea on this list.**
> - **#3 Spectator P2P Mesh Ring** — not implemented. Tractable (3-5 days) and decoupled from the gameplay hot path; revisit when spectator counts justify it.
>
> **Status of each "Rest of Wild Bag" idea (#4-#15):** all unimplemented, preserved for future direction. Ideas #8 (state-delta), #11 (WebTransport datagrams), #12 (palette overlay) are days of work each and could be cherry-picked into the latency-first push without disrupting the main plan.
>
> **What this exercise was actually for:**
> Option 6 set out to dedupe TA frames for bandwidth savings. The real lesson was that **bandwidth wasn't the user-visible problem; latency was.** Putting a server geographically closer (EWR community node) saved more wall-clock perceived latency than any cache scheme would have, with zero new architecture. Sometimes the right answer is to put a $6/mo VPS in the right city.

---

> Pivot A produced 0% hit rate on MVC2 (timer + HUD + animation tick make every TA buffer unique). Closing out Option 6 on the cheap is one path. Going *insane* is the other. This doc curates the wildest tractable ideas across the codebase, MVC2 reverse-engineering community, neural rendering frontier, and rollback-netcode prior art. Sorted by leverage × tractability.

## The Big Three

### 🥇 JIT Sprite Atlas + CAS Storage + Asymptotic Bandwidth

**The pitch:** Cold-start with an empty sprite atlas. Every frame `serverPublish` already detects VRAM dirty pages. Add a sprite-boundary heuristic — texture-page-aligned + palette-indexed — and hash every newly-decoded sprite (xxhash) into a content-addressed store at the hub. Server emits `{sprite_id, sha256}` references; client downloads sprites from CAS on demand and caches forever. After ~50 hours of *aggregate* play across all clients, the atlas converges. From then on, bandwidth is **just (input + position + palette deltas + occasional novel sprites)** — asymptotes to **50-200 Kbps**. From 4 Mbps. **20× reduction.**

**What's already built:**
- Dirty-page tracking in `maplecast_mirror.cpp::serverPublish` ✓
- xxhash linked, used in `core/rend/TexCache.cpp` ✓
- The 2026-04 prototype's `maplecast_visual_cache.cpp::captureTexture()` decodes VQ/paletted/twiddled → RGBA at the right hook point (post-`UploadToGPU`) ✓
- SurrealDB at the hub with tables for skins (`skin{ char_id, hash, palette_hex }`) — natural extension to `sprite{ id, sha256, atlas_chunk_id }` ✓
- 5,202 community palette entries already indexed ✓

**Wildcard combinator:** the atlas is **content-addressed**, so legal distribution piggybacks on the ROM-hash verification already in Phase 7 (`048b2b9f1`). Encrypt atlas chunks with a key derived from ROM SHA-256. Only legitimate ROM owners can decrypt. Sidesteps DMCA on sprite distribution.

**Cost:** 1-2 weeks. Measurable converging bandwidth from day one — no all-or-nothing rollout.

**Prior art:** Sean Middleditch's CAS storage notes (https://seanmiddleditch.github.io/asset-storage-cas/), Unity Addressables remote bundles, EmulatorJS IPFS integration. Reassembler's Sega System 16 emulator-memory sprite extraction (closest analog: http://reassembler.blogspot.com/2021/04/ripping-sega-system-16-sprites-palettes.html).

---

### 🥈 Browser-Side SH4 Prediction (Rollback Streaming)

**The pitch:** Run a partial flycast SH4 simulator in a Web Worker on the client. When the user presses a button, **predict** the next frame locally and render immediately. When the authoritative server frame arrives ~7ms later, snap to it. On cache-hit frames (95%+ for prediction matching), input-to-pixel latency goes to **0ms** — actually, slightly **negative** because the client renders before the server's frame even arrives. **Eliminates the 7ms E2E entirely.**

This is GGPO-style rollback netcode (https://www.ggpo.net/) but applied to streaming. **Nobody in production cloud gaming does this** — Stadia, GeForce Now, Parsec all use frame-by-frame video transport. We'd be first.

**What's already built:**
- Flycast SH4 is byte-perfect deterministic (`MAPLECAST_DUMP_TA=1` rig already validates this) ✓
- Windows native mirror client runs the SH4 alongside the live mirror (`maplecast_player.cpp` shelved branch) — proves the prediction path is fundable ✓
- 253-byte gamestate gives us ground truth for snap detection ✓
- Replica client (`maplecast_replica.cpp`) already does "fast-forward SH4 with rendering disabled" — exactly the rollback motion ✓

**The wild part:** ship a *partial* SH4 — not the full Dreamcast. Just simulate "input → next-frame char-position deltas." The expensive stuff (TA generation, AICA audio, BIOS) stays server-side. Client only predicts the 64 bytes of state that matter for visual reproduction.

**Wildcard combinator:** combine with idea #1 — once the atlas is converged, the client doesn't need TA buffers from the server at all. Just input + state predictions, with state snaps from the server. Bandwidth drops further to **~10 KB/s.**

**Cost:** 2-4 weeks. Risk: prediction divergence under network glitches. Mitigation: short rollback window (4-8 frames max), full snap on miss.

**Prior art:** GGPO rollback architecture (https://github.com/pond3r/ggpo), SnapNet's rollback writeup (https://www.snapnet.dev/blog/netcode-architectures-part-2-rollback/), infil.net's "Fightin' Words" on visual-state exclusion (https://words.infil.net/w02-netcode-p5.html).

---

### 🥉 Spectator P2P Mesh Ring

**The pitch:** Tournament viewers attach via WebRTC DataChannel mesh — first viewer pulls from VPS, viewers 2..N receive from neighbors. Relay bandwidth flat-lines regardless of audience size. **At a 500-spectator EVO grand finals**, our VPS would only ever serve the same 4 Mbps it does today, while the audience self-organizes into a peer mesh.

**What's already built:**
- `core/network/maplecast_webrtc.cpp` has the full DataChannel plumbing — `pc->onDataChannel()` for video/input/audio/gamestate, `peer->videoDc->send()` already wired ✓
- The hub (`hub_discovery.cpp`) has parallel UDP probing infrastructure with RTT measurement — natural fit for "find your nearest peer" ✓
- The relay already speaks WebTransport (QUIC) for ultra-low-latency primary path ✓

**What's missing:** the hub doesn't negotiate SDP offer/answer between peers. Three days of work to wire that.

**Wildcard combinator:** the mesh ring **also distributes the sprite atlas chunks** (idea #1). Late-joining spectators get atlas chunks from their nearest peer instead of the VPS. **Doubles down on bandwidth elimination at scale.**

**Cost:** 3-5 days. Doesn't touch the gameplay hot path (players stay on direct relay). Pure spectator-tier scaling.

**Prior art:** Fraunhofer 2025 WebTransport game-streaming validation (https://dl.acm.org/doi/10.1145/3744725.3744726). EmulatorJS IPFS peering. Mesh networks for ≤8 nodes are well-studied.

---

## The Rest of the Wild Bag

### 4. WebGPU Compute Shader TA Binner

Port `web/webgpu/ta-parser.mjs` (~600 lines of JS TA decode) to a WGSL compute shader. Server still ships TA buffers; client GPU-decodes them in parallel. Saves ~0.2ms per frame. Frees up the JS thread for predictive rendering (idea #2). Already has the WGSL infrastructure in `shaders.mjs`. **Cost: 3-5 days. Savings: 0.2ms latency.** Not a bandwidth play, a CPU play.

### 5. Neural Texture Compression for the Atlas

NVIDIA's RTXNTC achieves **8.5× compression** on textures (6.5 GB → 970 MB on a real game) with on-sample neural decode. WebGPU can run the decoder. Take the converged atlas from idea #1 (~50-300 MB raw) and ship it at **6-35 MB** with the same quality. **First download stays under 50 MB.** Source: https://github.com/NVIDIA-RTX/RTXNTC.

### 6. Hybrid Neural HUD Renderer

The reason Pivot A failed is the timer + HUD + animation per-frame variation. **Train a tiny VAE only for HUD elements.** Sprite atlas handles characters/stages (idea #1). Per-frame, server sends `{timer: 99, p1_meter: 3.5, combo_count: 14}` (10 bytes), client neural-renders the HUD. Microsoft Muse showed this works for full Quake at 10fps; a HUD-only model is **10× simpler**. Decode latency: ~5ms in WebGPU on midrange GPUs.

### 7. Sprite Atlas via IPFS with Peer Seeding

Combine ideas #1 and #3: the atlas is content-addressed → publish it via IPFS → every browser client peer-seeds the chunks they have. Late-joiners pull from peers, not VPS. **Tournament viewers become a CDN.** EmulatorJS already runs this pattern with ROMs and box art (https://github.com/linuxserver/docker-emulatorjs).

### 8. State-Delta Encoding for the 253-Byte Gamestate

The 253-byte gamestate ships every frame as raw bytes. Most frames change ≤10 bytes (timer ticks, HP changes on hit, combo increments). XOR-delta encoding + run-length compression drops average size to ~10 bytes per frame. **Saves 14.6 KB/s upstream.** Trivial work — 1 day. Multiplies with everything else.

### 9. PalMod-Driven Atlas Pre-Bake

ZachD's PalMod (https://github.com/Preppy/PalMod) has a complete map of MVC2 ROM offsets for sprite + palette tables. **Skip the runtime extraction entirely** — wire PalMod's data tables into a build-time tool that produces a complete atlas in one ROM scan. Then ship as the initial download. **Combines with #1 as the warm-start vs. cold-start choice.**

### 10. Diffusion Model for Unknown Sprite Generation

Train a tiny diffusion model on the atlas. When the client sees a `sprite_id` it doesn't have in cache, **generate it on-device** via 4-step DDIM (~10ms on midrange GPU). Server doesn't need to ship novel sprites at all — the model fills the gap until the legit asset arrives. GameNGen (https://gamengen.github.io/) proved this for full Doom; MVC2's deterministic visual grammar is far simpler.

### 11. WebTransport Datagram Mode for Lossy State Updates

The per-frame 253-byte gamestate doesn't *need* TCP reliability. Ship it as **WebTransport unreliable datagrams** (already wired via the relay's QUIC stack from `2d3ee5737`). Saves ~30% on transport overhead, eliminates head-of-line blocking. Player inputs stay on reliable datachannel.

### 12. Skin-System-Style Palette Override Layer

The skin system already proves a 32-byte palette swap is the right granularity. **Extend the same mechanism to per-sprite-frame palette deltas.** Most fighting-game palette variation is "this character flickers white when hit" or "super-flash darkening" — both compressible to a 4-byte palette index reference + tint amount. Replaces 100+ bytes of TA palette state with 4 bytes per affected sprite per frame.

### 13. Content-Hash Texture Cache Mode in TexCache

`core/rend/TexCache.cpp` has all the xxhash infrastructure but keys on `(VRAM_addr, TCW)` not on content. **Add a content-hash mode** so palette-swapped characters reuse texture geometry across all 5,202 community skins. **Eliminates re-upload on skin changes** — currently re-uploads ~100KB per costume swap.

### 14. WHAMM-Style In-Browser Generative Renderer

Microsoft WHAMM (April 2025) generates Quake II at 10fps in-browser via MaskGIT, trained on **just 1 week of gameplay data** (https://www.microsoft.com/en-us/research/articles/whamm-real-time-world-modelling-of-interactive-environments/). MVC2's visual grammar is far simpler. Recording 1 week of gameplay across the existing match archive could train a model that **renders from the 253-byte state alone** — no atlas required. **6-12 months out**, but the data collection can start now.

### 15. SH4 Dynarec → WebGPU Compute

The actual moonshot: port flycast's SH4Recomp (`c843320c5 feat: SH4Recomp integration + skin picker fix + queue auto-reclaim`) to WebGPU compute shaders. **Run the entire Dreamcast on browser GPU.** Server only ships inputs; client runs MVC2 locally at 60fps native speed. Bandwidth: input only (~1 KB/s). Latency: zero. Time horizon: 6-12 months. But once it works, it changes the entire architecture — server becomes optional.

---

## Tractability Matrix

| Idea | Bandwidth win | Latency win | Complexity | Time |
|------|--------------|-------------|------------|------|
| 1. JIT sprite atlas + CAS | **20×** | none | medium | 1-2 weeks |
| 2. SH4 prediction rollback | 0 directly | **7ms→0ms** | high | 2-4 weeks |
| 3. Spectator P2P mesh | **flat at scale** | <1ms (mesh hop) | low | 3-5 days |
| 4. WebGPU compute TA | none | 0.2ms | medium | 3-5 days |
| 5. Neural texture compression | 8× on atlas | none | medium | 1 week |
| 6. Hybrid neural HUD | 5-10× on HUD | -5ms (decode) | medium | 2 weeks |
| 7. IPFS peer-seeded atlas | infrastructure | none | medium | 1 week |
| 8. State delta encoding | 97% on gamestate | none | trivial | 1 day |
| 9. PalMod pre-bake | infrastructure | none | low | 3 days |
| 10. Diffusion fallback sprites | atlas robustness | -10ms | high | 2-4 weeks |
| 11. WebTransport datagrams | 30% transport | <1ms | low | 2 days |
| 12. Per-sprite palette overlay | 25× on palette | none | low | 3 days |
| 13. Content-hash TexCache | skin-swap saves | none | medium | 1 week |
| 14. WHAMM-style generative | atlas-free | -50ms (decode) | very high | 6-12 mo |
| 15. SH4 → WebGPU compute | **input-only** | **0ms** | extreme | 6-12 mo |

## Recommended "Insane But Achievable" Stack

If we wanted to pull off a **GDC-talk-worthy** demo in 4-6 weeks:

```
Week 1: idea #8 (state delta) + #11 (datagrams) + #3 (P2P mesh)
        ↓ low-risk warmup, immediate wins

Week 2-3: idea #1 (JIT atlas + CAS)
          ↓ the foundation; bandwidth converges

Week 4-5: idea #2 (SH4 prediction) — partial, char-position only
          ↓ latency goes negative on cache-hit frames

Week 6: idea #5 (NTC on atlas) + #9 (PalMod warm-start)
        ↓ initial download under 30 MB

Result: ~50 Kbps bandwidth, <0ms latency on 90%+ of frames,
        atlas-converged-in-a-week, scales to thousands of viewers
        without VPS upgrades. Closes the gap between "cloud
        gaming" and "running native."
```

## What This Achieves

Pivot A's failure was the wrong abstraction for fighting-game bandwidth. The right abstraction is **hash sprites, not frames**. With the JIT atlas + state delta + prediction triplet:

- **Bandwidth: 4 Mbps → 50-200 Kbps** (20-80× reduction)
- **Latency: 7ms E2E → 0ms** (cache-hit frames)
- **Scale: 500 spectators on the same VPS**
- **First download: under 50 MB after NTC compression**
- **Atlas converges in <50 hours of community play**

And every piece has *some* prior art. None of this is unprecedented — we'd just be the first to wire all of it together for an arcade fighter.

The honest question: do we want a research paper or a production system? The Big Three would land us a SIGGRAPH submission. **Or we close out at 4 Mbps, ship the master-HEAD fixes to prod, and call it a day.**
