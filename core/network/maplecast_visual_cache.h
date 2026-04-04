/*
	MapleCast Visual Cache — self-building game state → TA mapping.

	Every frame: hash(game state) → check cache → if miss, store TA data.
	Over time, builds a complete map of every visual state MVC2 can produce.

	Phase 1: Simple hashmap on disk (flat files)
	Phase 2: Graph database with transitions + cancel edges
	Phase 3: Shared across community nodes
*/
#pragma once
#include <cstdint>

struct rend_context;

namespace maplecast_visual_cache
{

struct CacheStats {
	uint64_t totalFrames;
	uint64_t cacheHits;
	uint64_t cacheMisses;
	uint64_t uniqueStates;
	uint64_t totalBytes;     // disk usage
};

// Initialize — set cache directory path
bool init(const char* cacheDir = "visual_cache");

// Record current frame: game state + TA display list
// Called every frame from onFrameRendered
void recordFrame(const rend_context& rc);

// Lookup: given game state hash, do we have TA data?
bool hasState(uint64_t stateHash);

// Get stats
CacheStats getStats();

// Capture decoded texture pixels (called from TexCache.cpp after decode)
void captureTexture(uint32_t startAddress, uint32_t tcwFull,
	uint16_t width, uint16_t height,
	const void* pixels, uint32_t pixelSize, bool is32bit);

// Shutdown
void shutdown();

}
