/*
	MapleCast Visual Cache — self-building game state → TA mapping.

	Every frame during gameplay:
	1. Read game state from RAM (character, animation, frame timer)
	2. Hash it → unique key for this visual state
	3. If not in cache → serialize TA display list → write to disk
	4. Record transition edge (prev state → this state)

	The cache grows as you play. After enough sessions,
	it covers every visual state the game can produce.
	Then: 253 bytes of game state → pixel-perfect rendering.
*/
#include "types.h"
#include "maplecast_visual_cache.h"
#include "maplecast_gamestate.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/mem/addrspace.h"
#include "rend/TexCache.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <atomic>
#include <sys/stat.h>

namespace maplecast_visual_cache
{

// On-disk file format version. Bumped to 2 when per-frame texture refs were
// added (Phase 1 of Option 6 — VRAM-rot fix). Older v1 files are still
// readable via a header check, but won't have a TexRefs section.
static constexpr uint32_t FILE_VERSION = 2;
static constexpr uint32_t FILE_MAGIC = 0x56495343;  // "VISC"

// Per-frame texture reference. The pool of decoded RGBA pixels lives in
// separate tex_<addr>_<tcw>_<wxh>.bin files (written by captureTexture).
// A cache entry stores a list of these refs so a replay client can fetch
// exactly the texture set the frame needs without scanning the live VRAM.
struct TexRef {
    uint32_t startAddress;
    uint32_t tcwFull;
    uint16_t width;
    uint16_t height;
    bool operator==(const TexRef& o) const {
        return startAddress == o.startAddress && tcwFull == o.tcwFull
            && width == o.width && height == o.height;
    }
};

static std::string _cacheDir;
static std::unordered_set<uint64_t> _knownStates;
static std::mutex _cacheMutex;
static std::atomic<uint64_t> _totalFrames{0};
static std::atomic<uint64_t> _cacheHits{0};
static std::atomic<uint64_t> _cacheMisses{0};
static std::atomic<uint64_t> _totalBytes{0};
static uint64_t _prevStateHash = 0;
static uint32_t _transitionCount = 0;
static bool _initialized = false;

// Per-frame texture-ref accumulator. captureTexture() (called from
// TexCache.cpp) appends here when flycast decodes a NEW texture this frame.
// recordFrame() additionally walks the rend_context's poly lists to add
// any textures already cached from prior frames (where captureTexture
// didn't fire). Cleared at the start of each recordFrame() call.
//
// Render thread is the only producer/consumer, so no mutex needed — but
// the storage is at file scope to make it accessible from both
// captureTexture() and recordFrame() without plumbing arguments through.
static thread_local std::vector<TexRef> _frameTextures;

// CSV-informed visual key (Phase 2). Replaces the 5-field key the
// 2026-04 prototype used. 11 bytes per character, decomposed from
// PlayerMemoryAddresses.csv so each field is a single bounded enum:
//
//   char_id          +0x001 — 0..58 roster ID
//   knockdown_state  +0x1D0 — 35-value state machine (was read as u16
//                              previously, but the CSV shows the high
//                              byte is unrelated; reading u8 cuts noise)
//   anim_animID      +0x158 — animation within a state
//   anim_groupID     +0x159 — animation sub-group (Is_Prox_Block)
//   key_frame        +0x142 — keyframe count within the anim (was u16
//                              anim_timer; the lo byte is the keyframe)
//   attack_number    +0x1A1 — current attack ID (per-char enum)
//   facing           +0x110 — direction
//   palette_id       +0x52D — color slot
//   sprite_id        +0x144 — current sprite frame (u16)
//   airborne         +0x1F9 — 0/1/2 stand/crouch/air
//
// Per the Option 6 plan in the planning doc: this is the unblocker for
// the visual_cache hit-rate problem the 2026-04 prototype hit (77%
// unique rate from 600 frames). Finer key = better discrimination, but
// correctness-wise it never collides — same engine state will produce
// the same key whereas the old 5-field key could collapse visually
// different frames into the same hash.
struct VisualKey {
	uint8_t  char_id;
	uint8_t  knockdown_state;
	uint8_t  anim_animID;
	uint8_t  anim_groupID;
	uint8_t  key_frame;
	uint8_t  attack_number;
	uint8_t  facing;
	uint8_t  palette_id;
	uint16_t sprite_id;
	uint8_t  airborne;
	uint8_t  _pad;
};
static_assert(sizeof(VisualKey) == 12, "VisualKey size must be stable for hashing");

// Per-char base addresses (matches maplecast_gamestate.cpp exactly).
// Stride 0x5A4, interleaved P1C1, P2C1, P1C2, P2C2, P1C3, P2C3.
static const uint32_t VK_CHAR_BASE[] = {
	0x8C268340, 0x8C2688E4, 0x8C268E88,
	0x8C26942C, 0x8C2699D0, 0x8C269F74,
};

// Field offsets — ALL come from MvC2Data PlayerMemoryAddresses.csv.
static const uint32_t VK_OFF_ACTIVE          = 0x000;
static const uint32_t VK_OFF_CHAR_ID         = 0x001;
static const uint32_t VK_OFF_KEYFRAME        = 0x142;  // u8 (csv: Anim_KeyFrame_FrameCount)
static const uint32_t VK_OFF_SPRITE_ID       = 0x144;  // u16 (csv: Animation_Value)
static const uint32_t VK_OFF_ANIM_ANIMID     = 0x158;
static const uint32_t VK_OFF_ANIM_GROUPID    = 0x159;
static const uint32_t VK_OFF_ATTACK_NUMBER   = 0x1A1;
static const uint32_t VK_OFF_KNOCKDOWN_STATE = 0x1D0;  // u8 per csv
static const uint32_t VK_OFF_AIRBORNE        = 0x1F9;
static const uint32_t VK_OFF_FACING          = 0x110;
static const uint32_t VK_OFF_PALETTE_ID      = 0x52D;

static void readCharVisualKey(int slot, VisualKey& vk)
{
	uint32_t base = VK_CHAR_BASE[slot];
	vk.char_id         = (uint8_t)addrspace::read8(base + VK_OFF_CHAR_ID);
	vk.knockdown_state = (uint8_t)addrspace::read8(base + VK_OFF_KNOCKDOWN_STATE);
	vk.anim_animID     = (uint8_t)addrspace::read8(base + VK_OFF_ANIM_ANIMID);
	vk.anim_groupID    = (uint8_t)addrspace::read8(base + VK_OFF_ANIM_GROUPID);
	vk.key_frame       = (uint8_t)addrspace::read8(base + VK_OFF_KEYFRAME);
	vk.attack_number   = (uint8_t)addrspace::read8(base + VK_OFF_ATTACK_NUMBER);
	vk.facing          = (uint8_t)addrspace::read8(base + VK_OFF_FACING);
	vk.palette_id      = (uint8_t)addrspace::read8(base + VK_OFF_PALETTE_ID);
	vk.sprite_id       = (uint16_t)addrspace::read16(base + VK_OFF_SPRITE_ID);
	vk.airborne        = (uint8_t)addrspace::read8(base + VK_OFF_AIRBORNE);
	vk._pad            = 0;
}

// Stable 64-bit hash over the 12 raw bytes of a VisualKey. We use
// FNV-1a — same as the 2026-04 lookup_test rig, which makes apples-
// to-apples hit-rate comparisons against that test set straightforward.
static uint64_t hashVisualKey(const VisualKey& vk)
{
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&vk);
	uint64_t h = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < sizeof(VisualKey); i++) {
		h ^= p[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

// Hash the full frame state: all 6 characters combined + minimal
// global context. Only `active` characters contribute (assist that
// hasn't tagged in is engine-internal-state with no on-screen sprite).
// stage_id is included so the same fighter pose on Bridge1 vs Cave2
// hashes differently — the cache's TA buffer has the stage geometry
// baked in, so the key must too. in_match always == 1 here (recordFrame
// returns early otherwise) but including it costs nothing and guards
// against future code changes.
static uint64_t hashFrameStateV2(const maplecast_gamestate::GameState& gs)
{
	uint64_t h = 0;
	for (int i = 0; i < 6; i++)
	{
		if (!gs.chars[i].active) continue;
		VisualKey vk;
		readCharVisualKey(i, vk);
		uint64_t ch = hashVisualKey(vk);
		h ^= ch + 0x9e3779b9ULL + (h << 6) + (h >> 2);  // boost::hash_combine
	}
	h ^= (uint64_t)gs.stage_id << 40;
	h ^= (uint64_t)gs.in_match << 48;
	return h;
}

// Serialize a TA polygon's essential data (no pointers, fixed size)
struct SerializedPoly {
	uint32_t first, count;
	uint32_t tsp, tcw, pcw, isp;
	float zvZ;
	uint32_t tileclip;
	uint32_t tsp1, tcw1;
	uint32_t texAddr;      // texture VRAM address (0 if no texture)
	uint16_t texW, texH;   // texture dimensions
};

static void serializePoly(const PolyParam& pp, SerializedPoly& sp)
{
	sp.first = pp.first;
	sp.count = pp.count;
	sp.tsp = pp.tsp.full;
	sp.tcw = pp.tcw.full;
	sp.pcw = pp.pcw.full;
	sp.isp = pp.isp.full;
	sp.zvZ = pp.zvZ;
	sp.tileclip = pp.tileclip;
	sp.tsp1 = pp.tsp1.full;
	sp.tcw1 = pp.tcw1.full;
	sp.texAddr = pp.texture ? pp.texture->startAddress : 0;
	sp.texW = pp.texture ? pp.texture->width : 0;
	sp.texH = pp.texture ? pp.texture->height : 0;
}

static bool writeFrameToDisk(uint64_t stateHash, const rend_context& rc,
	const maplecast_gamestate::GameState& gs,
	const std::vector<TexRef>& texRefs)
{
	char filename[512];
	snprintf(filename, sizeof(filename), "%s/%016llx.bin",
		_cacheDir.c_str(), (unsigned long long)stateHash);

	FILE* f = fopen(filename, "wb");
	if (!f) return false;

	// Header
	uint32_t magic = FILE_MAGIC;
	uint32_t version = FILE_VERSION;
	fwrite(&magic, 4, 1, f);
	fwrite(&version, 4, 1, f);
	fwrite(&stateHash, 8, 1, f);

	// Game state (for verification/lookup)
	fwrite(&gs, sizeof(gs), 1, f);

	// Vertex count + data
	uint32_t vertCount = (uint32_t)rc.verts.size();
	fwrite(&vertCount, 4, 1, f);
	if (vertCount > 0)
		fwrite(rc.verts.data(), sizeof(Vertex), vertCount, f);

	// Index count + data
	uint32_t idxCount = (uint32_t)rc.idx.size();
	fwrite(&idxCount, 4, 1, f);
	if (idxCount > 0)
		fwrite(rc.idx.data(), sizeof(uint32_t), idxCount, f);

	// Opaque polys
	uint32_t opCount = (uint32_t)rc.global_param_op.size();
	fwrite(&opCount, 4, 1, f);
	for (const auto& pp : rc.global_param_op)
	{
		SerializedPoly sp;
		serializePoly(pp, sp);
		fwrite(&sp, sizeof(sp), 1, f);
	}

	// Punch-through polys
	uint32_t ptCount = (uint32_t)rc.global_param_pt.size();
	fwrite(&ptCount, 4, 1, f);
	for (const auto& pp : rc.global_param_pt)
	{
		SerializedPoly sp;
		serializePoly(pp, sp);
		fwrite(&sp, sizeof(sp), 1, f);
	}

	// Translucent polys
	uint32_t trCount = (uint32_t)rc.global_param_tr.size();
	fwrite(&trCount, 4, 1, f);
	for (const auto& pp : rc.global_param_tr)
	{
		SerializedPoly sp;
		serializePoly(pp, sp);
		fwrite(&sp, sizeof(sp), 1, f);
	}

	// Render passes
	uint32_t rpCount = (uint32_t)rc.render_passes.size();
	fwrite(&rpCount, 4, 1, f);
	if (rpCount > 0)
		fwrite(rc.render_passes.data(), sizeof(RenderPass), rpCount, f);

	// Sorted triangles
	uint32_t stCount = (uint32_t)rc.sortedTriangles.size();
	fwrite(&stCount, 4, 1, f);
	if (stCount > 0)
		fwrite(rc.sortedTriangles.data(), sizeof(SortedTriangle), stCount, f);

	// Framebuffer params
	fwrite(&rc.fZ_max, sizeof(float), 1, f);
	fwrite(&rc.fog_clamp_min, sizeof(RGBAColor), 1, f);
	fwrite(&rc.fog_clamp_max, sizeof(RGBAColor), 1, f);

	// Texture references for this frame (Phase 1 — VRAM-rot fix).
	// The pool of decoded RGBA pixels lives in separate tex_*.bin files;
	// this list tells a replay client which entries from the pool to load
	// before running ta_parse on the cached TA buffer. Without this, the
	// replay path has to scan live VRAM (which the running game has
	// already clobbered) to find textures, which is exactly the failure
	// mode that killed the 2026-04 prototype.
	uint32_t texRefCount = (uint32_t)texRefs.size();
	fwrite(&texRefCount, 4, 1, f);
	if (texRefCount > 0)
		fwrite(texRefs.data(), sizeof(TexRef), texRefCount, f);

	uint64_t fileSize = ftell(f);
	fclose(f);

	_totalBytes.fetch_add(fileSize, std::memory_order_relaxed);
	return true;
}

static void writeTransition(uint64_t fromHash, uint64_t toHash)
{
	char filename[512];
	snprintf(filename, sizeof(filename), "%s/transitions.bin", _cacheDir.c_str());

	FILE* f = fopen(filename, "ab");
	if (!f) return;
	fwrite(&fromHash, 8, 1, f);
	fwrite(&toHash, 8, 1, f);
	fclose(f);
	_transitionCount++;
}

bool init(const char* cacheDir)
{
	_cacheDir = cacheDir;
	mkdir(cacheDir, 0755);

	// Load known state hashes from existing cache files
	// (scan directory for .bin files, extract hash from filename)
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "ls %s/*.bin 2>/dev/null | wc -l", cacheDir);
	FILE* p = popen(cmd, "r");
	int existing = 0;
	if (p) { fscanf(p, "%d", &existing); pclose(p); }

	_initialized = true;
	printf("[visual-cache] initialized at %s (%d existing states, file format v%u)\n",
		cacheDir, existing, FILE_VERSION);
	printf("[visual-cache] recording TA data for every new visual state\n");
	printf("[visual-cache] the cache grows as you play — every match fills it\n");
	return true;
}

// Append unique TexRef to the per-frame list. Linear scan is fine — frames
// reference at most a few dozen distinct textures and we want to preserve
// insertion order (the order ta_parse referenced them). Caller owns
// thread safety; this runs only from the render thread.
static void addTexRefIfMissing(uint32_t startAddress, uint32_t tcwFull,
	uint16_t width, uint16_t height)
{
	TexRef ref{startAddress, tcwFull, width, height};
	for (const auto& existing : _frameTextures)
		if (existing == ref) return;
	_frameTextures.push_back(ref);
}

// Walk the rend_context's poly lists and add every referenced texture to
// the per-frame list. captureTexture() catches NEW textures decoded this
// frame; this catches textures already cached in flycast's TexCache from
// prior frames (where Update() fast-pathed without re-decoding).
static void collectPolyTextures(const std::vector<PolyParam>& polys)
{
	for (const PolyParam& pp : polys)
	{
		if (pp.texture == nullptr) continue;
		addTexRefIfMissing(pp.texture->startAddress, pp.tcw.full,
			pp.texture->width, pp.texture->height);
	}
}

void recordFrame(const rend_context& rc)
{
	_totalFrames.fetch_add(1, std::memory_order_relaxed);

	// Skip the work entirely if init() wasn't called. The hook in
	// Renderer_if.cpp fires unconditionally (it's compiled in whenever
	// MAPLECAST_LOOKUP is on), but the caller only enables recording
	// via the MAPLECAST_VISUAL_CACHE env var. Without init() we have no
	// _cacheDir, so writeFrameToDisk would fopen("/<hash>.bin") and fail
	// silently — wasteful, and pollutes _knownStates with hashes that
	// never made it to disk.
	if (!_initialized) {
		_frameTextures.clear();
		return;
	}

	// Read game state from RAM
	maplecast_gamestate::GameState gs;
	maplecast_gamestate::readGameState(gs);

	if (!gs.in_match) {
		// captureTexture may still have appended; clear so the next
		// in-match frame starts with a clean texture set.
		_frameTextures.clear();
		return;
	}

	// Hash the visual state with the CSV-decomposed key (Phase 2).
	uint64_t stateHash = hashFrameStateV2(gs);

	// Check cache
	{
		std::lock_guard<std::mutex> lock(_cacheMutex);
		if (_knownStates.count(stateHash))
		{
			_cacheHits.fetch_add(1, std::memory_order_relaxed);

			// Still record transitions even on cache hit
			if (_prevStateHash != 0 && _prevStateHash != stateHash)
				writeTransition(_prevStateHash, stateHash);
			_prevStateHash = stateHash;
			_frameTextures.clear();
			return;
		}
	}

	// Cache miss — new visual state! Record it.
	_cacheMisses.fetch_add(1, std::memory_order_relaxed);

	// Augment the per-frame texture list with any textures the polys
	// reference but that weren't decoded this frame (cache hits in
	// flycast's TexCache). Without this the cache entry could reference
	// textures whose pool files exist (from a prior frame's decode) but
	// never appear in the cache entry's TexRefs section, so a replay
	// client would skip loading them.
	collectPolyTextures(rc.global_param_op);
	collectPolyTextures(rc.global_param_pt);
	collectPolyTextures(rc.global_param_tr);

	if (writeFrameToDisk(stateHash, rc, gs, _frameTextures))
	{
		std::lock_guard<std::mutex> lock(_cacheMutex);
		_knownStates.insert(stateHash);

		uint64_t total = _knownStates.size();
		if (total % 100 == 0)
		{
			printf("[visual-cache] %lu unique states recorded (%.1f MB on disk, %zu textures this frame)\n",
				total, _totalBytes.load() / (1024.0 * 1024.0), _frameTextures.size());
		}
	}

	// Record transition
	if (_prevStateHash != 0 && _prevStateHash != stateHash)
		writeTransition(_prevStateHash, stateHash);
	_prevStateHash = stateHash;
	_frameTextures.clear();
}

bool hasState(uint64_t stateHash)
{
	std::lock_guard<std::mutex> lock(_cacheMutex);
	return _knownStates.count(stateHash) > 0;
}

CacheStats getStats()
{
	CacheStats s;
	s.totalFrames = _totalFrames.load();
	s.cacheHits = _cacheHits.load();
	s.cacheMisses = _cacheMisses.load();
	s.uniqueStates = _knownStates.size();
	s.totalBytes = _totalBytes.load();
	return s;
}

// Texture capture — called from TexCache.cpp right after decode, before GPU upload
// This is the EXACT same pixel data flycast uploads to OpenGL
static std::unordered_set<uint64_t> _knownTextures;
static std::mutex _texMutex;
static std::atomic<uint32_t> _texCount{0};

void captureTexture(uint32_t startAddress, uint32_t tcwFull,
	uint16_t width, uint16_t height,
	const void* pixels, uint32_t pixelSize, bool is32bit)
{
	// Always record the texture-ref for the current frame, even if we've
	// already written the pool file. The cache entry needs a complete
	// list of textures the frame USES, not just the ones decoded this
	// frame. (The dedup against _knownTextures below is for the disk
	// write only — saves us re-encoding the same RGBA bytes each time
	// flycast reuploads after a palette change.)
	if (_initialized)
		addTexRefIfMissing(startAddress, tcwFull, width, height);

	// Texture ID: combine VRAM address + format info
	uint64_t texId = ((uint64_t)tcwFull << 32) | startAddress;

	{
		std::lock_guard<std::mutex> lock(_texMutex);
		if (_knownTextures.count(texId)) return;  // already captured
	}

	// Write texture to disk
	char filename[512];
	snprintf(filename, sizeof(filename), "%s/tex_%08x_%08x_%dx%d.bin",
		_cacheDir.c_str(), startAddress, tcwFull, width, height);

	FILE* f = fopen(filename, "wb");
	if (!f) return;

	uint32_t magic = 0x54455854;  // "TEXT"
	uint32_t bpp = is32bit ? 4 : 2;
	fwrite(&magic, 4, 1, f);
	fwrite(&startAddress, 4, 1, f);
	fwrite(&tcwFull, 4, 1, f);
	fwrite(&width, 2, 1, f);
	fwrite(&height, 2, 1, f);
	fwrite(&bpp, 4, 1, f);
	fwrite(pixels, pixelSize, 1, f);

	uint64_t fileSize = ftell(f);
	fclose(f);

	{
		std::lock_guard<std::mutex> lock(_texMutex);
		_knownTextures.insert(texId);
	}

	_texCount.fetch_add(1, std::memory_order_relaxed);
	_totalBytes.fetch_add(fileSize, std::memory_order_relaxed);

	if (_texCount.load() % 50 == 0)
	{
		printf("[visual-cache] %u unique textures captured\n", _texCount.load());
	}
}

void shutdown()
{
	auto s = getStats();
	printf("[visual-cache] shutdown — %lu unique states, %lu hits, %lu misses (%.1f%% hit rate)\n",
		s.uniqueStates, s.cacheHits, s.cacheMisses,
		s.totalFrames > 0 ? (100.0 * s.cacheHits / s.totalFrames) : 0.0);
	printf("[visual-cache] %.1f MB on disk, %u transitions, %u textures (file format v%u)\n",
		s.totalBytes / (1024.0 * 1024.0), _transitionCount, _texCount.load(),
		FILE_VERSION);
	_initialized = false;
}

} // namespace maplecast_visual_cache
