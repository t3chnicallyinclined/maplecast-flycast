/*
	Replay Reader — .mcrec playback.

	Loads a recorded match: parses header, decompresses + applies the
	starting savestate, then yields input events frame-by-frame for
	the emulator to consume. The deterministic SH4 emulation regenerates
	byte-perfect identical TA frames — same pixels as the original match.

	Hooked from the input server: when MAPLECAST_REPLAY_IN is set at
	startup, we load the .mcrec, restore the savestate, and inject input
	events at their recorded frame numbers into the existing input pipe
	(updateSlot path). Game runs as if the players were live.
*/
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace maplecast_replay
{

struct ReplayInfo {
	std::string match_id_hex;
	uint64_t    start_unix_us;
	uint64_t    duration_us;
	std::string p1_name;
	std::string p2_name;
	uint8_t     p1_chars[3];
	uint8_t     p2_chars[3];
	uint8_t     winner;          // 0=p1, 1=p2, 0xFF=unknown
	uint64_t    entry_count;
	uint32_t    savestate_raw_size;
	uint32_t    savestate_compressed_size;
};

// Open the file, parse header. Does NOT load savestate or inputs yet.
// Returns false if file invalid (missing magic, version mismatch, etc.)
bool openReplay(const std::string& path);

// Get metadata about the currently-open replay (after openReplay()).
const ReplayInfo& info();

// Load the starting savestate via dc_deserialize. Must be called after
// openReplay() and BEFORE the emulator is running. Returns false on error.
bool loadStartSavestate();

// Spawn a background thread that injects recorded inputs into the
// running emulator at their recorded frame numbers. Frames are matched
// against maplecast_mirror::currentFrame(). Stops when input log is
// exhausted. Returns immediately.
//
// speed: 1.0 = real-time, 0.5 = half speed, 2.0 = double speed.
//        (Implemented by scaling the frame-stamp comparison.)
void startPlayback(double speed = 1.0);

// True iff playback is active.
bool playbackActive();

// True iff a replay is open (regardless of playback state).
bool isOpen();

// Stop playback + close the file.
void close();

} // namespace maplecast_replay
