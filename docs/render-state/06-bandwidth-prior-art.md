# RENDER-STATE appendix 06 — Sub-1 Mbps TA-stream: prior-art survey & ranked stack

> Produced 2026-07-08 by the bandwidth research agent. Companion: the empirical compression-lab
> report (`_bwlab/REPORT.md`, gitignored corpus; conclusions to be folded here).
> Wire under study: ~4.1 Mbps ≈ 8.5 KB/frame @60fps; 73% geometry (byte-run delta of 140–230KB
> TA buffer, ~100–180 quads × 32-byte parcels), full keyframes every 60 frames, dirty 4KB VRAM
> pages, zstd-1 envelope. Budget: ~1ms/frame encode on 2 vCPU, zero quality loss.

## 1. Geometry / mesh compression

### meshoptimizer vertex stream codec (Kapoulkine) — THE DIRECT HIT
Lossless codec for fixed-stride vertex buffers: byte-deinterleaves (SoA), delta-codes consecutive
vertices, output stays highly compressible by a stacked general-purpose compressor. Stride must be
a multiple of 4 — a 32-byte TA parcel qualifies exactly.
Measured: 2–4× on quantized packed vertex data, decode 3–6 GB/s (github.com/zeux/meshoptimizer).
Aras Pranckevičius benchmark: 94.5MB floats → 29.5MB alone (beats plain zstd), → 24.4MB with zstd
stacked, 3× faster decode than filtered+zstd (aras-p.info 2023-02-02). Pre-filtering before it HURTS.
**Verdict: DIRECT.** WASM decoder exists (glTF EXT_meshopt_compression). Expected 1.5–3× on the
geometry portion on top of zstd-1. Cost: days.

### Google Draco / Corto — INAPPLICABLE
Designed for one-shot delivery of connected 3D meshes; reorders vertices (a correctness bug for
blend order), decode is ms-per-model not GB/s. Corto: ~30% faster encode than Draco, ~48% larger.

## 2. Entropy coding beyond zstd-1

### zstd trained dictionaries — DIRECT, cheap
Train offline on real TA-delta payloads; gains concentrate in the first few KB — our frames are
8.5KB, the sweet zone. Web-context headline numbers (539KB→10KB) are previous-version-as-dictionary
(we already exploit that via explicit deltas); expect the modest end for novel content: 10–30%.
Also raise the level: at 80µs current spend and ~1ms budget, zstd 6–12 is affordable.
**Combined expected: 15–35%. Cost: days.**
**CHEAPEST EXPERIMENT ON THE LIST: verify whether serverPublish uses a persistent
`ZSTD_compressStream` context or a fresh per-frame compress — a streaming window spanning prior
frames could capture the 6–10-frame idle-loop periodicity FOR FREE.**

### zstd long-distance matching (--long) — via the streaming-context route only
LDM's value = frame N matching frames N-6..N-10 inside one stream. 0–40% on geometry, speculative; measure.

### rANS/FSE raw — INAPPLICABLE standalone (zstd already IS FSE+Huff0+LZ; single-digit % for weeks).
### Brotli custom dictionaries — dominated by zstd-dict for a 60Hz server encode. Skip.
### OpenZL (Meta, 2025) — ADAPTABLE: format-aware transform graphs (parse→transpose→delta→entropy),
15–30% over zstd on structured data (paper numbers, upper bound). Spike only if manual transpose
doesn't land the win. ~1 week.

## 3. Video-codec ideas on command streams — THE BIG ONE

### LiveRender lineage (HKUST MM'14/ToN'16)
Intercepts D3D command streams, inter-frame command compression + geometry caching.
Measured: 52–73% bandwidth reduction vs raw graphics streaming; 40–90% less than H.264 at equal
quality (dl.acm.org/doi/10.1109/TNET.2015.2450254; github.com/CGCL-codes/LiveRender). Successor
gRemote (JSA 2021): command-characteristics-oriented compression beats LZ77 by >30%.
**Verdict: validates the whole ship-commands architecture. What we LACK vs them: SEMANTIC
inter-frame coding — match commands to previous-frame counterparts, code parameters
differentially, instead of byte-diffing a buffer.**

### AV1-style motion compensation / multi-reference prediction on quads — HIGHEST CEILING
Per-object coding: "quad i = reference frame k's quad j translated (dx,dy)". No published system
does this for TA parcels; closest priors = LiveRender inter-frame mode and Quake 3's
delta-vs-acknowledged-snapshot netcode (delta against any of the last 32 baselines +
static-Huffman residual — fabiensanglard.net/quake3/network.php).
Our own facts make the case: sprites TRANSLATE (4 verts share one (dx,dy); u,v/color/TCW unchanged
→ ~6–10 B/quad vs ~100+ B of byte-run churn today, since a float x-translate dirties bytes in all
4 verts × 2 coords), and idle loops (period 6–10) mean a quad absent from N-1 is usually
byte-identical at N-6..N-10 — a 10–16 frame reference ring catches them exactly.
Encoder: hash each 32-byte parcel (or per-quad key on TCW+UV) into the ring; emit
MATCH(ref,idx,dx,dy) | VERBATIM. O(#quads)/frame, well inside 1ms for 180 quads. Lossless by
construction (VERBATIM always available).
**Expected: 3–6× on the geometry 73% — the only single technique that plausibly reaches <1 Mbps
alone. Cost: 1–2 weeks incl. all four parsers + determinism rig.**

### Kill/shrink the periodic keyframe — DIRECT, days
On reliable transport nothing is lost; the 140–230KB/60-frame keyframe exists only for mid-stream
join — which the relay can serve from cached SYNC instead (Q3 model: delta against last
acknowledged state). Kahawai/Outatime are pixel-domain — inapplicable.

## 4. Remote-display protocols — nothing to import, only confirmation
RDP abandoned command remoting for bitmap codecs because general apps lost vector structure
(RemoteFX vGPU deprecated 2020). Parsec/Moonlight/Sunshine = pure hardware video, 5–150 Mbps,
need a GPU. waypipe = damage-diff+zstd, same shape we have. **The industry retreated to pixels
because GENERAL apps lost structure; a fixed, fully-RE'd game is the one case where command
remoting wins — and our 4.1 Mbps already beats every pixel protocol at zero loss.**

## 5. Texture side
- Content-addressed one-time shipping: steady-state texture cost already ~zero (0 misses / 24k
  frames); gains limited to join-time + the dirty-page tail. Measure the split first.
- BC7 etc.: INAPPLICABLE — lossy, and 4bpp palette-indexed sprites would INFLATE to 8bpp and
  break palette-swap skins.
- NVIDIA NTC: INAPPLICABLE — lossy, GPU inference, targets fat PBR stacks; a 16-color sprite is
  already ≤4bpp mathematically perfect.

## 6. Transport
WebTransport is Baseline 2026 (Chromium/Firefox/Safari 26.4). QUIC removes TCP head-of-line
blocking (one lost packet no longer stalls every later frame ≥1 RTT). FEC = drafts only; do
app-level XOR/RS over datagrams if needed. **Interaction warning: unreliable datagrams would force
keyframes BACK for loss recovery — the clean design is WebTransport with a reliable stream (or
per-frame streams) + Q3-style ack-based reference selection.** Latency-tail gain, zero bandwidth
gain. 1–2 wk (relay is Rust — wtransport/quinn mature; nginx can't proxy it, needs a UDP port).

## 7. Wildcards
- GGPO/Fightcade: input-streams ~30 kbps — the 0.05 Mbps endgame already pursued via
  render-replica/lockstep. Transferable piece: acked-baseline delta.
- Dolphin FIFO logs: the one public console-GPU-dump corpus; stored for regression, no
  compression results. Nothing to borrow.

## Ranked top-5 stack (4.1 → <1 Mbps, zero loss)

Assume geometry ≈ 3.0 Mbps, keyframes+textures+regs ≈ 1.1 Mbps (lab report will pin the split).

| # | Technique | Target | Expected | Cost | Risk |
|---|-----------|--------|----------|------|------|
| 1 | Semantic quad-delta w/ multi-ref ring (10–16 frames): MATCH(ref,idx,dx,dy) vs VERBATIM | geometry 3.0 Mbps | **3–6× → 0.5–1.0 Mbps** | 1–2 wk (4 parsers + rig) | Medium; VERBATIM fallback keeps it lossless by construction |
| 2 | Drop periodic keyframes; relay serves cached SYNC to joiners | keyframe share | removes outright (est. 0.3–0.8 Mbps) | days | Low |
| 3 | SoA byte-plane transpose (meshopt-style) on VERBATIM quads + residuals before zstd | remaining geometry | 1.5–2× on that residue | days | Low |
| 4 | zstd: persistent streaming context first (free), then trained dictionary + level 6–12 | everything | 15–35% | days | Near-zero |
| 5 | WebTransport for the latency tail (after 1–4) | p99 frame-time | latency, not Mbps | 1–2 wk | Medium (infra) |

**Plausibility:** layers 1+2 ≈ 0.8–1.3 Mbps; +3+4 → **~0.6–1.0 Mbps**. Every layer independently
revertible and measurable against the determinism rig; nothing lossy anywhere.

**Falsify-first experiments:** (a) zstd streaming-context vs per-frame compression on a captured
corpus; (b) per-quad byte-churn histogram between frames to confirm translate-dominates before
committing to layer 1. (Both assigned to the compression-lab agent — see `_bwlab/REPORT.md`.)

**Skepticism notes:** OpenZL 15–30% and 95%-dictionary headlines are best-case; LiveRender 52–73%
is peer-reviewed but on D3D9 MMO workloads — a translate-dominated sprite workload should do
BETTER; NTC solves a problem we don't have.
