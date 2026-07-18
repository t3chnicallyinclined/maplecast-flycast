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
#include "cfg/option.h"
#include "emulator.h"
#include "hw/pvr/spg.h"  // spg_last_jitter
#include "maplecast_rollback.h"
#include "oslib/oslib.h"
#include "serialize.h"
#include "types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <dirent.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
  #include <errno.h>
#endif

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
static std::string           _lastOutPath;

// ── Dataset state-stream (.mctele) state ──────────────────────────────
// Independent of the input recorder above. See replay_writer.h.
static std::atomic<bool>     _teleEnabled{false};    // MAPLECAST_RECORD_STATE
static std::atomic<bool>     _teleActive{false};     // stream opened
static FILE*                 _teleFile = nullptr;
static std::vector<uint8_t>  _teleBuf;               // batched frame entries
static std::mutex            _teleMtx;
static uint32_t              _teleBlobLen = 0;
static long                  _teleFirstFrameOff = 0; // header offset to patch at close

// Frame-alignment: stamps in the input log + checkpoint sidecar are
// relative to the first frame stamped after the deferred capture fires.
// Without this rebase the .mcrec would carry the recorder process's
// _localFrameNum (which is whatever it had ticked up to before recording
// started), but the replay process's counter resets to 0 at boot —
// entries would land N frames late in replay and the SH4 would run
// uncontrolled until the absolute counters happen to coincide.
static uint64_t              _firstFrame = UINT64_MAX;

// Dedicated savestate slot for replay record/restore. Outside the user-
// visible 0-9 range so manual saves are never clobbered.
static constexpr int REPLAY_SLOT = 99;

// Warmup frames: don't log inputs for the first N publishFrameTick calls
// after recording activates. Lets the SH4 settle into a deterministic
// post-loadstate groove before inputs start hitting, so any first-frame
// state slip doesn't corrupt the very first input's effect (e.g., the
// character-select cursor starting one position off).
//
// Configured via MAPLECAST_REPLAY_WARMUP env var (default 0). Stored in
// the .mcrec header so the reader knows how many frames to free-run
// before applying inputs.
static uint32_t              _warmupFrames = 0;

// V5-era arm_at_match plumbing — kept as no-op stubs for ABI compat with
// control_ws.cpp's record_start handler. Recording now fires only at
// autoload via MAPLECAST_REPLAY_OUT (V2 discipline), so these are dead.
static bool                  _armed = false;
static StartParams           _pendingParams;
static uint8_t               _prevInMatch = 0;

// Hotkey-trigger flag. Set by control-WS record_start endpoint after
// dc_savestate(REPLAY_SLOT) + emu.stop+start. Consumed by emulator.cpp's
// autoload section to know it should dc_loadstate from REPLAY_SLOT
// (mid-game state) rather than the user's slot 0.
static std::string           _nextRecordPath;
static std::mutex            _nextRecordMtx;

// Sidecar checkpoint file (<out_path>.ckpt). Opened lazily on the first
// checkpoint job processed by the worker. Layout: "MCCKPT\0\0" magic +
// version(4) + reserved(4) + repeated [frame:u64][size:u64][state_bytes:N].
// Entries hold raw dc_serialize bytes (same format as the .mcrec
// embedded savestate) so seekToFrame can apply via Deserializer +
// emu.loadstate with no slot/file/RZipFile detour.
static FILE*                 _ckptFile = nullptr;
static uint32_t              _ckptCount = 0;

// B.3: async checkpoint write. Old design did dc_savestate (zlib
// compress + slot file write) + readback + sidecar fwrite all on
// the renderer thread, blocking the SH4 for 50-150 ms at every
// checkpoint frame and producing a multi-frame stutter at 600/1200/...
//
// New design: emu thread captures dc_serialize (memcpy of DC state,
// no compression, ~5-15 ms) into a heap buffer and pushes to the
// queue. Worker thread fwrites to the sidecar.
struct CkptJob {
	uint64_t              frame;     // V4 relative
	std::vector<uint8_t>  bytes;     // raw dc_serialize output
};
static std::deque<CkptJob>      _ckptQueue;
static std::mutex               _ckptQueueMtx;
static std::condition_variable  _ckptQueueCv;
static std::atomic<bool>        _ckptWorkerRunning{false};
static std::atomic<bool>        _ckptShutdown{false};
static std::thread              _ckptWorker;

// Forward declarations for the async checkpoint worker (defined below).
static void startCkptWorker();
static void stopCkptWorker();

// Forward declaration for the per-match session input tap (defined at
// the bottom of this file in an anonymous namespace).
static void sessAppend(uint64_t frame, uint32_t seqAndSlot, uint16_t buttons,
                       uint8_t lt, uint8_t rt);

// Forward decl for the .mctele state-stream finalizer (registered via atexit).
static void closeStateStream();

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
//
// V2 discipline: called from emulator.cpp's autoload section, BEFORE
// the SH4 thread spawns. Both record and replay reach the post-load
// anchor via the SAME dc_loadstate(slot) code path at the SAME boot
// lifecycle moment. This is what worked in production for the May 7
// Magneto-tag-in validation (commit b732dfb6e) and what V5's mid-vblank
// captureFrameToBlob lost.
//
// Steps (run inline, not deferred):
//   1. Open .mcrec for write, write header
//   2. dc_savestate(REPLAY_SLOT) — canonical save path, writes .state file
//   3. Read .state bytes, embed in .mcrec
//   4. dc_loadstate(REPLAY_SLOT) — reload (anchor point replay will reach)
//   5. Activate writer

bool start(const StartParams& p) {
	std::lock_guard<std::mutex> lk(_mtx);
	if (_active.load()) {
		printf("[replay] start: already recording\n");
		return false;
	}

	_firstFrame = UINT64_MAX;
	_entryCount.store(0, std::memory_order_relaxed);

	// Warmup: read once at start. Skip appends with frame < warmup, so
	// SH4 free-runs that many frames from the post-loadstate anchor
	// before any input is logged.
	_warmupFrames = 0;
	if (const char* w = std::getenv("MAPLECAST_REPLAY_WARMUP")) {
		int n = std::atoi(w);
		if (n > 0) _warmupFrames = (uint32_t)n;
	}

	startCkptWorker();

	_file = fopen(p.out_path.c_str(), "wb");
	if (!_file) {
		printf("[replay] start: cannot open %s for writing\n", p.out_path.c_str());
		return false;
	}
	_startUnixUs = nowUnixUs();
	_lastOutPath = p.out_path;
	_inputBuf.clear();

	// ── header ──
	const char magic[8] = { 'M','C','R','E','C','\0','\0','\0' };
	fwrite(magic, 1, 8, _file);

	uint8_t v[4];
	writeLE32(v, 5); fwrite(v, 1, 4, _file);   // version (kept at 5 for now)
	writeLE32(v, 0); fwrite(v, 1, 4, _file);

	uint8_t match_id[16] = {0};
	writeLE64(match_id, _startUnixUs);
	fwrite(match_id, 1, 16, _file);

	uint8_t server_id[16] = {0};
	hexDecode(p.server_id, server_id, 16);
	fwrite(server_id, 1, 16, _file);

	uint8_t buf[8];
	writeLE64(buf, _startUnixUs); fwrite(buf, 1, 8, _file);
	memset(buf, 0, 8);            fwrite(buf, 1, 8, _file);  // duration_us patched in stop()

	uint8_t rom_hash[32] = {0};
	hexDecode(p.rom_hash_hex, rom_hash, 32);
	fwrite(rom_hash, 1, 32, _file);

	writePaddedString(_file, p.p1_name, 64);
	writePaddedString(_file, p.p2_name, 64);
	fwrite(p.p1_chars, 1, 3, _file);
	fwrite(p.p2_chars, 1, 3, _file);
	uint8_t winner = 0xFF;
	fwrite(&winner, 1, 1, _file);
	// reserved[40]: first 4 bytes carry warmup_frames (LE u32). Reader
	// uses this to set its baseline so input lookup aligns with the
	// recorder's first non-skipped publishFrameTick.
	uint8_t reserved[40] = {0};
	writeLE32(reserved, _warmupFrames);
	fwrite(reserved, 1, 40, _file);

	// ── savestate via canonical dc_savestate + dc_loadstate ──
	dc_savestate(REPLAY_SLOT);

	std::string statePath = hostfs::getSavestatePath(REPLAY_SLOT, false);
	FILE* sf = fopen(statePath.c_str(), "rb");
	if (!sf) {
		printf("[replay] start: cannot open %s after dc_savestate\n", statePath.c_str());
		fclose(_file); _file = nullptr;
		return false;
	}
	fseek(sf, 0, SEEK_END);
	long sz = ftell(sf);
	fseek(sf, 0, SEEK_SET);
	std::vector<uint8_t> stateBytes((size_t)sz);
	size_t got = fread(stateBytes.data(), 1, (size_t)sz, sf);
	fclose(sf);
	if (got != (size_t)sz) {
		printf("[replay] start: short read of %s\n", statePath.c_str());
		fclose(_file); _file = nullptr;
		return false;
	}

	uint8_t hdr[8];
	writeLE64(hdr, (uint64_t)sz);
	fwrite(hdr, 1, 8, _file);
	fwrite(stateBytes.data(), 1, (size_t)sz, _file);

	// Reload to set the post-load anchor — same state replay will reach.
	dc_loadstate(REPLAY_SLOT);

	// spg_jitter (kept for diagnostic compatibility with the file format).
	uint8_t jBuf[4];
	writeLE32(jBuf, (uint32_t)spg_last_jitter);
	fwrite(jBuf, 1, 4, _file);

	fflush(_file);
	_active.store(true);

	static bool atexitRegistered = false;
	if (!atexitRegistered) {
		atexit([]() { stop(0xFF); });
		atexitRegistered = true;
	}

	printf("[replay] recording started: %s (%ld state bytes via dc_savestate slot %d)\n",
	       p.out_path.c_str(), sz, REPLAY_SLOT);
	return true;
}

// V5-era no-op stub. V2 discipline does all the work inline in start()
// at autoload, so there's nothing to defer. Kept for emulator.cpp's
// vblank() hook ABI; safe to drop once that hook is removed.
bool executePendingCapture() { return false; }

// ── append() ──────────────────────────────────────────────────────────

void append(uint64_t frame, uint32_t seqAndSlot, uint16_t buttons,
            uint8_t lt, uint8_t rt) {
	// Per-match continuous recording: tap every input regardless of
	// whether the legacy single-file recorder is active. Defined later
	// in this file (visible via `using namespace` since we're in the
	// same TU).
	sessAppend(frame, seqAndSlot, buttons, lt, rt);

	if (!_active.load(std::memory_order_relaxed)) return;

	// Warmup skip: don't log appends with frame <= warmup. Recording's
	// first stored entry is at frame = warmup+1 (publishFrameTick fires
	// at end-of-frame-N with frame=N, so this skips the first N frames'
	// worth of inputs). The replayer's reader uses baseline=warmup to
	// align: replay's paceFrame=warmup (i.e., SH4 at vblank-of-frame-
	// warmup+1) hits entry rel=0, exactly when recording's SH4 first
	// logged an input.
	if (frame <= (uint64_t)_warmupFrames) return;

	std::lock_guard<std::mutex> lk(_mtx);
	if (!_file) return;

	// Rebase to relative frames so the recorder's absolute _localFrameNum
	// at record-start doesn't have to equal the replay's at restore-finish.
	// Reader pairs this with a baseline-capture in startPlayback().
	if (_firstFrame == UINT64_MAX) _firstFrame = frame;
	const uint64_t relFrame = frame - _firstFrame;

	// Tape entry layout: 16 bytes [frame:u64][seqAndSlot:u32][buttons:u16][lt:u8][rt:u8]
	uint8_t entry[16];
	writeLE64(entry,      relFrame);
	writeLE32(entry + 8,  seqAndSlot);
	entry[12] = (uint8_t)(buttons);
	entry[13] = (uint8_t)(buttons >> 8);
	entry[14] = lt;
	entry[15] = rt;

	_inputBuf.insert(_inputBuf.end(), entry, entry + 16);
	_entryCount.fetch_add(1, std::memory_order_relaxed);

	maybeFlush(false);
}

// ── checkpoint() ──────────────────────────────────────────────────────
//
// B.3 split: the EMU THREAD (this function) just snapshots dc_serialize
// into a heap buffer and queues it. The WORKER THREAD (ckptWorkerLoop
// below) does the sidecar fwrite. DC state is only safe to read at
// frame boundary, which is where serverPublish calls us — so the
// snapshot must happen synchronously here, but the I/O can wait.

static void ckptWorkerLoop() {
	while (true) {
		CkptJob job;
		{
			std::unique_lock<std::mutex> lk(_ckptQueueMtx);
			_ckptQueueCv.wait(lk, []{
				return _ckptShutdown.load() || !_ckptQueue.empty();
			});
			if (_ckptQueue.empty()) {
				if (_ckptShutdown.load()) return;
				continue;
			}
			job = std::move(_ckptQueue.front());
			_ckptQueue.pop_front();
		}

		// Lazy-open sidecar on first job. _lastOutPath is set by start()
		// before the worker is launched, so reading it without a lock is
		// safe (no concurrent writer during the worker's lifetime).
		if (!_ckptFile && !_lastOutPath.empty()) {
			std::string ckptPath = _lastOutPath + ".ckpt";
			_ckptFile = fopen(ckptPath.c_str(), "wb");
			if (!_ckptFile) {
				printf("[replay-ckpt] worker: cannot open sidecar %s\n",
				       ckptPath.c_str());
				continue;
			}
			const uint8_t magic[8] = {'M','C','C','K','P','T','\0','\0'};
			fwrite(magic, 1, 8, _ckptFile);
			uint8_t hdr[8];
			writeLE32(hdr,     2);  // sidecar version
			writeLE32(hdr + 4, 0);  // reserved
			fwrite(hdr, 1, 8, _ckptFile);
			printf("[replay-ckpt] sidecar opened: %s\n", ckptPath.c_str());
		}

		if (!_ckptFile) continue;

		// Append [frame:u64][size:u64][state_bytes:N]
		uint8_t entryHdr[16];
		writeLE64(entryHdr,     job.frame);
		writeLE64(entryHdr + 8, (uint64_t)job.bytes.size());
		fwrite(entryHdr, 1, 16, _ckptFile);
		fwrite(job.bytes.data(), 1, job.bytes.size(), _ckptFile);
		fflush(_ckptFile);
		_ckptCount++;
		printf("[replay-ckpt] worker wrote frame=%llu size=%zu (#%u)\n",
		       (unsigned long long)job.frame, job.bytes.size(), _ckptCount);
	}
}

static void startCkptWorker() {
	if (_ckptWorkerRunning.exchange(true)) return;
	_ckptShutdown.store(false);
	_ckptCount = 0;
	_ckptWorker = std::thread(ckptWorkerLoop);
}

static void stopCkptWorker() {
	if (!_ckptWorkerRunning.exchange(false)) return;
	{
		std::lock_guard<std::mutex> lk(_ckptQueueMtx);
		_ckptShutdown.store(true);
	}
	_ckptQueueCv.notify_all();
	if (_ckptWorker.joinable()) _ckptWorker.join();
	if (_ckptFile) {
		fclose(_ckptFile);
		_ckptFile = nullptr;
		printf("[replay-ckpt] sidecar closed (%u checkpoints)\n", _ckptCount);
	}
}

void checkpoint(uint64_t frame) {
	if (!_active.load(std::memory_order_relaxed)) return;

	uint64_t relFrame;
	{
		std::lock_guard<std::mutex> lk(_mtx);
		if (_lastOutPath.empty()) return;
		// Rebase to relative frames so checkpoints stay aligned with
		// input entries. If checkpoint() lands before any append() (won't
		// happen in practice — append fires every published tick — but
		// belt-and-suspenders), set the baseline here too.
		if (_firstFrame == UINT64_MAX) _firstFrame = frame;
		relFrame = frame - _firstFrame;
	}

	// EMU THREAD: dc_serialize into heap buffer (memcpy of DC state, no
	// compression). Fast — typical 5-15 ms vs the 50-150 ms zlib path
	// that used to live here. DC state is safe to read here because
	// serverPublish has the SH4 paused at frame boundary.
	std::vector<uint8_t> bytes(40 * 1024 * 1024);
	size_t sz = 0;
	try {
		Serializer ser(bytes.data(), bytes.size(), false);
		dc_serialize(ser);
		sz = ser.size();
	} catch (const Serializer::Exception& e) {
		printf("[replay-ckpt] dc_serialize failed at frame %llu: %s\n",
		       (unsigned long long)relFrame, e.what());
		return;
	}
	bytes.resize(sz);

	// Hand off to the worker thread for fwrite. Returns immediately.
	{
		std::lock_guard<std::mutex> lk(_ckptQueueMtx);
		_ckptQueue.push_back({relFrame, std::move(bytes)});
	}
	_ckptQueueCv.notify_one();
}

// ── Upload (optional, triggered by MAPLECAST_REPLAY_UPLOAD_URL) ───────
//
// After stop() finalizes the file, if MAPLECAST_REPLAY_UPLOAD_URL is set
// (e.g. "https://nobd.net/hub/api/replays"), POST the file bytes via
// libcurl. Returns the id + download URL the server echoes back.

static size_t curlWriteToString(void* contents, size_t size, size_t nmemb, std::string* s) {
	s->append((char*)contents, size * nmemb);
	return size * nmemb;
}

static void uploadToHub(const std::string& filePath, const std::string& hubUrl) {
	// Read the file into memory
	FILE* f = fopen(filePath.c_str(), "rb");
	if (!f) {
		printf("[replay-upload] cannot open %s\n", filePath.c_str());
		return;
	}
	fseek(f, 0, SEEK_END);
	size_t sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> body(sz);
	if (fread(body.data(), 1, sz, f) != sz) {
		fclose(f);
		printf("[replay-upload] file read failed\n");
		return;
	}
	fclose(f);

	CURL* curl = curl_easy_init();
	if (!curl) return;

	std::string response;
	struct curl_slist* headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/octet-stream");

	curl_easy_setopt(curl, CURLOPT_URL, hubUrl.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)sz);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToString);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

	CURLcode rc = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (rc != CURLE_OK) {
		printf("[replay-upload] failed: %s\n", curl_easy_strerror(rc));
		return;
	}
	if (http_code >= 200 && http_code < 300) {
		printf("[replay-upload] success (%ld bytes → %s): %s\n",
		       (long)sz, hubUrl.c_str(), response.c_str());
	} else {
		printf("[replay-upload] HTTP %ld: %s\n", http_code, response.c_str());
	}
}

// ── stop() ────────────────────────────────────────────────────────────

void stop(uint8_t winner) {
	std::lock_guard<std::mutex> lk(_mtx);
	// If we were merely armed (no capture fired yet), treat stop() as a
	// disarm. No file was opened, no inputs logged — nothing to finalize.
	if (_armed && !_active.load()) {
		_armed = false;
		_pendingParams = {};
		printf("[replay] stop: was armed but never fired — disarmed\n");
		return;
	}
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

	// Drain remaining checkpoint jobs and close the sidecar. stopCkptWorker
	// blocks until the worker has flushed every queued job, so the .ckpt
	// is always complete on the disk when we return.
	stopCkptWorker();

	printf("[replay] stopped: %llu input entries, %.2fs duration\n",
	       (unsigned long long)_entryCount.load(),
	       durationUs / 1000000.0);

	// Optional upload to hub. We stashed the out_path for this.
	if (const char* uploadUrl = std::getenv("MAPLECAST_REPLAY_UPLOAD_URL")) {
		static std::string lastPath;  // persists for the upload thread
		lastPath = _lastOutPath;
		std::string url = uploadUrl;
		std::thread([url]() {
			uploadToHub(lastPath, url);
		}).detach();
	}
}

bool active() { return _active.load(std::memory_order_relaxed); }
uint64_t entryCount() { return _entryCount.load(std::memory_order_relaxed); }

bool startInteractive(const StartParams& p)
{
	// Halt SH4 at instruction boundary, run start() (which dc_savestate's
	// + dc_loadstate's slot 99 internally), then resume. Same bracket
	// pattern that control-WS and mirror-WS record_start handlers use.
	emu.stop();
	const bool ok = start(p);
	emu.start();
	return ok;
}

// arm_at_match is no longer supported under V2 discipline (recording must
// fire at autoload boundary, not mid-match). This remains as a no-op stub
// for ABI compatibility with control_ws.cpp's record_start handler.
bool armed() { return false; }

void setNextRecordPath(const std::string& path)
{
	std::lock_guard<std::mutex> lk(_nextRecordMtx);
	_nextRecordPath = path;
}

std::string consumeNextRecordPath()
{
	std::lock_guard<std::mutex> lk(_nextRecordMtx);
	std::string p = _nextRecordPath;
	_nextRecordPath.clear();
	return p;
}

bool hasNextRecordPath()
{
	std::lock_guard<std::mutex> lk(_nextRecordMtx);
	return !_nextRecordPath.empty();
}

// ── Per-match continuous recording ────────────────────────────────────
// State + mechanics for MAPLECAST_RECORD_MATCHES. See replay_writer.h
// for the design. Independent of the legacy single-file path above so
// they can coexist without stepping on each other (e.g., a session
// recorder running while a one-shot record_start fires for some other
// reason).

namespace {

std::atomic<bool>            _sessActive{false};
std::vector<uint8_t>         _sessSavestate;        // captured once at autoload
std::vector<uint8_t>         _sessInputs;           // 16 bytes per TapeEntry, frame 0 onwards
uint64_t                     _sessFirstFrame = UINT64_MAX;
uint64_t                     _sessLastRelFrame = 0; // updated by sessAppend
std::string                  _sessDir;
uint64_t                     _sessStartUnixUs = 0;
uint8_t                      _sessPrevInMatch = 0;
uint64_t                     _sessMatchStartFrame = 0;
bool                         _sessMatchInProgress = false;
uint32_t                     _sessMatchSeq = 0;
std::mutex                   _sessMtx;

// Filesystem helpers ---------------------------------------------------

bool ensureDir(const std::string& dir)
{
#ifdef _WIN32
	if (CreateDirectoryA(dir.c_str(), nullptr) != 0) return true;
	return GetLastError() == ERROR_ALREADY_EXISTS;
#else
	if (mkdir(dir.c_str(), 0755) == 0) return true;
	return errno == EEXIST;
#endif
}

// Delete .mcrec files in dir whose mtime is older than retentionDays.
// Best-effort: failures are logged but don't abort.
void cleanupOldRecordings(const std::string& dir, int retentionDays)
{
	if (retentionDays <= 0) return;
	const std::time_t now = std::time(nullptr);
	const std::time_t cutoff = now - (std::time_t)retentionDays * 86400;
	int deleted = 0;

#ifdef _WIN32
	const std::string pat = dir + "\\*.mcrec";
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pat.c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		ULARGE_INTEGER ull;
		ull.LowPart  = fd.ftLastWriteTime.dwLowDateTime;
		ull.HighPart = fd.ftLastWriteTime.dwHighDateTime;
		// FILETIME: 100ns ticks since 1601. Convert to unix seconds.
		const std::time_t mtime = (std::time_t)((ull.QuadPart - 116444736000000000ULL) / 10000000ULL);
		if (mtime < cutoff) {
			std::string p = dir + "\\" + fd.cFileName;
			if (DeleteFileA(p.c_str())) deleted++;
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR* d = opendir(dir.c_str());
	if (!d) return;
	struct dirent* e;
	while ((e = readdir(d)) != nullptr) {
		const char* name = e->d_name;
		size_t n = strlen(name);
		if (n < 7 || strcmp(name + n - 6, ".mcrec") != 0) continue;
		std::string p = dir + "/" + name;
		struct stat st;
		if (stat(p.c_str(), &st) != 0) continue;
		if (st.st_mtime < cutoff) {
			if (unlink(p.c_str()) == 0) deleted++;
		}
	}
	closedir(d);
#endif
	if (deleted > 0)
		printf("[record-matches] retention: deleted %d file(s) older than %d days\n",
		       deleted, retentionDays);
}

// Build a match file path: <dir>/match-<sessionStamp>-<seq>.mcrec
std::string buildMatchPath(const std::string& dir, uint64_t sessionUnixUs, uint32_t seq)
{
	std::time_t t = (std::time_t)(sessionUnixUs / 1000000);
	std::tm tm;
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	char ts[32];
	std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
	char tail[64];
	std::snprintf(tail, sizeof(tail), "match-%s-%u.mcrec", ts, seq);
#ifdef _WIN32
	return dir + "\\" + tail;
#else
	return dir + "/" + tail;
#endif
}

// Write a match file. Same on-disk layout as legacy single-file recordings
// (magic + version 5 + metadata + START_SAVESTATE + input log + footer)
// so the existing reader can consume it. The start_frame field is stored
// in the reserved[40] block at offset 8 (after the warmup u32 already
// kept there for back-compat, which we leave as 0).
void writeMatchFile(const std::string& path,
                    const std::vector<uint8_t>& savestate,
                    const std::vector<uint8_t>& inputs,
                    uint64_t startFrame,
                    uint32_t entryCount,
                    uint64_t sessionUnixUs)
{
	FILE* f = fopen(path.c_str(), "wb");
	if (!f) {
		printf("[record-matches] write FAIL: cannot open %s\n", path.c_str());
		return;
	}

	const char magic[8] = { 'M','C','R','E','C','\0','\0','\0' };
	fwrite(magic, 1, 8, f);

	uint8_t v[4];
	writeLE32(v, 5); fwrite(v, 1, 4, f);   // version
	writeLE32(v, 0); fwrite(v, 1, 4, f);   // flycast_ver

	uint8_t match_id[16] = {0};
	writeLE64(match_id, sessionUnixUs);
	fwrite(match_id, 1, 16, f);

	uint8_t server_id[16] = {0};
	fwrite(server_id, 1, 16, f);

	uint8_t buf[8];
	writeLE64(buf, sessionUnixUs); fwrite(buf, 1, 8, f);  // start_unix_us
	memset(buf, 0, 8);             fwrite(buf, 1, 8, f);  // duration_us

	uint8_t rom_hash[32] = {0};
	fwrite(rom_hash, 1, 32, f);

	writePaddedString(f, "", 64);  // p1_name
	writePaddedString(f, "", 64);  // p2_name
	uint8_t chars[3] = {0xFF, 0xFF, 0xFF};
	fwrite(chars, 1, 3, f);
	fwrite(chars, 1, 3, f);
	uint8_t winner = 0xFF;
	fwrite(&winner, 1, 1, f);

	// reserved[40]: leave warmup at u32[0]=0, store start_frame at u64[1..2].
	uint8_t reserved[40] = {0};
	writeLE64(reserved + 8, startFrame);   // <-- the new field
	fwrite(reserved, 1, 40, f);

	// START_SAVESTATE block (raw_size:u64 + bytes). Same shape the writer
	// uses for single-file mode.
	uint8_t hdr[8];
	writeLE64(hdr, (uint64_t)savestate.size());
	fwrite(hdr, 1, 8, f);
	fwrite(savestate.data(), 1, savestate.size(), f);

	// spg_jitter trailer (kept for diagnostic compat; 0 here since the
	// session savestate was captured at autoload, not at vblank).
	uint8_t jBuf[4] = {0};
	fwrite(jBuf, 1, 4, f);

	// Input log.
	if (!inputs.empty())
		fwrite(inputs.data(), 1, inputs.size(), f);

	// Footer: "MCEND" + entry_count(u32) + hmac(32, zeroed for now).
	const char foot[5] = {'M','C','E','N','D'};
	fwrite(foot, 1, 5, f);
	writeLE32(v, entryCount);
	fwrite(v, 1, 4, f);
	uint8_t hmac[32] = {0};
	fwrite(hmac, 1, 32, f);

	fflush(f);
	long sz = ftell(f);
	fclose(f);

	printf("[record-matches] wrote %s (%ld bytes, %u inputs, start_frame=%llu)\n",
	       path.c_str(), sz, entryCount, (unsigned long long)startFrame);
	fflush(stdout);
}

// Spawn a detached thread to write a match file. Snapshots inputs under
// the session lock so the live ring keeps growing during the I/O.
void spawnMatchWrite(uint64_t startFrame)
{
	std::vector<uint8_t> inputsCopy;
	std::vector<uint8_t> savestateRef;
	std::string path;
	uint32_t entryCount;
	uint64_t sessionUnixUs;
	{
		std::lock_guard<std::mutex> lk(_sessMtx);
		inputsCopy   = _sessInputs;                  // copy
		savestateRef = _sessSavestate;               // copy (~7 MB)
		path         = buildMatchPath(_sessDir, _sessStartUnixUs, _sessMatchSeq);
		entryCount   = (uint32_t)(inputsCopy.size() / 16);
		sessionUnixUs = _sessStartUnixUs;
	}
	std::thread([path, savestateRef = std::move(savestateRef),
	             inputsCopy = std::move(inputsCopy), startFrame,
	             entryCount, sessionUnixUs]() {
		writeMatchFile(path, savestateRef, inputsCopy,
		               startFrame, entryCount, sessionUnixUs);
	}).detach();
}

} // namespace (per-match)

// Append one input entry to the always-on session log. Called from the
// existing append() path so per-match recording piggybacks on the same
// hot-path the legacy recorder already uses. Defined OUTSIDE the anon
// namespace so the forward declaration at the top of the file links.
static void sessAppend(uint64_t frame, uint32_t seqAndSlot, uint16_t buttons,
                       uint8_t lt, uint8_t rt)
{
	if (!_sessActive.load(std::memory_order_relaxed)) return;
	std::lock_guard<std::mutex> lk(_sessMtx);
	if (_sessFirstFrame == UINT64_MAX) _sessFirstFrame = frame;
	const uint64_t rel = frame - _sessFirstFrame;
	_sessLastRelFrame = rel;

	uint8_t entry[16];
	writeLE64(entry,      rel);
	writeLE32(entry + 8,  seqAndSlot);
	entry[12] = (uint8_t)(buttons);
	entry[13] = (uint8_t)(buttons >> 8);
	entry[14] = lt;
	entry[15] = rt;
	_sessInputs.insert(_sessInputs.end(), entry, entry + 16);
}

bool initMatchRecording(const std::string& dir, int retentionDays)
{
	if (_sessActive.load()) {
		printf("[record-matches] already initialized -- ignoring\n");
		return true;
	}
	if (!ensureDir(dir)) {
		printf("[record-matches] cannot create dir: %s\n", dir.c_str());
		return false;
	}
	cleanupOldRecordings(dir, retentionDays);

	// Dataset state-stream gate (see replay_writer.h). Independent of the
	// input recorder: .mctele is written only when MAPLECAST_RECORD_STATE is set.
	_teleEnabled.store(std::getenv("MAPLECAST_RECORD_STATE") != nullptr,
	                   std::memory_order_relaxed);

	// Capture the autoload savestate ONCE. Same dc_savestate +
	// dc_loadstate dance the single-file recorder does, just keeping
	// the bytes in memory instead of writing to a .mcrec.
	dc_savestate(REPLAY_SLOT);
	std::string statePath = hostfs::getSavestatePath(REPLAY_SLOT, false);
	FILE* sf = fopen(statePath.c_str(), "rb");
	if (!sf) {
		printf("[record-matches] cannot open %s after dc_savestate\n", statePath.c_str());
		return false;
	}
	fseek(sf, 0, SEEK_END);
	long sz = ftell(sf);
	fseek(sf, 0, SEEK_SET);
	std::vector<uint8_t> bytes((size_t)sz);
	size_t got = fread(bytes.data(), 1, (size_t)sz, sf);
	fclose(sf);
	if (got != (size_t)sz) {
		printf("[record-matches] short read of %s\n", statePath.c_str());
		return false;
	}
	dc_loadstate(REPLAY_SLOT);

	{
		std::lock_guard<std::mutex> lk(_sessMtx);
		_sessSavestate       = std::move(bytes);
		_sessInputs.clear();
		_sessFirstFrame      = UINT64_MAX;
		_sessLastRelFrame    = 0;
		_sessDir             = dir;
		_sessStartUnixUs     = nowUnixUs();
		_sessPrevInMatch     = 0;
		_sessMatchStartFrame = 0;
		_sessMatchInProgress = false;
		_sessMatchSeq        = 0;
	}
	_sessActive.store(true);

	printf("[record-matches] active. dir=%s, savestate=%ld bytes, retention=%d days\n",
	       dir.c_str(), sz, retentionDays);
	fflush(stdout);
	return true;
}

bool matchRecordingActive()
{
	return _sessActive.load(std::memory_order_relaxed);
}

void onFrameInMatchFlag(uint8_t in_match)
{
	if (!_sessActive.load(std::memory_order_relaxed)) return;

	uint8_t prev;
	uint64_t curRel;
	{
		std::lock_guard<std::mutex> lk(_sessMtx);
		prev   = _sessPrevInMatch;
		curRel = _sessLastRelFrame;
		_sessPrevInMatch = in_match;
	}

	if (prev == 0 && in_match == 1) {
		std::lock_guard<std::mutex> lk(_sessMtx);
		_sessMatchStartFrame = curRel;
		_sessMatchInProgress = true;
		_sessMatchSeq++;
		printf("[record-matches] match #%u start at relFrame=%llu\n",
		       _sessMatchSeq, (unsigned long long)curRel);
		fflush(stdout);
	} else if (prev == 1 && in_match == 0) {
		uint64_t startFrame = 0;
		bool inProgress;
		{
			std::lock_guard<std::mutex> lk(_sessMtx);
			startFrame = _sessMatchStartFrame;
			inProgress = _sessMatchInProgress;
			_sessMatchInProgress = false;
		}
		if (inProgress) {
			printf("[record-matches] match #%u end at relFrame=%llu (writing async)\n",
			       _sessMatchSeq, (unsigned long long)curRel);
			fflush(stdout);
			spawnMatchWrite(startFrame);
		}
	}
}

// ── Dataset state-stream (.mctele) — see replay_writer.h ──────────────

bool stateRecordingEnabled() { return _teleEnabled.load(std::memory_order_relaxed); }

static void closeStateStream()
{
	std::lock_guard<std::mutex> lk(_teleMtx);
	if (!_teleFile) return;
	if (!_teleBuf.empty()) {
		fwrite(_teleBuf.data(), 1, _teleBuf.size(), _teleFile);
		_teleBuf.clear();
	}
	// Patch sess_first_frame (the session frame origin the input log rebases
	// to) into the header so the offline join can map input relFrames to the
	// absolute frame keys used here.
	uint64_t ff;
	{ std::lock_guard<std::mutex> slk(_sessMtx); ff = _sessFirstFrame; }
	fseek(_teleFile, _teleFirstFrameOff, SEEK_SET);
	uint8_t b[8]; writeLE64(b, ff);
	fwrite(b, 1, 8, _teleFile);
	fclose(_teleFile);
	_teleFile = nullptr;
	_teleActive.store(false, std::memory_order_relaxed);
	printf("[record-state] .mctele closed (sess_first_frame=%llu)\n",
	       (unsigned long long)ff);
	fflush(stdout);
}

void beginStateStream(const StateSeg* segs, uint32_t nsegs, uint32_t blobLen)
{
	std::lock_guard<std::mutex> lk(_teleMtx);
	if (_teleActive.load(std::memory_order_relaxed)) return;
	if (!_teleEnabled.load(std::memory_order_relaxed)) return;

	std::string dir; uint64_t stampUs;
	{
		std::lock_guard<std::mutex> slk(_sessMtx);
		dir = _sessDir; stampUs = _sessStartUnixUs;
	}
	if (dir.empty()) return;

	std::time_t t = (std::time_t)(stampUs / 1000000);
	std::tm tm;
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	char ts[32];   std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
	char tail[64]; std::snprintf(tail, sizeof(tail), "session-%s.mctele", ts);
#ifdef _WIN32
	std::string path = dir + "\\" + tail;
#else
	std::string path = dir + "/" + tail;
#endif

	_teleFile = fopen(path.c_str(), "wb");
	if (!_teleFile) {
		printf("[record-state] cannot open %s\n", path.c_str());
		return;
	}

	// Header: magic + version + blob_len + num_segs + [addr,len]*n +
	// sess_first_frame(u64, patched at close) + reserved[16].
	const char magic[8] = { 'M','C','T','E','L','E','0','1' };
	fwrite(magic, 1, 8, _teleFile);
	uint8_t v[4];
	writeLE32(v, 1);       fwrite(v, 1, 4, _teleFile);   // version
	writeLE32(v, blobLen); fwrite(v, 1, 4, _teleFile);   // blob_len
	writeLE32(v, nsegs);   fwrite(v, 1, 4, _teleFile);   // num_segs
	for (uint32_t i = 0; i < nsegs; i++) {
		writeLE32(v, segs[i].addr); fwrite(v, 1, 4, _teleFile);
		writeLE32(v, segs[i].len);  fwrite(v, 1, 4, _teleFile);
	}
	_teleFirstFrameOff = ftell(_teleFile);
	uint8_t ff[8]; writeLE64(ff, UINT64_MAX); fwrite(ff, 1, 8, _teleFile);
	uint8_t reserved[16] = {0}; fwrite(reserved, 1, 16, _teleFile);

	_teleBlobLen = blobLen;
	_teleBuf.clear();
	_teleActive.store(true, std::memory_order_relaxed);
	fflush(_teleFile);

	static bool atexitRegistered = false;
	if (!atexitRegistered) { atexit([]() { closeStateStream(); }); atexitRegistered = true; }

	printf("[record-state] .mctele open: %s (blob=%u B, %u segs)\n",
	       path.c_str(), blobLen, nsegs);
	fflush(stdout);
}

void appendState(uint64_t frame, const uint8_t* blob, uint32_t len)
{
	if (!_teleActive.load(std::memory_order_relaxed)) return;
	std::lock_guard<std::mutex> lk(_teleMtx);
	if (!_teleFile || len != _teleBlobLen) return;

	uint8_t hdr[8]; writeLE64(hdr, frame);
	_teleBuf.insert(_teleBuf.end(), hdr, hdr + 8);
	_teleBuf.insert(_teleBuf.end(), blob, blob + len);

	if (_teleBuf.size() >= 256 * 1024) {   // flush ~every 256 KB off-thread of the sim
		fwrite(_teleBuf.data(), 1, _teleBuf.size(), _teleFile);
		_teleBuf.clear();
	}
}

} // namespace maplecast_replay
