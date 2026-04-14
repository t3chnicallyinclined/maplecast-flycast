/*
	Replay Reader — implementation.

	Reads the .mcrec format documented in replay_writer.h. Loading is
	a two-step dance:
	  1. openReplay() — parse header, populate ReplayInfo
	  2. loadStartSavestate() — decompress + dc_deserialize into RAM
	Then startPlayback() spawns a thread that walks the input log and
	calls updateSlot() at the right frame, giving us deterministic
	regeneration of the original match.
*/
#include "replay_reader.h"
#include "maplecast_input_server.h"
#include "maplecast_mirror.h"
#include "serialize.h"
#include "types.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <zstd.h>

namespace maplecast_replay
{

// ── State ─────────────────────────────────────────────────────────────

static std::mutex            _mtx;
static std::atomic<bool>     _open{false};
static std::atomic<bool>     _playing{false};
static FILE*                 _file = nullptr;
static ReplayInfo            _info{};
static std::thread           _playbackThread;

// Loaded into memory from the file at openReplay(). The savestate buffer
// is held until loadStartSavestate() consumes it; the input log is held
// until startPlayback() drains it.
static std::vector<uint8_t>  _savestateCompressed;
static std::vector<uint8_t>  _inputLog;       // raw 16-byte entries

// ── Helpers ───────────────────────────────────────────────────────────

static inline uint32_t readLE32(const uint8_t* p) {
	return (uint32_t)p[0]
	     | ((uint32_t)p[1] << 8)
	     | ((uint32_t)p[2] << 16)
	     | ((uint32_t)p[3] << 24);
}
static inline uint64_t readLE64(const uint8_t* p) {
	uint64_t v = 0;
	for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (i * 8);
	return v;
}

static std::string toHex(const uint8_t* data, size_t len) {
	static const char* hex = "0123456789abcdef";
	std::string s;
	s.reserve(len * 2);
	for (size_t i = 0; i < len; i++) {
		s.push_back(hex[data[i] >> 4]);
		s.push_back(hex[data[i] & 0xF]);
	}
	return s;
}

// ── openReplay() ──────────────────────────────────────────────────────

bool openReplay(const std::string& path) {
	std::lock_guard<std::mutex> lk(_mtx);
	if (_open.load()) {
		printf("[replay-reader] already open — close() first\n");
		return false;
	}

	_file = fopen(path.c_str(), "rb");
	if (!_file) {
		printf("[replay-reader] can't open %s\n", path.c_str());
		return false;
	}

	// Parse header (fixed 271 bytes for version 1).
	// Layout (offset → size):
	//   0   magic 8       8   version 4       12 flycast_ver 4
	//   16  match_id 16   32  server_id 16   48 start_unix_us 8
	//   56  duration_us 8 64  rom_hash 32    96 p1_name 64
	//   160 p2_name 64    224 p1_chars 3     227 p2_chars 3
	//   230 winner 1      231 reserved 40   = 271 bytes total
	uint8_t hdr[271];
	if (fread(hdr, 1, sizeof(hdr), _file) != sizeof(hdr)) {
		printf("[replay-reader] header truncated\n");
		fclose(_file); _file = nullptr;
		return false;
	}

	// Magic check
	if (memcmp(hdr, "MCREC\0\0\0", 8) != 0) {
		printf("[replay-reader] bad magic — not a .mcrec file\n");
		fclose(_file); _file = nullptr;
		return false;
	}

	// Version check
	uint32_t version = readLE32(hdr + 8);
	if (version != 1) {
		printf("[replay-reader] unsupported version %u (expected 1)\n", version);
		fclose(_file); _file = nullptr;
		return false;
	}

	// Populate ReplayInfo at the corrected offsets
	_info.match_id_hex  = toHex(hdr + 16, 16);
	_info.start_unix_us = readLE64(hdr + 48);
	_info.duration_us   = readLE64(hdr + 56);
	_info.p1_name       = std::string((const char*)hdr + 96,
	                                  strnlen((const char*)hdr + 96, 64));
	_info.p2_name       = std::string((const char*)hdr + 160,
	                                  strnlen((const char*)hdr + 160, 64));
	memcpy(_info.p1_chars, hdr + 224, 3);
	memcpy(_info.p2_chars, hdr + 227, 3);
	_info.winner        = hdr[230];

	// ── Read savestate block ──
	uint8_t saveHdr[8];
	if (fread(saveHdr, 1, 8, _file) != 8) {
		printf("[replay-reader] savestate header truncated\n");
		fclose(_file); _file = nullptr;
		return false;
	}
	_info.savestate_raw_size        = readLE32(saveHdr);
	_info.savestate_compressed_size = readLE32(saveHdr + 4);

	if (_info.savestate_compressed_size > 0) {
		_savestateCompressed.resize(_info.savestate_compressed_size);
		if (fread(_savestateCompressed.data(), 1, _info.savestate_compressed_size, _file)
		    != _info.savestate_compressed_size) {
			printf("[replay-reader] savestate data truncated\n");
			fclose(_file); _file = nullptr;
			return false;
		}
	}

	// ── Read input log ──
	// If the file ends with a "MCEND" footer, trim it and use the
	// entry_count it reports. Otherwise the recording was interrupted
	// (SIGTERM before stop()) — read everything from here to EOF,
	// rounding down to the nearest 16-byte entry boundary.
	long here = ftell(_file);
	fseek(_file, 0, SEEK_END);
	long fileSize = ftell(_file);
	long bytesAfterSavestate = fileSize - here;

	long inputBytes = bytesAfterSavestate;
	bool hasFooter = false;
	if (bytesAfterSavestate >= 41) {
		uint8_t footer[41];
		fseek(_file, fileSize - 41, SEEK_SET);
		if (fread(footer, 1, 41, _file) == 41 && memcmp(footer, "MCEND", 5) == 0) {
			hasFooter = true;
			inputBytes = bytesAfterSavestate - 41;
			_info.entry_count = readLE32(footer + 5);
		}
	}

	// Round inputBytes down to a multiple of 16 (handles partial last
	// entry from an interrupted recording)
	long origInput = inputBytes;
	inputBytes -= inputBytes % 16;
	if (origInput != inputBytes) {
		printf("[replay-reader] WARNING: trimmed %ld trailing bytes (partial entry)\n",
		       origInput - inputBytes);
	}

	if (inputBytes < 0) {
		printf("[replay-reader] negative input log size %ld — corrupted\n", inputBytes);
		fclose(_file); _file = nullptr;
		return false;
	}

	fseek(_file, here, SEEK_SET);
	_inputLog.resize(inputBytes);
	if (inputBytes > 0 && fread(_inputLog.data(), 1, inputBytes, _file)
	    != (size_t)inputBytes) {
		printf("[replay-reader] input log read failed\n");
		fclose(_file); _file = nullptr;
		return false;
	}

	if (!hasFooter) {
		printf("[replay-reader] WARNING: no footer (interrupted recording)\n");
		_info.entry_count = inputBytes / 16;
	}

	fclose(_file);
	_file = nullptr;
	_open.store(true);

	printf("[replay-reader] opened %s\n", path.c_str());
	printf("[replay-reader]   match: %s  duration: %.2fs\n",
	       _info.match_id_hex.c_str(), _info.duration_us / 1000000.0);
	printf("[replay-reader]   players: '%s' vs '%s'\n",
	       _info.p1_name.c_str(), _info.p2_name.c_str());
	printf("[replay-reader]   savestate: %u bytes raw / %u compressed\n",
	       _info.savestate_raw_size, _info.savestate_compressed_size);
	printf("[replay-reader]   inputs: %llu events (%ld bytes)\n",
	       (unsigned long long)_info.entry_count, inputBytes);
	return true;
}

const ReplayInfo& info() { return _info; }
bool isOpen() { return _open.load(std::memory_order_relaxed); }

// ── loadStartSavestate() ──────────────────────────────────────────────

bool loadStartSavestate() {
	std::lock_guard<std::mutex> lk(_mtx);
	if (!_open.load()) return false;
	if (_savestateCompressed.empty() || _info.savestate_raw_size == 0) {
		printf("[replay-reader] no savestate in this replay (skipping)\n");
		return true;  // not fatal — caller can fall back to a fresh boot
	}

	// Decompress
	std::vector<uint8_t> raw(_info.savestate_raw_size);
	size_t actual = ZSTD_decompress(raw.data(), raw.size(),
	                                _savestateCompressed.data(),
	                                _savestateCompressed.size());
	if (ZSTD_isError(actual)) {
		printf("[replay-reader] zstd decompress failed: %s\n",
		       ZSTD_getErrorName(actual));
		return false;
	}
	if (actual != _info.savestate_raw_size) {
		printf("[replay-reader] decompressed size mismatch: %zu vs %u\n",
		       actual, _info.savestate_raw_size);
		return false;
	}

	// Apply via dc_deserialize. The state object reads from our buffer.
	Deserializer ctx(raw.data(), raw.size());
	dc_deserialize(ctx);

	// Free the compressed buffer — we don't need it anymore
	_savestateCompressed.clear();
	_savestateCompressed.shrink_to_fit();

	printf("[replay-reader] savestate restored (%zu bytes)\n", actual);
	return true;
}

// ── playback thread ──────────────────────────────────────────────────

static uint64_t nowUs() {
	auto now = std::chrono::steady_clock::now();
	return std::chrono::duration_cast<std::chrono::microseconds>(
		now.time_since_epoch()).count();
}

static void playbackLoop(double speed) {
	// Walk the input log entry by entry, blocking until each entry's
	// recorded frame matches the emulator's current frame, then call
	// updateSlot() to inject the input.
	const size_t N = _inputLog.size() / 16;
	size_t i = 0;

	auto firstEntry = nowUs();  // not used until we know the first frame
	uint64_t firstFrame = N > 0 ? readLE64(&_inputLog[0]) : 0;

	while (_playing.load(std::memory_order_relaxed) && i < N) {
		const uint8_t* e = &_inputLog[i * 16];
		uint64_t frame      = readLE64(e);
		uint32_t seqAndSlot = readLE32(e + 8);
		uint16_t buttons    = (uint16_t)e[12] | ((uint16_t)e[13] << 8);
		uint8_t  ltVal      = e[14];
		uint8_t  rtVal      = e[15];
		int slot = (int)((seqAndSlot >> 24) & 0x3);

		// Wait until the emulator catches up to this frame number
		// (scaled by speed — 0.5 = wait twice as long, 2.0 = half).
		while (_playing.load(std::memory_order_relaxed)) {
			uint64_t curFrame = maplecast_mirror::currentFrame();
			// Scale frame target by speed (faster speed = "consume" more
			// recorded frames per real frame)
			uint64_t targetFrame = firstFrame +
				(uint64_t)((frame - firstFrame) / speed);
			if (curFrame >= targetFrame) break;
			std::this_thread::sleep_for(std::chrono::microseconds(500));
		}

		// Inject the input event via the public injectInput() — same
		// path live gameplay uses, guarantees identical observability
		// for the SH4 (atomic update of _slotInputAtomic + accumulator).
		maplecast_input::injectInput(slot, ltVal, rtVal, buttons);
		i++;
	}

	_playing.store(false);
	printf("[replay-reader] playback complete (%zu events injected)\n", i);
}

void startPlayback(double speed) {
	std::lock_guard<std::mutex> lk(_mtx);
	if (!_open.load() || _playing.load()) return;
	_playing.store(true);
	_playbackThread = std::thread(playbackLoop, speed);
	printf("[replay-reader] playback started @ %.2fx speed\n", speed);
}

bool playbackActive() { return _playing.load(std::memory_order_relaxed); }

void close() {
	{
		std::lock_guard<std::mutex> lk(_mtx);
		_playing.store(false);
	}
	if (_playbackThread.joinable()) _playbackThread.join();
	std::lock_guard<std::mutex> lk(_mtx);
	_savestateCompressed.clear();
	_inputLog.clear();
	_open.store(false);
}

} // namespace maplecast_replay
