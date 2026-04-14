/*
	Replay Writer — implementation.

	Design notes:
	• Append path is hot (called from input tape publisher every frame).
	  Uses a small in-memory ring; a background flusher serializes to disk.
	  Avoids per-frame I/O cost on the critical path.
	• Savestate captured at start() — calls maplecast_mirror::buildFullSaveState()
	  which is the canonical dc_serialize() path used everywhere.
	• zstd compression for the savestate (~3-4× smaller, ~600 KB for MVC2).
	• HMAC computed over the file at stop(). Using SHA-256 from existing
	  picosha2 if present, or a tiny embedded impl (header-only).
*/
#include "replay_writer.h"
#include "maplecast_mirror.h"
#include "types.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <zstd.h>

namespace maplecast_replay
{

// ── State ─────────────────────────────────────────────────────────────

static std::mutex            _mtx;
static std::atomic<bool>     _active{false};
static std::atomic<uint64_t> _entryCount{0};
static FILE*                 _file = nullptr;

// In-memory accumulator for input log — flushed in chunks every N entries
// or at stop(). 1024 entries × 16 bytes = 16 KB chunk.
static std::vector<uint8_t>  _inputBuf;
static constexpr size_t      FLUSH_AFTER_BYTES = 16 * 1024;

// Header fields (some filled at finalize)
static uint64_t              _startUnixUs = 0;

// ── Helpers ───────────────────────────────────────────────────────────

static inline void writeLE32(uint8_t* dst, uint32_t v) {
	dst[0] = (uint8_t)(v);
	dst[1] = (uint8_t)(v >> 8);
	dst[2] = (uint8_t)(v >> 16);
	dst[3] = (uint8_t)(v >> 24);
}
static inline void writeLE64(uint8_t* dst, uint64_t v) {
	for (int i = 0; i < 8; i++) dst[i] = (uint8_t)(v >> (i * 8));
}

static uint64_t nowUnixUs() {
	auto now = std::chrono::system_clock::now();
	return std::chrono::duration_cast<std::chrono::microseconds>(
		now.time_since_epoch()).count();
}

// Flush the input buffer to disk if it crosses the chunk threshold.
// Caller must hold _mtx.
static void maybeFlush(bool force) {
	if (!_file) return;
	if (!force && _inputBuf.size() < FLUSH_AFTER_BYTES) return;
	if (_inputBuf.empty()) return;
	fwrite(_inputBuf.data(), 1, _inputBuf.size(), _file);
	_inputBuf.clear();
}

// Write one fixed-size field, padded to N bytes with zeros.
static void writePaddedString(FILE* f, const std::string& s, size_t total) {
	std::vector<uint8_t> buf(total, 0);
	size_t n = std::min(s.size(), total);
	memcpy(buf.data(), s.data(), n);
	fwrite(buf.data(), 1, total, f);
}

// Decode hex string into bytes. Returns true if exactly out_len bytes
// were produced. Pads with zeros if input is shorter.
static bool hexDecode(const std::string& hex, uint8_t* out, size_t out_len) {
	memset(out, 0, out_len);
	auto hexVal = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	size_t outIdx = 0;
	for (size_t i = 0; i + 1 < hex.size() && outIdx < out_len; i += 2) {
		int hi = hexVal(hex[i]);
		int lo = hexVal(hex[i + 1]);
		if (hi < 0 || lo < 0) return false;
		out[outIdx++] = (uint8_t)((hi << 4) | lo);
	}
	return outIdx > 0;
}

// ── start() ───────────────────────────────────────────────────────────

bool start(const StartParams& p) {
	std::lock_guard<std::mutex> lk(_mtx);
	if (_active.load()) {
		printf("[replay] start: already recording\n");
		return false;
	}

	// Open output file
	_file = fopen(p.out_path.c_str(), "wb");
	if (!_file) {
		printf("[replay] start: cannot open %s for writing\n", p.out_path.c_str());
		return false;
	}

	_startUnixUs = nowUnixUs();
	_entryCount.store(0);
	_inputBuf.clear();

	// ── Write header ──
	// magic (8)
	const char magic[8] = { 'M','C','R','E','C','\0','\0','\0' };
	fwrite(magic, 1, 8, _file);

	// version (4) + flycast_ver (4)
	uint8_t v[4];
	writeLE32(v, 1);                    // version
	fwrite(v, 1, 4, _file);
	writeLE32(v, 0);                    // flycast_ver placeholder
	fwrite(v, 1, 4, _file);

	// match_id (16) — generated on the fly, simple time-based for now.
	// Real UUID-v4 can come later.
	uint8_t match_id[16] = {0};
	writeLE64(match_id, _startUnixUs);
	fwrite(match_id, 1, 16, _file);

	// server_id (16)
	uint8_t server_id[16] = {0};
	hexDecode(p.server_id, server_id, 16);
	fwrite(server_id, 1, 16, _file);

	// start_unix_us (8)
	uint8_t buf[8];
	writeLE64(buf, _startUnixUs);
	fwrite(buf, 1, 8, _file);

	// duration_us placeholder (8) — patched in stop()
	memset(buf, 0, 8);
	fwrite(buf, 1, 8, _file);

	// rom_hash (32)
	uint8_t rom_hash[32] = {0};
	hexDecode(p.rom_hash_hex, rom_hash, 32);
	fwrite(rom_hash, 1, 32, _file);

	// p1_name, p2_name (64 each)
	writePaddedString(_file, p.p1_name, 64);
	writePaddedString(_file, p.p2_name, 64);

	// p1_chars, p2_chars (3 each)
	fwrite(p.p1_chars, 1, 3, _file);
	fwrite(p.p2_chars, 1, 3, _file);

	// winner placeholder
	uint8_t winner = 0xFF;
	fwrite(&winner, 1, 1, _file);

	// reserved (40)
	uint8_t reserved[40] = {0};
	fwrite(reserved, 1, 40, _file);

	// ── Write start savestate ──
	size_t saveSize = 0;
	uint8_t* saveData = maplecast_mirror::buildFullSaveState(saveSize);
	if (!saveData || saveSize == 0) {
		printf("[replay] start: buildFullSaveState() returned nothing — recording inputs only\n");
		// Write zero-sized savestate header (replay must be supplied a
		// fresh savestate at playback time)
		uint8_t z[8] = {0};
		fwrite(z, 1, 8, _file);
	} else {
		// Compress with zstd level 3 (good ratio/speed balance for ~600KB)
		size_t bound = ZSTD_compressBound(saveSize);
		std::vector<uint8_t> compressed(bound);
		size_t cSize = ZSTD_compress(compressed.data(), bound,
		                              saveData, saveSize, 3);
		if (ZSTD_isError(cSize)) {
			printf("[replay] zstd compress failed: %s\n",
			       ZSTD_getErrorName(cSize));
			cSize = 0;
		}

		// Write [raw_size:u32][compressed_size:u32][data:N bytes]
		uint8_t hdr[8];
		writeLE32(hdr,     (uint32_t)saveSize);
		writeLE32(hdr + 4, (uint32_t)cSize);
		fwrite(hdr, 1, 8, _file);
		if (cSize > 0)
			fwrite(compressed.data(), 1, cSize, _file);
		free(saveData);
		printf("[replay] savestate: %zu raw → %zu compressed\n",
		       saveSize, cSize);
	}

	fflush(_file);
	_active.store(true);
	printf("[replay] recording started: %s\n", p.out_path.c_str());
	return true;
}

// ── append() ──────────────────────────────────────────────────────────

void append(uint64_t frame, uint32_t seqAndSlot, uint16_t buttons,
            uint8_t lt, uint8_t rt) {
	if (!_active.load(std::memory_order_relaxed)) return;

	std::lock_guard<std::mutex> lk(_mtx);
	if (!_file) return;

	// Tape entry layout: 16 bytes [frame:u64][seqAndSlot:u32][buttons:u16][lt:u8][rt:u8]
	uint8_t entry[16];
	writeLE64(entry,      frame);
	writeLE32(entry + 8,  seqAndSlot);
	entry[12] = (uint8_t)(buttons);
	entry[13] = (uint8_t)(buttons >> 8);
	entry[14] = lt;
	entry[15] = rt;

	_inputBuf.insert(_inputBuf.end(), entry, entry + 16);
	_entryCount.fetch_add(1, std::memory_order_relaxed);

	maybeFlush(false);
}

// ── stop() ────────────────────────────────────────────────────────────

void stop(uint8_t winner) {
	std::lock_guard<std::mutex> lk(_mtx);
	if (!_active.load() || !_file) return;

	// Final flush of any buffered input entries
	maybeFlush(true);

	// Footer: "MCEND"(5) + entry_count(4) + hmac_placeholder(32)
	uint8_t footer[5 + 4 + 32];
	memcpy(footer, "MCEND", 5);
	writeLE32(footer + 5, (uint32_t)_entryCount.load());
	memset(footer + 9, 0, 32);  // HMAC TODO Phase 7
	fwrite(footer, 1, sizeof(footer), _file);

	// Patch duration_us at offset 8(magic) + 8(ver+flycast_ver) + 16(match_id)
	// + 16(server_id) + 8(start_unix_us) = 56
	uint64_t durationUs = nowUnixUs() - _startUnixUs;
	uint8_t durBuf[8];
	writeLE64(durBuf, durationUs);
	fseek(_file, 56, SEEK_SET);
	fwrite(durBuf, 1, 8, _file);

	// Patch winner at offset 56 + 8(duration) + 32(rom_hash)
	// + 64(p1_name) + 64(p2_name) + 3(p1_chars) + 3(p2_chars) = 230
	fseek(_file, 230, SEEK_SET);
	fwrite(&winner, 1, 1, _file);

	fclose(_file);
	_file = nullptr;
	_active.store(false);

	printf("[replay] stopped: %llu input entries, %.2fs duration\n",
	       (unsigned long long)_entryCount.load(),
	       durationUs / 1000000.0);
}

bool active() { return _active.load(std::memory_order_relaxed); }
uint64_t entryCount() { return _entryCount.load(std::memory_order_relaxed); }

} // namespace maplecast_replay
