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

// Activate replay-mode input read. After this call, getInputAtFrame()
// returns recorded inputs and the SH4's input read path (ggpo::
// getLocalInput) routes through it.
//
// 'speed' is preserved for API compatibility. 1.0 = real-time playback
// at the emulator's natural frame rate. Other values are ignored under
// the pull model — the SH4's frame loop drives playback rate.
//
// Pull-model (2026-05-07 redesign): replaces the previous push-model
// playback thread that injected inputs into the live input atomic.
// That model raced the SH4 input read path and crashed at SIGSEGV
// 0x5e6bb82b5f80. Inspired by Fightcade/GGPO's approach where the
// emulator's frame loop pulls recorded inputs at each frame instead
// of a separate thread pushing.
void startPlayback(double speed = 1.0);

// True iff replay-mode input read is active.
bool playbackActive();

// Pull-model input read. Called from ggpo::getLocalInput() once per
// frame, per slot. Returns true and writes outputs if a recorded entry
// exists for (frame, slot). Returns false otherwise — caller should
// hold the last-known input or fall through to defaults.
//
// Implementation: linear scan with cached per-slot cursor; O(1)
// amortized for monotonically-advancing frame numbers.
bool getInputAtFrame(uint64_t frame, int slot,
                     uint16_t& outButtons, uint8_t& outLt, uint8_t& outRt);

// True iff a replay is open (regardless of playback state).
bool isOpen();

// Stop playback + close the file.
void close();

} // namespace maplecast_replay
