# Replay — Simplification Plan

> Companion to [ROLLBACK-PREDICTION.md](ROLLBACK-PREDICTION.md). Written 2026-05-07
> after two days of crash-and-patch on `replay_reader` / `replay_writer`. The proven-systems
> research below makes the right shape obvious — we're carrying ~80% more architecture
> than we need, and the parts that *are* essential are wired in the wrong order.

---

## 1. Executive summary

We've been building a tournament-grade archive format (HMAC, hub upload, UUID match-ids,
zstd-compressed `dc_serialize` snapshots) before we have a working `(frame, buttons)`
loop. **Delete the archive scaffolding, keep the input log, and start from a known-good
on-disk savestate (`mvc2.state`) instead of round-tripping `dc_serialize` at runtime.**
The result will be a ~200-line reader/writer pair that mirrors how FBNeo's `.fr` recordings
have shipped to thousands of users for a decade — and it dissolves all three failure
modes we've been chasing (crash A, crash B, the 3,900-byte desync).

---

## 2. How Fightcade / GGPO / FBNeo actually do it

### 2.1 GGPO — the callback contract that defines the shape

From `ggponet.h` (`pond3r/ggpo`):

```c
typedef struct {
  bool (__cdecl *begin_game)      (const char *game);
  bool (__cdecl *save_game_state) (unsigned char **buffer, int *len, int *checksum, int frame);
  bool (__cdecl *load_game_state) (unsigned char *buffer, int len);
  bool (__cdecl *log_game_state)  (char *filename, unsigned char *buffer, int len);
  void (__cdecl *free_buffer)     (void *buffer);
  bool (__cdecl *advance_frame)   (int flags);
  bool (__cdecl *on_event)        (GGPOEvent *info);
} GGPOSessionCallbacks;
```

In the canonical `vectorwar` sample (`src/apps/vectorwar/vectorwar.cpp`):

```c
// save_game_state — NOT dc_serialize. NOT a file format. Just a flat memcpy.
*len      = sizeof(gs);
*buffer   = (unsigned char *)malloc(*len);
memcpy(*buffer, &gs, *len);
*checksum = fletcher32_checksum(...);

// load_game_state — also a flat memcpy.
memcpy(&gs, buffer, len);
```

Live frame loop:

```c
ggpo_add_local_input(...);
ggpo_synchronize_input(ggpo, inputs, sizeof(int)*MAX_SHIPS, &disconnect_flags);
VectorWar_AdvanceFrame(inputs, disconnect_flags);  // calls ggpo_advance_frame() at end
```

Two contracts that matter for us:

1. The "save state" GGPO traffics in is the **raw bytes the emulator needs to restart from
   this point** — not a file format with a version. Versioning belongs to whatever wraps
   GGPO (FBNeo's `.fr`, Fightcade's `.fs`), not to the rollback engine itself.
2. **The same code path runs live, rollback, and replay.** `ggpo_synchronize_input` returns
   live inputs in normal play, recorded inputs during rollback. There is no separate replay
   thread. Replay = `ggpo_start_spectating` against the recorded stream.

References:
- <https://github.com/pond3r/ggpo/blob/master/src/include/ggponet.h>
- <https://github.com/pond3r/ggpo/blob/master/src/apps/vectorwar/vectorwar.cpp>
- <https://github.com/pond3r/ggpo/blob/master/doc/DeveloperGuide.md>

### 2.2 FBNeo `.fr` — the format Fightcade actually ships

`src/burner/win32/replay.cpp`:

```
file = "FB1 " (4) | flags (4) | ... | [optional embedded savestate] | input buffer | ...
flags:
  MOVIE_FLAG_FROM_POWERON   — no savestate, just hard-reset and feed inputs
  MOVIE_FLAG_WITH_NVRAM     — embed NVRAM only (cartridge saves)
```

Crucial detail: **savestate is conditional.** When the recording starts from a power-on
boot, the file contains no savestate at all — replay runs from a hard reset, fed by the
input buffer alone. The full savestate is only embedded when the recording starts mid-game.

From `src/burner/state.cpp`:

```c
nFileVer            // version of the emulator that wrote the file
nMinimumMovieVersion = 0x0404  // anything lower will not load
```

Old replays simply fail to load. **No migration. No upcasting. Refuse and log.**

`src/burner/inputbuf.cpp`: per-frame input is huffman-compressed deltas (only changes
encoded). Buffer is byte-sequential, position-indexed, expands in 64KB chunks. `ReplayInput()`
runs once per frame from the same point in the loop that reads live input.

References:
- <https://github.com/finalburnneo/FBNeo/blob/master/src/burner/win32/replay.cpp>
- <https://github.com/finalburnneo/FBNeo/blob/master/src/burner/state.cpp>
- <https://github.com/finalburnneo/FBNeo/blob/master/src/burner/inputbuf.cpp>
- <https://deepwiki.com/finalburnneo/FBNeo/2.5-input-and-memory-management>

### 2.3 Patterns we should imitate

| Pattern | Source | What it gives us |
|---|---|---|
| Replay = live = rollback (one path) | GGPO + FBNeo | Kills the "playback thread vs SH4 thread" race forever |
| Pull-model `synchronize_input` per frame | GGPO | We already half-have this in `getInputAtFrame` — keep it, simplify |
| Power-on start, no embedded state | FBNeo `MOVIE_FLAG_FROM_POWERON` | Sidesteps the `dc_serialize` completeness gap entirely for V1 |
| Versioned format, refuse on mismatch | FBNeo `nMinimumMovieVersion` | One-line gate, no migration code |
| Flat-buffer save state (when needed) | GGPO `vectorwar` | If we ever DO need a state, it's `memcpy`, not a 600 KB compressed envelope |
| Strict EOF behavior — pause then exit | FBNeo `ReplayInput` | Explicit termination, no "WARNING: trimmed N trailing bytes" workaround |

---

## 3. What we're doing wrong

The current code is functional in places but the *shape* is misaligned with the proven
patterns. Each item below is a specific divergence with the file:line that contains it.

### 3.1 Wrong starting point — runtime `dc_serialize` round-trip

`replay_writer.cpp:181-212` calls `maplecast_mirror::buildFullSaveState(saveSize)`, which
goes through the full `dc_serialize` path. `replay_reader.cpp:262-263` matches with
`dc_deserialize`. **This is the source of the 3,900-byte desync** — `dc_serialize` does
not capture 100% of the live emulator state (renderer-side scratch, PVR sub-registers,
audio buffer pointers — pick any of them).

FBNeo's working answer: **don't round-trip at runtime when you don't have to.** Either:
- Start from power-on (`MOVIE_FLAG_FROM_POWERON`) — no savestate at all, or
- Reuse a savestate that was already on disk (autoload of `mvc2.state`) — same path as
  `dc_loadstate(SavestateSlot)` already takes. The disk file gets one round-trip lifetime,
  not one per match.

We have `mvc2.state` working today (autoload path proves it). We've never validated that
in-process `buildFullSaveState` round-trips cleanly, and the desync says it doesn't. Stop
using it for V1.

### 3.2 Crash A explanation — autoload point is correct, but for the wrong reason

`emulator.cpp:765-776` moved the replay restore to the autoload point and the SIGSEGV at
`0x5e6bb82b5f80` stopped. Good. But the JIT-cache theory in the doc is partially wrong.
The actual reason it works is simpler: **`dc_loadstate(config::SavestateSlot)` already
runs at this exact lifecycle moment** — line 746 in the same file. If that's the proven
hook for full state restore, anything we do that mirrors `dc_loadstate` must run from the
same place.

What the doc misses: at the autoload point, we call `dc_deserialize` *directly*. The
upstream `dc_loadstate` path goes through `dc_loadstate` which does a few extra steps
around the deserializer (sanity wrappers, RAM watch reset, `EventManager::event(Event::LoadState)`).
**Calling `dc_deserialize` directly from `emulator.cpp` is bypassing the same wrappers
`dc_loadstate` runs.** That alone could explain frame-1 drift even if the buffer round-trips
perfectly.

The fix is to either (a) skip the savestate entirely, FBNeo-style, or (b) write the
recorded savestate to a temp `.state` file and call `dc_loadstate` on it — same code path
as today's working autoload. Option (a) is simpler and our V1 target.

### 3.3 Crash B explanation — version skew with prod-recorded `.mcrec`

The `.mcrec` was recorded by the April-17 production binary. The local Windows binary is
built from `feat/option6-lookup-renderer` with serialization changes since then. `dc_deserialize`
reads bytes laid out by an older binary into a new struct shape and the read goes wild.

This is exactly what FBNeo's `nMinimumMovieVersion` exists to prevent — and we have **no
version check anywhere**. Our header reserves `flycast_ver u32` (writer.cpp:137 leaves it
zero, reader doesn't validate) and our `version u32` is hardcoded to 1. Both fields are
inert.

### 3.4 Reader has more "robustness" than the format deserves

`replay_reader.cpp:170-214` has a fallback for missing footers ("interrupted recording —
trim partial trailing bytes"). FBNeo refuses to load a movie that doesn't pass version
check — period. We've inverted the priority: we tolerate corrupted files and crash on
clean ones.

### 3.5 Writer's atexit handler is unreliable and we know it

`replay_writer.cpp:220-224` registers an `atexit` to call `stop()`. The doc itself says
"atexit isn't reliably firing on systemctl-stop signals." We know it's broken. We're
silently producing footerless files and then writing reader logic to handle them. This
is whack-a-mole.

### 3.6 Cloud upload + libcurl in the hot path

`replay_writer.cpp:28, 254-316` pulls libcurl into the hot path of the emulator. We don't
need this in the writer. If we want to upload, that's a post-hoc shell script (`scp`, `curl`)
or a separate tool. Putting it in the writer wires the recording path to a network library
and fattens the binary.

### 3.7 Pull-model input read — almost right, one wart

`ggpo.cpp:71` defines a static `_replayPaceFrame[2]` counter that's incremented per slot
per call. It's *almost* the right model (matches `ggpo_synchronize_input` semantics) but:
- Each slot's pace counter advances independently. This works only because both slots are
  read once per SH4 frame. If anything ever calls `getLocalInput` for one slot without the
  other (e.g., a 1P-only mode), they drift.
- The "advance the pace counter inside `getLocalInput`" pattern means the counter and the
  actual emulator frame number are decoupled. We should be keying off SH4's frame number
  directly, the way `ggpo_synchronize_input` does — there is one authoritative frame
  counter, and replay reads from index `frame`.

### 3.8 Sticky-input semantic in reader is silently wrong

`replay_reader.cpp:316-323` only updates the last-seen state when an entry's slot matches.
But our input log contains entries for *both* slots interleaved (one entry per packet, not
one entry per slot per frame). The current logic is correct under the "one entry per slot
per frame" assumption that doesn't actually hold — when player 2 holds a button without
re-sending, the writer doesn't emit a new entry, and the reader holds the last value.
That's fine for steady state but breaks at the very first frames where neither slot has
been seen yet — `getInputAtFrame` returns `false` (line 333), the SH4 falls through to the
plain-globals path (`ggpo.cpp:152-159`), and grabs whatever the SDL input layer happened to
have when replay started. That's a non-deterministic frame-zero source.

### 3.9 Header is 271 bytes for fields no V1 user will set

`replay_reader.h:23-35` and `replay_writer.h:14-46`: `match_id`, `server_id`, `rom_hash`,
`p1_chars[3]`, `p2_chars[3]`, `winner`, `reserved[40]` — none of these are required to
play the file back. They're metadata for a replay-archive UX we haven't built. They should
either move to a sidecar `.json` or just be deferred.

---

## 4. The minimum rewrite

Ordered by dependency. After each step the build still passes and replay either works
better or fails with a clearer error. Stop after step 6 — the rest is optional.

### Step 1. Drop in-process `dc_serialize` and use the existing on-disk savestate

**File:** `core/network/replay_writer.cpp` — `start()` around line 181-212.
**Change:** Replace `maplecast_mirror::buildFullSaveState(saveSize)` + zstd-compress block
with a single byte: `flags = MCREC_FLAG_FROM_AUTOLOAD`. **Do not embed any savestate.**

**File:** `core/network/replay_reader.cpp` — `loadStartSavestate()` around line 237-271.
**Change:** Delete the whole function (or stub it to return true and log "no savestate
embedded — relying on autoload"). Remove the call site in `emulator.cpp:768`.

**File:** `core/emulator.cpp` — autoload point around line 743-746.
**Change:** When `MAPLECAST_REPLAY_IN` is set, ensure `AutoLoadState` is forced on (env
override, same shape as `MAPLECAST_HEADLESS_AUTOLOAD`). The flycast `dc_loadstate(config::SavestateSlot)`
call already sitting at line 746 then becomes the canonical state-restore for replay.
**This is the single largest correctness win in the whole rewrite** — we use the same
code path that's been working for the autoload feature for years.

Why it works: both record and replay rely on the same `mvc2.state` file on disk. No
runtime serialization round-trip, so no completeness gap, so no 3,900-byte drift. Crash B
also goes away because we're no longer reading old-binary bytes into new-binary structs.

### Step 2. Add format version check that refuses incompatible files

**File:** `core/network/replay_writer.h` and `replay_reader.h`.
**Change:** Define `MCREC_VERSION_CURRENT = 2` and `MCREC_VERSION_MIN = 2`. Bump because
the V1 files are now incompatible (no embedded savestate).

**File:** `core/network/replay_reader.cpp` — `openReplay()` around line 126-131.
**Change:** Reject any file with `version < MCREC_VERSION_MIN` with an explicit error
message. Match FBNeo's `nMinimumMovieVersion` pattern: "this build can't load V1 .mcrec
files; re-record with the current binary."

**File:** Same.
**Change:** Add a `flycast_build_id u32` (or 16-byte hash) field that the writer fills
from a `MCREC_BUILD_ID` macro defined at compile time (we can pipe in `git rev-parse
--short HEAD` from CMake — there's existing infrastructure for build-stamp macros). On
read, refuse any file whose `flycast_build_id` differs from the running binary unless
`MAPLECAST_REPLAY_FORCE=1` is set. Same shape as the FBNeo "anything lower will not work
w/this version" rule.

### Step 3. Trim the header to V1-essential fields

**File:** `core/network/replay_writer.h` and `replay_reader.h`.
**Change:** Reduce the header to:

```
magic              "MCREC\0\0\0"   8
version            u32 = 2          4
flycast_build_id   u32 (or 16B)     4-16
flags              u32              4   (FROM_AUTOLOAD bit, FROM_POWERON future)
start_unix_us      u64              8
duration_us        u64 (patched)    8
entry_count        u64 (patched)    8
                                   ----
                                   ~44-56 bytes total
```

Drop: `match_id`, `server_id`, `rom_hash`, `p1_name/p2_name`, `p1_chars/p2_chars`,
`winner`, `reserved`. If we want any of those later, they live in a sidecar `<replay>.json`
written next to the `.mcrec` — same approach Quake demos / TAS movies / dolphin DTM all
take when archive metadata isn't fundamental to playback.

### Step 4. Make stop() reliable — fix the footer-on-SIGTERM bug

**File:** `core/network/replay_writer.cpp` — `start()` around line 220-224.
**Change:** Replace the `atexit(stop)` registration with:
1. Per-frame `fflush(_file)` in `append()` (cheap — already in the buffered hot path,
   and we only flush after `FLUSH_AFTER_BYTES` chunks anyway).
2. A SIGTERM/SIGINT signal handler installed at `start()` that calls `stop()` and chains
   to the previous handler. Same pattern flycast uses for graceful shutdown elsewhere
   (search for `signal(SIGTERM` in the tree).
3. Write the footer as the last thing in `append()` of the *previous* chunk, not just at
   `stop()`. Specifically: maintain a "tentative footer" at the current EOF after every
   flush, then truncate-and-rewrite-real-footer in `stop()`. A reader that opens a
   killed-mid-recording file always sees a valid footer.

The reader-side "no footer (interrupted recording) WARNING" + `inputBytes -= inputBytes %
16` workaround at `replay_reader.cpp:188-214` then becomes dead code. **Delete it.**

### Step 5. Unify the pull-model input read against the SH4 frame counter

**File:** `core/network/ggpo.cpp` around line 49-100.
**Change:** Replace the per-slot static `_replayPaceFrame[2]` counter with a call into
something that returns the SH4's authoritative frame number. The cleanest source we have
is whatever the existing `dc_*` frame counter is (find via `Grep` for `frame_counter` in
core/hw/sh4 or core/emulator.cpp — the comment at line 70-71 admits it's not currently
reading SH4's frame counter). One key for both slots, derived from the SH4, not a synthetic
per-slot counter.

**File:** `core/network/replay_reader.cpp` — `getInputAtFrame()` line 293-334.
**Change:** Document the sticky-input contract explicitly and handle the
"never-seen-an-entry-for-this-slot-yet" case by returning the neutral default (`0xFFFF` /
0 / 0) and `true`, not `false`. A `false` return triggers `ggpo.cpp:152` to fall through
to live globals — non-deterministic. **Replay should never fall through.** If replay is
active and the lookup hasn't seen an entry yet, neutral is the only deterministic answer.

### Step 6. Delete the cloud upload path

**File:** `core/network/replay_writer.cpp` lines 28, 254-316, 356-363.
**Change:** Remove `#include <curl/curl.h>`, `uploadToHub()`, and the `MAPLECAST_REPLAY_UPLOAD_URL`
trigger. Remove curl from `replay_writer`'s link line in CMakeLists. If users want to
upload, they run `curl -X POST --data-binary @match.mcrec https://nobd.net/...` from a
shell script after the file finalizes.

This is purely deletion — no design — and removes ~100 lines plus a runtime dependency.

### Optional steps (do AFTER 1-6 prove out)

#### Step 7. Add a `MCREC_FLAG_FROM_POWERON` mode

**Files:** `replay_writer.cpp` `start()`, `replay_reader.cpp` `openReplay()`,
`emulator.cpp` autoload point.
**Change:** When the writer's `flags` indicates `FROM_POWERON`, the reader does NOT trigger
`dc_loadstate` — it lets flycast hard-boot from BIOS, and the input log fed from frame 0
drives everything. This is FBNeo's default mode and it's the **most reproducible** of all
because there's no savestate file to go stale. Cost: you record the entire boot + attract
mode + character select. Benefit: zero state-format dependency.

For MVC2 specifically, the boot-to-character-select path is ~600-1000 frames (~10-17s).
For testing rollback determinism this is fine; for archived match replays it's cumbersome.
Worth having both modes once the basic path works.

#### Step 8. Move metadata to a sidecar JSON

**File:** new `core/network/replay_metadata.cpp` (small).
**Change:** When recording starts and `MAPLECAST_REPLAY_OUT=path.mcrec` is set, also write
`path.mcrec.json` containing `{p1_name, p2_name, p1_chars, p2_chars, server_id, match_id,
rom_hash, hub_match_url}`. The `.mcrec` itself stays minimal. Hub upload (if we ever
re-add it) uploads the pair.

This keeps the replay format playable forever — the metadata fields can grow without
breaking the binary parser.

---

## 5. Open questions

These are things the web research couldn't resolve cleanly — they need a code experiment
or a closer read of flycast internals.

1. **Does flycast's existing `dc_loadstate(SavestateSlot)` autoload run cleanly with the
   replay input log feeding inputs from frame 0?** The autoload happens at a known-good
   lifecycle moment (proven by `MAPLECAST_HEADLESS_AUTOLOAD`), but the SH4 frame counter
   right after autoload is *not* zero — it picks up wherever the savestate left it. Our
   input log's frame numbers are absolute (recorded against `currentFrame()` at record
   time). If record and replay both autoload the same `mvc2.state`, do their frame
   counters start at the same value? **Test:** record 10 frames, replay 10 frames, log
   the SH4 frame counter at the moment replay starts and confirm it matches the first
   recorded entry's frame. If yes — Step 1 works as-is. If no — input log needs to be
   stored as deltas-from-record-start and replay needs to subtract a base offset.

2. **Where exactly in the SH4 frame loop should we hook `getInputAtFrame`?** Today it's
   in `ggpo::getLocalInput` which is called from the maple-bus DMA handler. That's
   probably fine but it's a deeper hook than FBNeo's "input read at top of frame." If
   anything in flycast reads input from somewhere other than `ggpo::getLocalInput` (e.g.,
   DMA polling for a coin slot), replay will get inconsistent data. **Test:** grep for
   all callers of `kcode[]` / `lt[]` / `rt[]` and make sure the replay hook covers them
   all. Likely candidates: `core/hw/maple/maple_devs.cpp` and `core/hw/aica/`.

3. **Is `dc_serialize` round-trip actually broken, or is the call site wrong?** We
   asserted in `ROLLBACK-PREDICTION.md` that frame-1 byte-differs by 3,900 bytes after a
   `serialize → deserialize → run-1-frame` cycle. We did NOT compare against a clean
   `dc_loadstate` of `mvc2.state` — only against a runtime-captured snapshot. If
   `dc_loadstate` of an on-disk state IS deterministic but in-process serialize/deserialize
   isn't, the bug is in the buffer plumbing or in some side effect of `buildFullSaveState`
   that doesn't happen in `dc_loadstate`. **Test:** record using `dc_loadstate(mvc2.state)`
   instead of `buildFullSaveState`. If frame-1 matches replay, the bug is in
   `buildFullSaveState`. If it still doesn't, the bug is in deserialize itself.
   Step 1 of the rewrite makes this question moot for V1 (we don't round-trip at all),
   but we'll need to answer it before Phase 1 rollback ships, because rollback DOES
   round-trip every frame.

4. **Power-on replay: how stable is MVC2's boot sequence across runs?** FBNeo's
   `MOVIE_FLAG_FROM_POWERON` only works because their cores boot deterministically from
   the same ROM. We've validated SH4 determinism for in-game state, but never specifically
   for boot — there might be RNG seeded from time, BIOS-clock-dependent paths, or
   uninitialized RAM that drifts run-to-run. **Test:** boot MVC2 fresh twice, hash the
   first 600 frames of TA output, compare. If identical → power-on replay is viable
   (Step 7). If different → we're stuck with the savestate path and need to fix Q3.

5. **What's the right frame counter to key replay off?** `maplecast_mirror::currentFrame()`
   only advances in mirror-mode (it's tied to the publish loop). The doc comment in
   `ggpo.cpp:67-71` admits this. The SH4's actual frame counter lives elsewhere — likely
   PVR vsync count or a `dc_*` counter. **Test:** find it by grep; pick the one that
   advances exactly once per `dc_run` iteration.

6. **Should `dc_loadstate` be called BEFORE or AFTER `replay_writer::start` captures
   metadata?** Currently `emulator.cpp:765-792` does replay-restore before record-start
   in the same block, which is fine for the env-var-driven flow. But if we ever unify
   into a single "match start" event, the order matters: metadata wants the post-load
   state, but the savestate (if any) wants to be the pre-load state. We can punt on this
   until matchmaking integration in Phase 2.

---

## TL;DR for the next session

Start with Step 1: delete `buildFullSaveState`/`dc_deserialize` from the replay path, set
`AutoLoadState` when `MAPLECAST_REPLAY_IN` is set, and let the existing autoload code do
the state-restore work. That single change is expected to fix Crash B and the 3,900-byte
desync simultaneously, because both bugs live in the runtime `dc_serialize` round-trip we
shouldn't be doing.

Then Step 2 (version gate) so we never silently load an incompatible file again, Steps
3-4 (trim format, fix footer) for hygiene, Step 5 (unified frame counter) for replay
correctness, Step 6 (delete curl) for line count.

Steps 7-8 are nice-to-have. The first six get us a working, deterministic replay.
