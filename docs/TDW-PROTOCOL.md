# TDW-PROTOCOL.md — the normative TDW wire specification

**Status: NORMATIVE (2026-07-15).** Every TDW parser (server encoder in
`core/network/maplecast_mirror.cpp` `tadict::`, the native client
`native-client*/src/tdw.rs`, the Python reference `_bwlab/tadict_gate_live.py`,
and future browser/relay implementations) MUST match this document. Campaign
record + measurements: docs/TA-DICT-WIRE-PLAN.md, docs/TDW-GOLD-STANDARD.md.
All integers little-endian.

---

## 1. Message types (WebSocket binary, own outer magics)

Never introduce a new inner type under the `ZCST` outer — legacy consumers
decompress every ZCST message and pattern-match the inner (learned the hard
way; see the §2b lesson in TA-DICT-WIRE-PLAN.md). Unknown OUTER magics are
dropped by all legacy clients and pass the relay as Critical (never dropped,
strictly ordered) — the delivery class a dictionary stream requires.

### 1a. `TDW1` — the per-frame message
```
off 0   'T','D','W','1'
off 4   dictEpoch   u8      dictionary generation; client hard-resyncs on change
off 5   flags       u8      bit0 = zstd streamStart
                            bit1 = TA is TACANON dead-byte-masked (descriptive)
                            bit3 = camera block present in inner
                            bit4 = page section present in inner
                            bit5 = PVR reg snapshot present in inner
                            bits 2,6,7 reserved (0)
off 6   seq         u16     per-epoch counter; a gap ⇒ desync until next streamStart
off 8   innerSize   u32     decompressed inner length
off 12  streaming-zstd chunk: ONE persistent ZSTD stream (window log 24, level 3),
        flushed once per message; each message decodes to exactly innerSize bytes.
        Stream restarts (flags bit0) at: encoder creation, SYNC broadcasts, dict
        epoch bumps, MAPLECAST_TADICT_RESET cadence (default 0 = SYNC-only).
```

Inner payload layout (in order):
```
off 0   frameNum    u32     server publish counter
off 4   vframe      u32     game frame counter 0x8C3496B0 (STARTRENDER-stamped)
off 8   taSize      u32     reassembled TA byte length (= Σ ref'd block sizes)
off 12  nBlocks     u32
off 16  newSection  u32     byte length of the newBlocks section (incl. length prefixes)
[bit3]  camera      132 B   stage_id u32 + M2 16×f32 (guest 0x8C2D6AD8, raw RAM
                            dword order = column-major) + M1 16×f32 (0x8C2D6B18)
[bit5]  pvrRegs     64 B    the legacy inner-header PVR snapshot (bytes 8..72 of
                            the legacy frame); reg[0] = tile counts → the
                            client projection matrix. REQUIRED for rendering.
        refs        nBlocks × u32   dictionary ids, TA emit order
        newBlocks   repeat { u8 len (32|64), len bytes }   first-appearance order
[bit4]  pageSection the legacy dirty-page layout VERBATIM:
                            u32 count | 0xFFFFFFFF (VCACHE sentinel, then u32 count)
                            entries: plain  = regionId u8, pageIdx u32, 4096 B
                                     vcache = regionId u8, pageIdx u32, hash u64,
                                              hasData u8 [, 4096 B]
                            regionId 1 = VRAM, 3 = PVR regs (palettes)
[tail]  E2EP        32 B    self-locating (last 4 bytes == 'E2EP'), optional:
                            frameNum u32, t_latch i64, t_publish i64,
                            latchedSeq[slot0] u32, [slot1] u32, 'E2EP'
```

**Decode algorithm (normative):** iterate `refs` in order; `id == dict.len()`
⇒ pop the next newBlock (append to dict, emit it); `id < dict.len()` ⇒ emit
`dict[id]`; `id > dict.len()` ⇒ desync. Concatenation of emitted blocks = the
frame's TA stream, byte-exact, in engine emit order. The page walk is
count-driven and MUST ignore trailing bytes (the E2EP tail).

### 1b. `TDWS` — dictionary snapshot
```
off 0  'T','D','W','S'
off 4  uncompressedSize u32
off 8  one-shot zstd blob →
   inner: 'T','D','W','S', dictEpoch u8, pad u8×3,
          blockCount u32, sectionBytes u32,
          repeat { u8 len, len bytes } × blockCount   (id = index)
```
Sent: on client join (forced SYNC path) and on dict epoch bumps. A client that
receives TDWS(E) decodes TDW1 only from the next flags-bit0 message with
dictEpoch E.

### 1c. Seed (transitional): legacy `ZCST`-wrapped `SYNC`
One-shot at connect: full VRAM (8 MB) + PVR regs (32 KB), zstd. The client
applies it for `have_sync`; all subsequent state arrives via TDW1. This is the
only legacy message a TDW client consumes; a TDW-native seed replaces it when
the legacy legs retire server-side.

## 2. Block grammar

Blocks are the PVR TA's native 32-byte parcels (64-byte for the two-parcel
vertex/param types), walked with the taCanonicalize FSM
(`maplecast_mirror.cpp` — any FSM change must update server walker, client, and
`_bwlab` reference together). With MAPLECAST_TACANON=2 (REQUIRED in production:
unmasked live play churns ~1,800 blocks/frame of dead-byte scratch) the six
validated dead-byte ranges are zeroed before encoding; flags bit1 says so.

**Keep-rule v4 (MAPLECAST_TADICT_PLAYERS=1):** drop paraType-4 polys (and their
vertices) whose `tcw & 0x1FFFFF ∈ {0x9FC00, 0xA0000}` (the stage allowlist —
the measured 89.6 % of dictionary churn; per-stage lists arrive with the
17-stage sweep). EVERYTHING else rides: all sprites (bodies, satellites,
effects, fonts/menus/timer), body-bank polys, TR dynamics (untextured overlays,
shadow banks 0x8E/0x8F), real HUD polys. `taSize` in the inner is the filtered
size. The client renders the reassembled TA directly and draws the stage
locally (`.mcstg` bake re-projected through the in-band camera with full
guard-volume clipping: w ≥ 1e-4 AND |X|,|Y| ≤ 4096·w, Sutherland-Hodgman in
pre-divide space).

## 3. Dictionary lifecycle
- Ids are dense insertion order, never reused within an epoch; the dict
  persists across zstd stream restarts.
- Caps: MAPLECAST_TADICT_MAXBLOCKS (default 1,048,576) / _MAXMB (default 64).
  Breach ⇒ dictEpoch++, clear, TDWS broadcast, streamStart.
- Server intern: FNV-1a 64 over block bytes → id map with memcmp confirm.
- PLANNED (persistent-cache milestone): content-hash ids replace insertion
  ids so client caches survive reconnects/restarts — a protocol rev (TDW2).

## 4. Error handling (normative)
- seq gap, epoch mismatch, inner-size mismatch, ref > dict, section overrun:
  desync — discard until the next TDWS (if epoch unknown) + streamStart.
- Encoder-side zstd error: skip the message, queue a stream restart.
- The transport must be lossless and ordered for TDW1/TDWS (relay: Critical).

## 5. Server gold configuration
```
MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1 MAPLECAST_HEADLESS_AUTOLOAD=1
MAPLECAST_TADICT=1 MAPLECAST_TADICT_PLAYERS=1 MAPLECAST_TACANON=2
MAPLECAST_TDW_ONLY=1 MAPLECAST_E2E_PROBE=1
```
MAPLECAST_TDW_ONLY gates the legacy ZCST deltas and every side channel
(GSTA/PALF/WTCH/OBJS/OBJF/EFCT/TXTR) off the socket; SYNC one-shots remain.
Measured socket composition in this mode: TDW1 + one SYNC at connect + TDWS.
