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
};

bool start(const StartParams& p);

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

} // namespace maplecast_replay
