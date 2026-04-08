# MapleCast Input Latch — The Dual-Policy Model

> **TL;DR for humans:** MVC2 reads your controller once per frame (every
> 16.67 ms). Network input arrives continuously and randomly. We give players
> two policies for how to bridge that gap: **LATENCY** (instantaneous, lowest
> delay, can drop very-fast taps) and **CONSISTENCY** (preserves every press
> even if it lands between two reads, costs up to 1 frame of delay). Both ship,
> per-slot toggle, players choose what feels right.

> **TL;DR for future agents:** the input latch path lives at
> [core/network/ggpo.cpp:35](../core/network/ggpo.cpp#L35) `getLocalInput()`.
> It's called once per vblank from `maple_DoDma()` at
> [core/hw/maple/maple_if.cpp:153](../core/hw/maple/maple_if.cpp#L153). The
> policy fork is read-time only; both policies coexist behind a runtime gate
> and **the LATENCY path is bit-perfect identical to pre-Phase-B behavior**
> (verified by the determinism rig). Don't add a third buffer layer; the
> existing two (`_slotInputAtomic` for the live word, `mapleInputState[]` for
> the frame-stable snapshot) are sufficient. Read this entire doc before
> touching anything in the latch path.

---

## 1. The Hardware Truth: CMD9 Once Per Frame

The Dreamcast Maple Bus protocol reads controllers via the **CMD9
GetCondition** command. The game writes `STARTRENDER` to the PVR registers
to start drawing a frame, and at the same time the SPG (sync pulse generator)
schedules a vblank-out interrupt. When that interrupt fires, the maple bus
DMA controller pulls the most recent button state from each connected
controller and hands it to the game.

The key fact: **this happens exactly once per frame**, at the vblank boundary,
roughly every 16.67 ms (60 fps). The game has no continuous view of your
controller. Whatever your buttons happened to be at that one specific instant
is what the game sees for the entire upcoming frame.

```
t = 0 ms       16.67 ms       33.34 ms       50.01 ms
│              │              │              │
▼              ▼              ▼              ▼
[VBLANK]------[VBLANK]------[VBLANK]------[VBLANK]----
   │             │             │             │
   reads         reads         reads         reads
   buttons       buttons       buttons       buttons
   for           for           for           for
   frame N       frame N+1     frame N+2     frame N+3
```

This is **arcade-perfect hardware behavior**. Modern fighting games (SF6, GG
Strive, etc.) read inputs multiple times per frame and have multi-frame
buffering precisely because their developers understood this limitation. MVC2
didn't, so we have to work around the once-per-frame sample on the server side.

---

## 2. Where The Bug Lives

In flycast, the controller-state-to-game path looks like:

```
network packet (NOBD UDP, browser WS, etc.)
  ↓
maplecast_input_server::updateSlot(slot, lt, rt, buttons)
  ↓
writes to live globals + an atomic snapshot
  ↓
                    [VBLANK FIRES]
  ↓
maple_vblank() → maple_DoDma()                          [maple_if.cpp:153]
  ↓
ggpo::getInput(mapleInputState)                         [maple_if.cpp:153]
  ↓
ggpo::getLocalInput(...) reads live state into          [ggpo.cpp:35]
mapleInputState[player]
  ↓
DMA loop walks CMD9 handlers:
  maple_sega_controller::dma(MDCF_GetCondition)         [maple_devs.cpp:185]
    → MapleConfigMap::GetInput(&pjs)                    [maple_cfg.cpp:73]
    → reads mapleInputState[playerNum()]
  ↓
DMA response packet built, game has buttons for the next frame
```

**Pre-Phase-B**, `updateSlot()` did three plain stores:
```cpp
kcode[slot] = buttons | 0xFFFF0000;  // OVERWRITE
lt[slot]    = (uint16_t)ltVal << 8;  // OVERWRITE
rt[slot]    = (uint16_t)rtVal << 8;  // OVERWRITE
```

Pure overwrite. **There was no record of any button transition that happened
between two vblanks.** If a player tapped → for 1 ms and then released, both
the press packet AND the release packet would land between two vblanks. By
the time the next vblank fired and the latch read `kcode[slot]`, it saw
`neutral` — the press was gone forever. The game never knew it happened.

This is the **dashing bug**. It was real. We measured it.

### The empirical proof — 50/50

[tests/dash_repeatability_test.cpp](../tests/dash_repeatability_test.cpp)
runs 50 synthetic dash sequences (press-then-release) at random offsets
within a single vblank interval. Under the pre-Phase-B `LatencyFirst` model,
**0 of 50 dashes survive to the SH4** because the release packet always
overwrites the press packet before the latch reads. Under the Phase-B
`ConsistencyFirst` model, **50 of 50 dashes survive**.

```
$ ./dash_repeatability_test
LatencyFirst:    0 / 50 dashes seen by SH4  (today's behavior — bug)
ConsistencyFirst: 50 / 50 dashes seen by SH4  (Phase B fix)
```

---

## 3. The Two Policies, Side by Side

### LatencyFirst (default — what most players use today)

The CMD9 latch reads the **most recent** packet's state directly. Whatever
the network thread last wrote is what the SH4 sees for this frame. This is
bit-perfect identical to pre-Phase-B behavior, modulo a separate atomic-read
fix that closed a torn-read race in the C++ memory model.

**Pros:**
- Lowest possible per-input latency (zero added delay)
- Bit-perfect deterministic against the historical wire baseline
- Best for fast NOBD sticks (~12,000 packets/sec) where the bug essentially
  cannot fire — your stick is always sending, the next packet always pre-empts
  the release before the next vblank

**Cons:**
- Vulnerable to the dashing bug for players whose packet rate is slower than
  vblank rate (browser gamepads at 60-250 Hz, lossy networks)
- Tap-and-release sequences faster than the inter-packet interval can vanish

**When to use it:**
- You're playing on a NOBD stick (12 KHz packet rate)
- You're on a clean LAN or low-jitter WAN
- You feel the game is responsive and you're not dropping inputs

### ConsistencyFirst (opt-in — for players who want guaranteed press registration)

The CMD9 latch drains a per-slot **input accumulator** that records every
button-press transition that occurred during the last vblank interval. Any
press that happened at any moment between the previous latch and this one
shows up as PRESSED in this frame's `mapleInputState[]`, even if the player
released the button before the latch fired.

A symmetric mechanism (`deferredReleaseMask`) ensures that "blip presses"
(press AND release in the same interval) get released on the *next* frame
rather than staying held forever. So a 1 ms tap becomes a guaranteed 1-frame
held press in-game, which is what the player wanted.

There's also a **guard window** (default 500 µs, configurable via
`MAPLECAST_GUARD_US`) that defers near-boundary network arrivals by exactly
one frame for predictability. Inputs that arrive within the guard before a
vblank get classified as "next frame" deterministically rather than racing
the vblank.

**Pros:**
- Every press transition the player intended is preserved
- Blip-press dashes always register
- Boundary-arrival jitter becomes deterministic +1 frame instead of random
- Best for browser players, lossy networks, or any environment where
  packet rate < vblank rate

**Cons:**
- Adds up to 1 frame of latency on inputs that fall within the guard window
- Adds up to 1 frame of latency on blip presses (the deferred release runs
  on the next vblank)
- Wire bytes differ from LatencyFirst for any frame where the policy actually
  fires (this is **expected** — it's why we keep the determinism rig
  baselined separately for each policy)

**When to use it:**
- You're playing on a browser gamepad (Gamepad API, 60-250 Hz)
- You're on a wifi/lossy network
- You can feel that some of your dashes don't register
- Your LATCH STATS (in the diagnostics modal) show `min delta_us < 1 ms`

---

## 4. The Architecture

### 4a. The atomic write side (single producer, no tearing)

The network thread writes to a single packed 64-bit atomic per slot:

```cpp
// core/network/maplecast_input_server.h
extern std::atomic<uint64_t> _slotInputAtomic[2];

inline uint64_t packSlotInput(uint16_t buttons, uint8_t lt, uint8_t rt, uint32_t seq) {
    return (uint64_t)buttons
         | ((uint64_t)lt << 16)
         | ((uint64_t)rt << 24)
         | ((uint64_t)seq << 32);
}
```

Layout: `[buttons:16][lt:8][rt:8][seq:32]`. The whole 64-bit word is updated
atomically with `memory_order_release`, so the latch can read it tear-free
with `memory_order_acquire`. The seq is incremented on every write so the
latch can tell whether a fresh packet arrived since the previous read.

**Why this matters:** the pre-Phase-B path used three plain `u32` stores
(`kcode[slot]`, `lt[slot]`, `rt[slot]`). On x86_64 those are CPU-atomic per
word but **not atomic across the three-word group**. Under the C++ memory
model, the latch could observe a torn triple (new buttons + old triggers, or
vice versa). The atomic packed slot closes that race. This was independently
verified by [tests/input_atomic_smoke.cpp](../tests/input_atomic_smoke.cpp)
running 19.89 BILLION reads against a 12 KHz writer with zero observed
torn snapshots.

### 4b. The accumulator (only used by ConsistencyFirst, but always populated)

Alongside the simple packed slot, the network thread ALSO updates a per-slot
accumulator that records button-press edges:

```cpp
// core/network/maplecast_input_server.h
struct AccumPacked {
    uint16_t any_pressed;   // OR of every bit that went 1→0 since last drain
    uint16_t any_released;  // OR of every bit that went 0→1 since last drain
    uint16_t current;       // most-recent kcode lower 16 bits
    uint8_t  lt;            // most-recent LT
    uint8_t  rt;            // most-recent RT
};
extern std::atomic<uint64_t> _slotAccum[2];
```

`updateSlot()` runs a CAS-loop that atomically reads the prior accumulator
state, computes `newly_pressed = (~buttons_in) & a.current` (active-low,
bits that went 1→0) and `newly_released = buttons_in & (~a.current)` (bits
that went 0→1), ORs them into the accumulators, and stores back.

**Why CAS:** the atomic load + edge compute + atomic store can't be a
straight 64-bit store because we need to OR-merge with whatever was already
there. The CAS retry handles the (extremely rare in practice) case where
two updateSlot calls race.

**Why the accumulator is updated even on LatencyFirst slots:** so that if
you toggle a slot from LATENCY to CONSISTENCY mid-match, the accumulator
already has live data. No stale-state surprises on policy switch.

### 4c. The latch read (the policy fork)

[core/network/ggpo.cpp:35](../core/network/ggpo.cpp#L35) `getLocalInput()`
runs once per vblank. For each maplecast slot (0 and 1), it forks on the
per-slot policy:

```cpp
const LatchPolicy policy = maplecast_input::getLatchPolicy(player);

if (policy == LatchPolicy::ConsistencyFirst) {
    // Drain accumulator + edge preservation + deferred releases + guard window
    const auto cr = maplecast_input::consistencyFirstLatch(player, tLatchUs);
    buttons = cr.buttons;
    lt = cr.lt;
    rt = cr.rt;
} else {
    // LatencyFirst: instantaneous packed atomic load
    const uint64_t packed = _slotInputAtomic[player].load(memory_order_acquire);
    unpackSlotInput(packed, buttons, lt, rt, seq);
}

state.kcode = (u32)buttons | 0xFFFF0000u;
state.halfAxes[PJTI_L] = (u16)((u16)lt << 8);
state.halfAxes[PJTI_R] = (u16)((u16)rt << 8);
```

`consistencyFirstLatch()` (in `maplecast_input_server.cpp`) is the heart of
ConsistencyFirst. The full sequence:

1. **Drain accumulator atomically** — read+CAS that clears `any_pressed` and
   `any_released` while keeping `current/lt/rt`. Returns the snapshot.
2. **Compute latched buttons under edge preservation:**
   ```
   latched = a.current & ~a.any_pressed
   ```
   (Active-low: clear any bit that was pressed at any moment during the
   interval. So a button that was pressed-then-released within one vblank
   appears PRESSED in the latch even though `current` has it released.)
3. **Apply previous frame's deferred releases:**
   ```
   latched |= p.deferredReleaseMask
   ```
   (Bits that were pressed-and-released last frame need to actually go back
   to released NOW. Without this, blip-presses would stay held forever.)
4. **Compute new deferred mask for next frame:**
   ```
   p.deferredReleaseMask = a.any_pressed & a.any_released
   ```
   (Bits that were both pressed AND released in this interval are blip-presses.
   They report PRESSED this frame and RELEASED next frame.)
5. **Apply guard window:** if a fresh packet arrived within `guard_us` of the
   latch and the accumulator had real activity, freeze at `lastLatchedButtons`
   and bump `guardHits`. This converts boundary-arrival jitter into a
   deterministic +1-frame mapping.

### 4d. Wire and frame phase publishing

[core/network/maplecast_mirror.cpp](../core/network/maplecast_mirror.cpp)
mirrors the frame counter and latch wall-clock time into atomics that the
status JSON broadcaster can read:

```cpp
static std::atomic<uint64_t> _atomicCurrentFrame{0};
static std::atomic<int64_t>  _atomicLastLatchTimeUs{0};
static std::atomic<int64_t>  _atomicFramePeriodUs{16670};  // EMA, live
```

The frame period is computed as a 1/16 EMA of `(this_publish - prev_publish)`,
so it converges on the real PVR rate (which is slightly off 60 Hz nominally)
within ~16 frames.

These get published in every WS status JSON broadcast as the `frame_phase`
block:

```json
{
  "frame_phase": {
    "frame": 12345,
    "t_last_latch_us": 192633644594,
    "t_next_latch_us": 192633661299,
    "frame_period_us": 16677,
    "guard_us": 500
  }
}
```

The browser uses this to **phase-align its gamepad sends** — see section 5.

### 4e. The full data flow

```
NETWORK THREAD (UDP/XDP/WebSocket)                       SH4 / MAPLE THREAD
─────────────────────────────────                       ──────────────────

packet arrives
   ↓
updateSlot(slot, lt, rt, buttons)
   ↓                                                    [VBLANK]
   ├─→ _slotInputAtomic[slot].store(packed, release)        ↓
   │      ← LatencyFirst reads this                    maple_vblank()
   │                                                       ↓
   ├─→ _slotAccum[slot] CAS-loop:                       maple_DoDma()
   │      a.any_pressed  |= newly_pressed                   ↓
   │      a.any_released |= newly_released              ggpo::getInput(mapleInputState)
   │      a.current = buttons_in                            ↓
   │      ← ConsistencyFirst drains this              ggpo::getLocalInput()
   │                                                       ↓
   ├─→ _players[slot].lastPacketUs = nowUs()           switch(getLatchPolicy(slot)):
   │   _players[slot].lastPacketSeq = ++seq                 │
   │                                                        ├─ LatencyFirst:
   └─→ kcode[]/lt[]/rt[] plain globals                      │     load _slotInputAtomic
       (legacy path for SDL local pads)                     │     unpack
                                                            │
                                                            └─ ConsistencyFirst:
                                                                  drainAccumulator
                                                                  edge preservation
                                                                  deferred releases
                                                                  guard window
                                                                  ↓
                                                       state.kcode = ...
                                                       state.halfAxes[L/R] = ...
                                                            ↓
                                                       recordLatchSample(slot,
                                                          deltaUs, seq, frameNum)
                                                       (telemetry ring buffer)
                                                            ↓
                                                       CMD9 handler reads
                                                       mapleInputState[playerNum()]
                                                            ↓
                                                       DMA response → game
```

---

## 5. Browser-Side Phase Alignment (the latency win for web players)

Without phase alignment, a browser's gamepad polling is rAF-aligned (~16 ms
cycle, randomly offset from the server's vblank). A packet you send arrives
at the server at a random point within the next vblank — anywhere from 0 to
16 ms before the latch reads. **Average packet age at latch time: 8 ms.**

With phase alignment, the browser knows **when the next server vblank will
fire** and schedules its sends to land 3-4 ms before that point.
**Average packet age at latch time: 3 ms.** Cuts effective input lag in half
for browser players, no server-side change required.

### How it works

Server publishes `frame_phase.t_next_latch_us` in every WS status broadcast.
Browser:

1. On every status frame, computes `serverClockOffsetMs = (server_us / 1000) - performance.now()`.
   1/16 EMA over recent samples to smooth out jitter.
2. Predicts when the next server vblank will fire in local clock terms:
   `localNextLatchMs = serverNextLatchMs - offset`.
3. Schedules a `setTimeout` to fire `(oneWayMs + guardMs + safetyMs)`
   before that point — typically 3-5 ms before the latch.
4. The timer fires `pollOnce()` with a forced-send override so a packet
   goes out even if button state hasn't changed.

The existing rAF burst-poll path (16 polls per vsync via MessageChannel)
keeps running for change detection. The phase-aligned send is an **additional**
heartbeat that ensures the latch always sees a fresh packet.

Implementation: [web/js/gamepad.mjs](../web/js/gamepad.mjs) `onServerFramePhase()`.

---

## 6. Why You Have a Choice

There is no single "right" policy. The dashing bug only manifests when your
**packet rate is slower than the vblank rate** AND your physical taps are
short enough to fit between vblanks. For different player setups:

| Player setup | Packet rate | Bug fires | Recommended policy |
|---|---|---|---|
| **NOBD stick (W6100 Ethernet)** | ~12,000 Hz | Essentially never | **LATENCY** |
| **NOBD stick (UART fallback)** | ~1,000 Hz | Rarely | **LATENCY** |
| **Browser gamepad on rAF** | ~60-250 Hz | Frequently on fast taps | **CONSISTENCY** |
| **Browser gamepad on wifi** | ~60 Hz, jittery | Frequently | **CONSISTENCY** |
| **Browser keyboard** | ~60 Hz, only on change | Rarely (most key presses are long-held) | Either |

**The key insight: ConsistencyFirst is not "better." It's a different tradeoff.**
For a player with a fast NOBD stick, ConsistencyFirst adds 1 frame of jitter
on near-boundary inputs without buying anything (because the bug never fires
on their hardware anyway). For a browser player on wifi, ConsistencyFirst
recovers presses that would otherwise vanish — at the cost of a predictable
+1 frame on the inputs that fall in the guard.

This is exactly what the GP2040-CE NOBD firmware figured out at a different
layer of the same problem. From their docs:

> *"NOBD doesn't give you a bigger window than original hardware — it gives
> you a smaller one. 5 ms vs 16.67 ms. Your presses need to be closer
> together than the arcade ever required. What NOBD removes is the lottery
> — the random chance that your correctly-timed input happens to straddle a
> frame boundary through no fault of your own."*

The MapleCast accumulator is doing the same thing the NOBD firmware does, but
on the server side, for any input source. NOBD-connected players already get
the benefit at the firmware layer (5 ms grouping); browser players get it for
the first time at the MapleCast accumulator (16.67 ms vblank-interval grouping).

---

## 7. Operator Reference

### Environment variables

| Var | Default | Effect |
|---|---|---|
| `MAPLECAST_LATCH_POLICY` | (unset) | `latency` (default), `consistency`. Sets BOTH slots' policy at boot. Per-slot overrides via WS at runtime. |
| `MAPLECAST_GUARD_US` | `500` | Guard window in microseconds, ConsistencyFirst only. 0 disables, max 5000. |

### WebSocket control message

```json
{ "type": "set_latch_policy", "slot": 0, "policy": "consistency" }
```

Server responds with:
```json
{ "type": "latch_policy_changed", "slot": 0, "policy": "consistency" }
```

And the next status broadcast reflects the new policy.

### Status JSON blocks

Every status broadcast (~1 Hz) includes:

```json
{
  "frame_phase": { ... },     // frame, t_last_latch_us, t_next_latch_us, frame_period_us, guard_us
  "latch_policy": { "p1": "latency", "p2": "consistency" },
  "latch_stats": {
    "p1": { "total_latches": ..., "latches_with_data": ..., "avg_delta_us": ..., "p99_delta_us": ..., "min_delta_us": ..., "max_delta_us": ..., "last_packet_seq": ..., "last_frame": ... },
    "p2": { ... }
  }
}
```

`latch_stats` ring is per-slot, 256 samples (~4.3 seconds at 60 Hz). Only
samples on vblanks where a fresh packet arrived are recorded; idle vblanks
don't pollute the histogram.

### Diagnostics modal (browser)

The unified gear icon at the bottom-right of the cabinet opens the
Settings/Diagnostics modal with four tabs:

- **GRAPHICS** — quality, post-processing, presets (was the old settings panel)
- **STATS** — server fps, ping, render performance, per-player input rate
- **LATENCY** — per-slot policy buttons (live A/B), frame phase, latch stats
  histogram, inline help text explaining what each metric means
- **INPUT** — gamepad info, slot info, future input mapping

Hotkeys (when the modal is open):
- **F1** — toggle P1 latch policy
- **F2** — toggle P2 latch policy
- **Esc** — close modal

### How to verify a policy is actually live

```bash
# From the VPS:
ssh root@<your-vps> "python3 -c '
import asyncio, json, websockets, time
async def main():
    async with websockets.connect(\"ws://127.0.0.1:7201\") as ws:
        end = time.time() + 4
        while time.time() < end:
            try: m = await asyncio.wait_for(ws.recv(), timeout=0.1)
            except asyncio.TimeoutError: continue
            if isinstance(m, str):
                j = json.loads(m)
                if j.get(\"type\") == \"status\":
                    print(j.get(\"latch_policy\"))
                    return
asyncio.run(main())
'"
```

---

## 8. Testing

Three test rigs cover the dual-policy implementation:

### `tests/input_atomic_smoke.cpp` — atomic tear test
Standalone, no flycast linkage. One thread hammers `_slotInputAtomic[0]` at
12 KHz with a known walking-bit pattern; another thread reads in a tight
loop and asserts every observed `(buttons, lt, rt)` triple is one of the
writer's known-good combinations. **Verified: 19,887,464,733 reads, 0 torn
over 60 seconds.**

```bash
g++ -std=c++17 -O2 -pthread tests/input_atomic_smoke.cpp -o /tmp/smoke
/tmp/smoke 60
```

### `tests/input_accumulator_test.cpp` — accumulator logic test
Single-threaded synthetic test of the accumulator's edge detection, drain
semantics, deferred releases, and three-presses-collapse-to-one. 34 assertions.

```bash
g++ -std=c++17 -O2 tests/input_accumulator_test.cpp -o /tmp/accum
/tmp/accum
# === 34 passed, 0 failed ===
```

### `tests/dash_repeatability_test.cpp` — comparative dashing-bug test
Runs 50 synthetic dash sequences (press-then-release within one vblank) under
both LatencyFirst and ConsistencyFirst, asserts ConsistencyFirst sees all 50
and LatencyFirst sees fewer than 50.

```bash
g++ -std=c++17 -O2 tests/dash_repeatability_test.cpp -o /tmp/dash
/tmp/dash
# LatencyFirst:    0 / 50 dashes seen by SH4
# ConsistencyFirst: 50 / 50 dashes seen by SH4
```

### Determinism rig — wire byte verification
The `MAPLECAST_DUMP_TA=1` rig dumps every TA buffer the server produces and
the client receives, then byte-cmps. **Both policies must be internally
deterministic** (same input schedule + same code = same wire bytes), and
**LatencyFirst must be byte-perfect identical to the historical pre-Phase-B
baseline** (because nothing observable changed for that path). The baseline
hashes are stored in `tests/determinism-baselines/phase-a1.sha256`.

See [docs/ARCHITECTURE.md](ARCHITECTURE.md) "The determinism test rig" for
the full recipe.

---

## 9. Per-User Model (shipped 2026-04-08, post-Phase-B)

The latch policy follows the **player**, not the slot. The policy a player
chooses lives in their browser localStorage and is sent to flycast as a
`latch_policy` field on every `join` handshake. The slot inherits the
player's preference when they (re)take it, regardless of which slot they
end up in or who was sitting there before.

### How the per-user model works

1. **User opens the diagnostics modal LATENCY tab** and sees ONLY their own
   slot's policy buttons (if they're a player). Spectators see read-only
   labels for both slots. The other player's row is hidden — only that
   player can change their own setting.

2. **User clicks LATENCY or CONSISTENCY** (or presses F1). Three things happen:
   - `_diagSettings.preferredLatchPolicy` is updated and saved to
     `localStorage.maplecast_diag_settings`
   - A `set_latch_policy` WS message is sent to flycast with their slot
   - Server verifies the requesting connection actually owns that slot
     (`getSlotForConn(hdl)`) and applies the change. Non-owners get
     `set_latch_policy_error: "you can only change your own slot's latch policy"`

3. **User leaves and rejoins** (page refresh, kicked, slot swap, queue
   promotion). The next `join` handshake includes their stored
   `latch_policy: "latency"|"consistency"` from localStorage. Server's `join`
   handler reads the field and calls `setLatchPolicy(slot, theirPref)` after
   the slot is assigned. **Their preference takes effect immediately on the
   new slot, no manual re-toggle.**

4. **The slot stays the implementation detail.** The per-slot
   `_latchPolicy[2]` atomic in `maplecast_input_server.cpp` is still the
   only place the latch path actually reads from. The per-user layer just
   pushes user preferences into that atomic at the right moments.

### Storage layers (in order of authority)

| Layer | Where | Persistence | Status |
|---|---|---|---|
| **Browser localStorage** | `maplecast_diag_settings.preferredLatchPolicy` | This browser only, survives reload | ✅ shipped |
| **NOBD stick binding** | `~/.maplecast/sticks.json` (eventual) | Survives flycast restart, hardware-bound | 🔜 follow-up |
| **SurrealDB user profile** | `player.latch_policy` (eventual) | Cross-device, survives anything | 🔜 Phase C |

### Why per-user instead of per-stick (or per-account)

**Per-user via the join handshake** is the smallest correct change that
delivers the right player-facing behavior today, with zero new server-side
storage. It composes cleanly with both follow-up layers:

- **Stick-memory** can layer in: when a registered NOBD stick takes a slot,
  flycast looks up the stick's stored preference (from the binding cache)
  and applies it via the same `setLatchPolicy()` call. The browser user's
  preference (from the join handshake) and the stick's preference reconcile
  by recency or by an explicit precedence rule (TBD).
- **SurrealDB sync** can layer in: on sign-in, the browser fetches the
  player's profile and seeds localStorage from it. On preference change,
  the collector mirrors localStorage → SurrealDB. Cross-device just works.

### Files involved

| File | Per-user role |
|---|---|
| [web/js/diagnostics.mjs](../web/js/diagnostics.mjs) | `getPreferredLatchPolicy()` exported, `sendLatchPolicy()` client gate, LATENCY tab UI hides non-owned slots |
| [web/js/queue.mjs](../web/js/queue.mjs) | Two `join` send sites include `latch_policy: getPreferredLatchPolicy()` |
| [web/js/ws-connection.mjs](../web/js/ws-connection.mjs) | Two more `join` send sites (your_turn promotion, open-slot autojoin) |
| [core/network/maplecast_ws_server.cpp](../core/network/maplecast_ws_server.cpp) | `set_latch_policy` server gate via `getSlotForConn(hdl)`, `join` handler applies `latch_policy` field |

---

## 10. Critical Files (don't grep, just open these)

| File | What it owns |
|---|---|
| [core/network/maplecast_input_server.h](../core/network/maplecast_input_server.h) | `LatchPolicy` enum, `AccumPacked` struct, `_slotInputAtomic[]`, `_slotAccum[]`, `LatchStats`, public API |
| [core/network/maplecast_input_server.cpp](../core/network/maplecast_input_server.cpp) | `updateSlot()` (atomic + CAS-loop), `consistencyFirstLatch()`, `drainAccumulator()`, env var parsing, telemetry ring buffer |
| [core/network/ggpo.cpp](../core/network/ggpo.cpp) | `getLocalInput()` — the latch read site, where the policy fork lives (line 35) |
| [core/hw/maple/maple_if.cpp](../core/hw/maple/maple_if.cpp) | `maple_DoDma()` — calls `ggpo::getInput()` once per vblank (line 153) |
| [core/network/maplecast_mirror.cpp](../core/network/maplecast_mirror.cpp) | `serverPublish()` — frame counter / latch time / frame period EMA |
| [core/network/maplecast_mirror.h](../core/network/maplecast_mirror.h) | `currentFrame()`, `lastLatchTimeUs()`, `framePeriodUs()` accessors |
| [core/network/maplecast_ws_server.cpp](../core/network/maplecast_ws_server.cpp) | `getStatus()` (status JSON), `set_latch_policy` WS handler |
| [web/js/gamepad.mjs](../web/js/gamepad.mjs) | `onServerFramePhase()`, phase-aligned setTimeout-based forced send |
| [web/js/diagnostics.mjs](../web/js/diagnostics.mjs) | The unified Settings/Diagnostics modal — Graphics/Stats/Latency/Input tabs |
| [web/js/ws-connection.mjs](../web/js/ws-connection.mjs) | `handleStatus()` — wires the `frame_phase`/`latch_policy`/`latch_stats` blocks into `state.diag` |

---

## 11. Things You Will Be Tempted to Do — Don't

- **Don't add a third buffer layer between `_slotInputAtomic` and `mapleInputState`.** The two existing buffers are correct. The race that the original "Frame-Aligned Input Latching" proposal was trying to solve is different from the actual dashing bug; that proposal would have built a redundant intermediate. The accumulator IS the third layer, but it lives next to the live word, not between the live word and the latched word.

- **Don't move the latch hook point.** It's at the top of `maple_DoDma()`, which fires from `maple_vblank()` at the `vblank_out_interrupt_line_number` scanline, exactly once per frame. The CMD9 handlers in the same DMA pass read from `mapleInputState[]` microseconds later. Anything that splits the latch from the CMD9 read introduces a race.

- **Don't make the guard window wider than ~1-2 ms.** The guard is supposed to deal with sub-millisecond network jitter at the boundary. A 5 ms guard would catch real player inputs that they intended to land in the current frame, defer them to the next, and feel like input lag. The default is 500 µs.

- **Don't enable `MAPLECAST_LATCH_POLICY=consistency` as the global default on deploy.** The current default is `latency` and that's correct — players who haven't opted in shouldn't get a behavior change on a deploy. The dual-policy gate exists so each player picks for themselves.

- **Don't reintroduce input prediction.** The original proposal had a "hold previous" prediction layer that fired when a packet arrived inside the guard window. We dropped it because for a 5 ms-late packet it would hold the previous state for 1 frame, then deliver the late packet on the frame after — which can manifest as a doubled input or AB→CD reordering. The accumulator's edge-preservation handles intra-frame transitions correctly without prediction; don't add it back.

- **Don't run the determinism rig per-step during a multi-step change in the latch path.** The rig is the FINAL gate, not a per-edit check. Run it once at the very end, after the entire change is shipped and verified live. If it fails, backtrack.

- **Don't conflate the GP2040-CE NOBD sync window with the MapleCast accumulator.** They solve the same problem at different layers. NOBD groups GPIO presses inside the firmware's 5 ms window before the packet ever leaves the stick. MapleCast's accumulator preserves whatever the firmware already grouped, AND catches grouping for input sources that don't have firmware-level grouping (browser gamepads). **Do NOT layer a second 5 ms timer on top of the firmware's** — that would be double-debounce.

- **Don't bypass the per-user gate on `set_latch_policy`.** The server-side check at the top of the handler in `maplecast_ws_server.cpp` calls `getSlotForConn(hdl)` and rejects mismatches. This is the load-bearing security check — not the UI hide. A spectator with a dev console can craft and send `set_latch_policy` for any slot; the server is the only thing that stops them from clobbering the actual player's preference. **If you ever add a new code path that calls `setLatchPolicy()` directly from a WS handler, gate it the same way.**

---

## 12. References

- GP2040-CE NOBD firmware sync window: [GP2040-CE/src/gp2040.cpp](file:///mnt/win1/Users/trist/projects/GP2040-CE/src/gp2040.cpp) `syncGpioGetAll()` — the prior art that inspired the accumulator design.
- GP2040-CE WHY-NOBD design rationale: [GP2040-CE/docs/WHY-NOBD.md](file:///mnt/win1/Users/trist/projects/GP2040-CE/docs/WHY-NOBD.md).
- Determinism rig: [docs/ARCHITECTURE.md](ARCHITECTURE.md) "Mirror Wire Format — Rules of the Road" + "The determinism test rig".
- VPS deployment: [docs/VPS-SETUP.md](VPS-SETUP.md).
- Original Phase B proposal + critique: `~/.claude/plans/tidy-wibbling-dongarra.md` (preserved for future reference).

---

*Document created 2026-04-08 alongside the Phase B deploy. Update this doc
whenever the latch path changes, the accumulator semantics change, or a new
policy is added. The future-you who has to debug a regression will thank
present-you for keeping it current.*
