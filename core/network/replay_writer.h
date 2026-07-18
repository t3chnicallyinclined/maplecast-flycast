/*
	Replay Writer — deterministic .mcrec recorder.

	A `.mcrec` file is the deterministic-replay artifact: a starting
	savestate plus a per-frame input log. Replay = load savestate,
	feed inputs frame-by-frame, emulator regenerates byte-perfect
	identical TA frames.

	File layout (little-endian throughout):

	┌──────────────────────────────────────────┐
	│ HEADER (variable size)                    │
	│   magic         "MCREC\0\0\0"             │  8 bytes
	│   version       u32 = 1                   │
	│   flycast_ver   u32 (build hash, future)  │
	│   match_id      16-byte UUID              │
	│   server_id     16-byte UUID              │
	│   start_unix_us u64                       │
	│   duration_us   u64 (filled at finalize)  │
	│   rom_hash      32 bytes (SHA-256)        │
	│   p1_name       64 bytes (null-padded)    │
	│   p2_name       64 bytes                  │
	│   p1_chars[3]   3 bytes                   │
	│   p2_chars[3]   3 bytes                   │
	│   winner        u8 (0=p1, 1=p2, 0xFF=na)  │
	│   reserved      40 bytes                  │
	├──────────────────────────────────────────┤
	│ START SAVESTATE                           │
	│   raw_size      u32                       │
	│   compressed_size u32                     │
	│   data          zstd(dc_serialize), N bytes │
	├──────────────────────────────────────────┤
	│ INPUT LOG (variable, one entry per frame)│
	│   each: TapeEntry from input_server.h    │
	│     frame:       u64                       │
	│     seqAndSlot:  u32                       │
	│     buttons:     u16                       │
	│     lt:          u8                        │
	│     rt:          u8                        │
	│     = 16 bytes                             │
	├──────────────────────────────────────────┤
	│ FOOTER                                    │
	│   "MCEND"       5 bytes                   │
	│   entry_count   u32                       │
	│   hmac_sha256   32 bytes (over header     │
	│                  + savestate + input log) │
	└──────────────────────────────────────────┘

	Storage math:
	  Savestate: ~600 KB after zstd
	  Input log: 16 bytes × 60 fps × 60 sec = 56 KB/min
	  5-min match total: ~880 KB. ~350× smaller than TA-stream replay.
*/
#pragma once

#include <cstdint>
#include <string>

namespace maplecast_replay
{

// Begin recording to outPath. Captures a starting savestate immediately,
// then accumulates input tape entries until stop() is called.
//
// Concurrency: thread-safe to call from any thread. Internal mutex
// serializes access. The actual file write is buffered + flushed on
// stop() to keep the per-frame fast-path lock-free.
//
// metadata fields are optional but populate the header. Returns false
// if file can't be opened or savestate fails.
struct StartParams {
	std::string out_path;
	std::string p1_name;       // optional
	std::string p2_name;       // optional
	uint8_t     p1_chars[3] = {0xFF, 0xFF, 0xFF};
	uint8_t     p2_chars[3] = {0xFF, 0xFF, 0xFF};
	std::string rom_hash_hex;  // optional, 64 hex chars
	std::string server_id;     // optional, hex UUID

	// When true, start() arms the recorder but does NOT capture the
	// savestate or open the file yet. The mirror server polls the MVC2
	// in_match flag every frame; on the first 0→1 transition while
	// armed, the writer fires the actual capture. Lets the operator
	// trigger record_start from a menu and have the .mcrec begin at
	// the literal first match frame, skipping menu/character-select.
	bool        arm_at_match = false;
};

bool start(const StartParams& p);

// Same as start(), but brackets the call with emu.stop()+emu.start() so
// the SH4 is halted at an instruction boundary while dc_savestate runs
// and resumed cleanly afterwards. Used by runtime triggers (control-WS,
// mirror-WS, F9 hotkey) where the SH4 is mid-execution. Don't call this
// from the autoload-time path -- the SH4 isn't running there yet, and
// emu.stop() on a non-running emu is undefined.
bool startInteractive(const StartParams& p);

// Called from mirror server's serverPublish each frame with the freshly-
// read in_match flag from guest RAM. If the writer was armed (start()
// with arm_at_match=true), watches for the 0→1 transition and fires
// the deferred capture. No-op if not armed or transition hasn't happened.
void onFrameInMatchFlag(uint8_t in_match);

// Whether the writer is armed but not yet capturing. Distinct from
// active() (which returns true once capture has fired and inputs are
// being logged).
bool armed();

// V5-era no-op stub. V2 discipline does all the work inline in start()
// at autoload, so there's nothing to defer. Kept for emulator.cpp's
// vblank() hook ABI; safe to drop once that hook is removed.
bool executePendingCapture();

// ── Hotkey-trigger plumbing ──────────────────────────────────────────
// Lets a runtime caller (control-WS endpoint, in-game hotkey) request
// recording from the user's CURRENT playing position rather than from
// the autoload state. The caller dc_savestate's the current state to
// REPLAY_SLOT, calls setNextRecordPath(path), then emu.stop()+start().
// emulator.cpp's autoload section consumes the path and dc_loadstate's
// from REPLAY_SLOT (instead of slot 0), so the SH4 resumes at the
// captured mid-game state. This preserves the V2 invariant that
// dc_loadstate runs at autoload before the SH4 thread spawns.

void setNextRecordPath(const std::string& path);
std::string consumeNextRecordPath();
bool hasNextRecordPath();

// ── Per-match continuous recording (MAPLECAST_RECORD_MATCHES) ─────────
// Captures the autoload savestate ONCE per server boot, accumulates an
// always-on input log starting at frame 0, and writes one .mcrec per
// match (split on in_match 0->1->0 transitions). Each match file
// embeds the SAME autoload savestate + the cumulative input log up to
// that match's end + a start_frame header field. Replay loads the
// savestate at autoload boundary, fast-forwards through inputs to
// start_frame, then plays from there. Byte-perfect because every
// dc_savestate happens once at the proven autoload moment.
//
// Init from emulator.cpp's autoload section after the SH4 has loaded
// state but before the emu thread spawns. Returns false if the
// recordings dir can't be created or the savestate capture fails.

bool initMatchRecording(const std::string& recordings_dir, int retention_days);
bool matchRecordingActive();

// Append one input event. Called from the input server's tape publisher
// loop (existing infrastructure in maplecast_input_server.cpp). No-op
// if not currently recording.
void append(uint64_t frame, uint32_t seqAndSlot, uint16_t buttons,
            uint8_t lt, uint8_t rt);

// Stop recording and finalize the file. Sets winner field, computes HMAC,
// writes footer. Safe to call multiple times.
void stop(uint8_t winner = 0xFF);

// True iff currently recording.
bool active();

// How many input events appended this session.
uint64_t entryCount();

// Periodic state checkpoint. Captures a fresh savestate via dc_savestate
// to a non-autoload slot (so we don't clobber the user's autoload baseline)
// and appends the bytes to the sidecar file <out_path>.ckpt. Lets the
// reader seek into long replays without playing through leading idle.
//
// Driven from the renderer thread (serverPublish) which holds the SH4
// thread paused at frame boundaries, so dc_serialize is safe at this
// point. No-op if recording isn't active or sidecar wasn't opened.
//
// Default cadence is once every 600 frames (~10s at 60Hz); the caller
// is responsible for the throttle (we don't gate internally so the
// caller can tune cadence per use case).
void checkpoint(uint64_t frame);

// ── Dataset state-stream (.mctele) sidecar ────────────────────────────
// Companion to the input log for the mvc2-ai dataset exporter: one
// full-RAM state blob per published frame, streamed to a session file
// <recordings_dir>/session-<stamp>.mctele. Gated by MAPLECAST_RECORD_STATE
// (read in initMatchRecording); OFF unless that env is set, so existing
// MAPLECAST_RECORD_MATCHES users are unaffected. The mirror's serverPublish
// builds the blob from guest RAM (this module stays memory-map-agnostic)
// and calls appendState at the same frame boundary as checkpoint().
struct StateSeg { uint32_t addr; uint32_t len; };  // guest addr + byte length

// True iff MAPLECAST_RECORD_STATE was set when initMatchRecording ran.
bool stateRecordingEnabled();

// ── Runtime dataset-recording toggle (admin, via the control WS) ──────
// When ON, serverPublish's tap writes the .mctele — per-frame game state PLUS
// both players' latched Input_DEC, so the dataset is fully self-contained (no
// .mcrec needed). OFF by default so idle/menu/normal play is never recorded;
// the operator flips it on for a specific training session and off after. The
// tap re-checks this every frame, so it takes effect instantly. `dir` is where
// the .mctele lands (defaults to the record-matches dir).
bool datasetRecordingActive();
void setDatasetRecording(bool on, const std::string& dir);

// Declare the per-frame blob layout and open the .mctele. Idempotent —
// first call opens the file and writes the header; later calls no-op.
// blobLen must equal the sum of the seg lengths.
void beginStateStream(const StateSeg* segs, uint32_t nsegs, uint32_t blobLen);

// Append one frame's state blob. `frame` is the publish frame
// (hdr->frame_count), the same clock the input log uses. No-op until
// beginStateStream has run.
void appendState(uint64_t frame, const uint8_t* blob, uint32_t len);

} // namespace maplecast_replay
