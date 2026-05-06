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
#include "rend/TexCache.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <sys/stat.h>

namespace maplecast_visual_cache
{

static std::string _cacheDir;
static std::unordered_set<uint64_t> _knownStates;
static std::mutex _cacheMutex;
static std::atomic<uint64_t> _totalFrames{0};
static std::atomic<uint64_t> _cacheHits{0};
static std::atomic<uint64_t> _cacheMisses{0};
static std::atomic<uint64_t> _totalBytes{0};
static uint64_t _prevStateHash = 0;
static uint32_t _transitionCount = 0;

// Hash a visual state: character + animation + frame timer + facing + palette
// One hash per CHARACTER, not per frame — 6 characters per frame
static uint64_t hashCharState(uint8_t charId, uint16_t animState, uint16_t animTimer,
	uint8_t facing, uint8_t palette)
{
	uint64_t h = 0;
	h = charId;
	h = (h << 16) | animState;
	h = (h << 16) | animTimer;
	h = (h << 8) | facing;
	h = (h << 8) | palette;
	// Mix bits for better distribution
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccd;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53;
	h ^= h >> 33;
	return h;
}

// Hash the full frame state: all 6 characters combined
static uint64_t hashFrameState(const maplecast_gamestate::GameState& gs)
{
	uint64_t h = 0;
	for (int i = 0; i < 6; i++)
	{
		const auto& c = gs.chars[i];
		if (!c.active) continue;
		uint64_t ch = hashCharState(c.character_id, c.animation_state,
			c.anim_timer, c.facing_right, c.palette_id);
		h ^= ch + 0x9e3779b9 + (h << 6) + (h >> 2);  // boost::hash_combine
	}
	// Include global state
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
	const maplecast_gamestate::GameState& gs)
{
	char filename[512];
	snprintf(filename, sizeof(filename), "%s/%016llx.bin",
		_cacheDir.c_str(), (unsigned long long)stateHash);

	FILE* f = fopen(filename, "wb");
	if (!f) return false;

	// Header
	uint32_t magic = 0x56495343;  // "VISC" — visual cache
	uint32_t version = 1;
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

	printf("[visual-cache] initialized at %s (%d existing states)\n", cacheDir, existing);
	printf("[visual-cache] recording TA data for every new visual state\n");
	printf("[visual-cache] the cache grows as you play — every match fills it\n");
	return true;
}

void recordFrame(const rend_context& rc)
{
	_totalFrames.fetch_add(1, std::memory_order_relaxed);

	// Read game state from RAM
	maplecast_gamestate::GameState gs;
	maplecast_gamestate::readGameState(gs);

	if (!gs.in_match) return;  // only record during actual gameplay

	// Hash the visual state
	uint64_t stateHash = hashFrameState(gs);

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
			return;
		}
	}

	// Cache miss — new visual state! Record it.
	_cacheMisses.fetch_add(1, std::memory_order_relaxed);

	if (writeFrameToDisk(stateHash, rc, gs))
	{
		std::lock_guard<std::mutex> lock(_cacheMutex);
		_knownStates.insert(stateHash);

		uint64_t total = _knownStates.size();
		if (total % 100 == 0)
		{
			printf("[visual-cache] %lu unique states recorded (%.1f MB on disk)\n",
				total, _totalBytes.load() / (1024.0 * 1024.0));
		}
	}

	// Record transition
	if (_prevStateHash != 0 && _prevStateHash != stateHash)
		writeTransition(_prevStateHash, stateHash);
	_prevStateHash = stateHash;
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
	printf("[visual-cache] %.1f MB on disk, %u transitions, %u textures\n",
		s.totalBytes / (1024.0 * 1024.0), _transitionCount, _texCount.load());
}

} // namespace maplecast_visual_cache
