/*
	MapleCast Lookup Test

	Records 600 frames of:
	  - 253-byte serialized game state (the "key")
	  - Full TA command buffer (the "value")
	  - Per-character state tuple for fine-grained lookup

	Then replays: reads game state from RAM, hashes it, looks up
	the matching TA commands, feeds them to the renderer.

	This proves: 253 bytes can index a template table → render any frame.
*/
#include "types.h"
#include "maplecast_lookup_test.h"
#include "maplecast_gamestate.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/sh4/sh4_mem.h"
#include "rend/gles/gles.h"
#include "rend/TexCache.h"
#include "rend/texconv.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <unordered_map>

extern Renderer* renderer;
extern bool pal_needs_update;

namespace maplecast_lookup_test
{

static const int RECORD_FRAMES = 600;
static bool _active = false;
static bool _recording = true;
static int _recordCount = 0;
static int _replayPos = 0;
static bool _waitMsgShown = false;

// FNV-1a hash
static uint64_t fnv1a(const void* data, size_t len)
{
	const uint8_t* p = (const uint8_t*)data;
	uint64_t h = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < len; i++) {
		h ^= p[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

// Recorded frame — game state hash maps to TA commands
struct RecordedFrame {
	uint64_t gs_hash;            // hash of 253-byte game state
	std::vector<uint8_t> ta_data; // raw TA command buffer
	uint32_t ta_size;
	// PVR register snapshot for rendering
	uint32_t pvr_snapshot[16];
	// The 253-byte game state for this frame
	uint8_t gs_bytes[256];
	int gs_size;
};

static std::vector<RecordedFrame> _frames;

// Lookup table: gs_hash → index into _frames
static std::unordered_map<uint64_t, int> _lookupTable;

// Stats
static int _lookupHits = 0;
static int _lookupMisses = 0;

void init()
{
	_active = true;
	_recording = true;
	_recordCount = 0;
	_replayPos = 0;
	_waitMsgShown = false;
	_lookupHits = 0;
	_lookupMisses = 0;
	_frames.clear();
	_frames.reserve(RECORD_FRAMES);
	_lookupTable.clear();

	printf("[LOOKUP] === INITIALIZED ===\n");
	printf("[LOOKUP] Phase 1: Recording %d frames of game state + TA commands\n", RECORD_FRAMES);
	printf("[LOOKUP] Phase 2: Replay using ONLY game state → lookup TA from table\n");
	printf("[LOOKUP] Get into a match and fight!\n");
}

bool active() { return _active; }
bool isReplaying() { return _active && !_recording; }

// Phase 1: called from server render path to record state + TA
void serverRecord(TA_context* ctx)
{
	if (!_active || !_recording || !ctx) return;
	if (ctx->rend.isRTT) return;

	// Wait for match
	uint8_t in_match = (uint8_t)addrspace::read8(0x8C289624);
	if (!in_match) {
		if (!_waitMsgShown) { printf("[LOOKUP] Waiting for match...\n"); _waitMsgShown = true; }
		return;
	}

	RecordedFrame frame;

	// Read game state
	maplecast_gamestate::GameState gs;
	maplecast_gamestate::readGameState(gs);
	frame.gs_size = maplecast_gamestate::serialize(gs, frame.gs_bytes, sizeof(frame.gs_bytes));

	// Hash ONLY visual identity — NOT positions, timers, counters
	// What determines the SHAPE of the frame (which sprites to draw):
	uint8_t visualKey[64];
	int vk = 0;
	visualKey[vk++] = gs.stage_id;
	visualKey[vk++] = gs.in_match;
	for (int i = 0; i < 6; i++)
	{
		visualKey[vk++] = gs.chars[i].active;
		visualKey[vk++] = gs.chars[i].character_id;
		visualKey[vk++] = gs.chars[i].facing_right;
		visualKey[vk++] = gs.chars[i].palette_id;
		// animation_state + anim_timer = which sprite frame
		memcpy(visualKey + vk, &gs.chars[i].animation_state, 2); vk += 2;
		memcpy(visualKey + vk, &gs.chars[i].anim_timer, 2); vk += 2;
		memcpy(visualKey + vk, &gs.chars[i].sprite_id, 2); vk += 2;
	}
	frame.gs_hash = fnv1a(visualKey, vk);

	// Capture TA commands
	frame.ta_size = (uint32_t)(ctx->tad.thd_data - ctx->tad.thd_root);
	frame.ta_data.resize(frame.ta_size);
	memcpy(frame.ta_data.data(), ctx->tad.thd_root, frame.ta_size);

	// Capture PVR registers
	frame.pvr_snapshot[0] = TA_GLOB_TILE_CLIP.full;
	frame.pvr_snapshot[1] = SCALER_CTL.full;
	frame.pvr_snapshot[2] = FB_X_CLIP.full;
	frame.pvr_snapshot[3] = FB_Y_CLIP.full;
	frame.pvr_snapshot[4] = FB_W_LINESTRIDE.full;
	frame.pvr_snapshot[5] = FB_W_SOF1;
	frame.pvr_snapshot[6] = FB_W_CTRL.full;
	frame.pvr_snapshot[7] = FOG_CLAMP_MIN.full;
	frame.pvr_snapshot[8] = FOG_CLAMP_MAX.full;
	frame.pvr_snapshot[9] = ctx->rend.framebufferWidth;
	frame.pvr_snapshot[10] = ctx->rend.framebufferHeight;
	frame.pvr_snapshot[11] = ctx->rend.clearFramebuffer ? 1 : 0;
	float fz = ctx->rend.fZ_max;
	memcpy(&frame.pvr_snapshot[12], &fz, 4);

	// Store in lookup table
	int idx = (int)_frames.size();
	_lookupTable[frame.gs_hash] = idx;
	_frames.push_back(std::move(frame));
	_recordCount++;

	if (_recordCount % 60 == 0)
		printf("[LOOKUP] Recording: %d/%d | %zu unique VISUAL states (%.0f%% unique)\n",
			_recordCount, RECORD_FRAMES, _lookupTable.size(),
			_lookupTable.size() * 100.0 / _recordCount);

	if (_recordCount >= RECORD_FRAMES)
	{
		_recording = false;
		printf("[LOOKUP] === RECORDING COMPLETE ===\n");
		printf("[LOOKUP] %d frames recorded | %zu unique game states\n",
			_recordCount, _lookupTable.size());
		printf("[LOOKUP] === REPLAY STARTING — rendering from lookup table ===\n");
		printf("[LOOKUP] The game keeps running. We render from recorded TA templates.\n");

		size_t totalTA = 0;
		for (const auto& f : _frames) totalTA += f.ta_size;
		printf("[LOOKUP] Total TA data: %.1f MB | Avg: %.1f KB/frame\n",
			totalTA / (1024.0*1024.0), totalTA / (1024.0 * _recordCount));
	}
}

// Phase 2: called from render path during replay
// Reads CURRENT game state, looks up matching TA commands from table
bool clientLookup(rend_context& rc)
{
	if (!_active || _recording) return false;

	// Read current game state from RAM
	maplecast_gamestate::GameState gs;
	maplecast_gamestate::readGameState(gs);

	// Hash ONLY visual identity — same key as recording
	uint8_t visualKey[64];
	int vk = 0;
	visualKey[vk++] = gs.stage_id;
	visualKey[vk++] = gs.in_match;
	for (int i = 0; i < 6; i++)
	{
		visualKey[vk++] = gs.chars[i].active;
		visualKey[vk++] = gs.chars[i].character_id;
		visualKey[vk++] = gs.chars[i].facing_right;
		visualKey[vk++] = gs.chars[i].palette_id;
		memcpy(visualKey + vk, &gs.chars[i].animation_state, 2); vk += 2;
		memcpy(visualKey + vk, &gs.chars[i].anim_timer, 2); vk += 2;
		memcpy(visualKey + vk, &gs.chars[i].sprite_id, 2); vk += 2;
	}
	uint64_t gsHash = fnv1a(visualKey, vk);

	// Try exact lookup
	auto it = _lookupTable.find(gsHash);
	RecordedFrame* frame = nullptr;

	if (it != _lookupTable.end())
	{
		frame = &_frames[it->second];
		_lookupHits++;
	}
	else
	{
		// No exact match — use sequential playback as fallback
		frame = &_frames[_replayPos % _recordCount];
		_replayPos++;
		_lookupMisses++;
	}

	if (!frame || frame->ta_size == 0) return false;

	// Build TA context from looked-up template
	static TA_context lookupCtx;
	static bool ctxAlloced = false;
	if (!ctxAlloced) { lookupCtx.Alloc(); ctxAlloced = true; }

	lookupCtx.rend.Clear();
	lookupCtx.tad.Clear();

	// Copy TA commands
	memcpy(lookupCtx.tad.thd_root, frame->ta_data.data(), frame->ta_size);
	lookupCtx.tad.thd_data = lookupCtx.tad.thd_root + frame->ta_size;

	// Set PVR registers
	TA_GLOB_TILE_CLIP.full = frame->pvr_snapshot[0];
	SCALER_CTL.full = frame->pvr_snapshot[1];
	FB_X_CLIP.full = frame->pvr_snapshot[2];
	FB_Y_CLIP.full = frame->pvr_snapshot[3];
	FB_W_LINESTRIDE.full = frame->pvr_snapshot[4];
	FB_W_SOF1 = frame->pvr_snapshot[5];
	FB_W_CTRL.full = frame->pvr_snapshot[6];
	FOG_CLAMP_MIN.full = frame->pvr_snapshot[7];
	FOG_CLAMP_MAX.full = frame->pvr_snapshot[8];

	lookupCtx.rend.isRTT = false;
	lookupCtx.rend.fb_W_SOF1 = frame->pvr_snapshot[5];
	lookupCtx.rend.fb_W_CTRL.full = frame->pvr_snapshot[6];
	lookupCtx.rend.ta_GLOB_TILE_CLIP.full = frame->pvr_snapshot[0];
	lookupCtx.rend.scaler_ctl.full = frame->pvr_snapshot[1];
	lookupCtx.rend.fb_X_CLIP.full = frame->pvr_snapshot[2];
	lookupCtx.rend.fb_Y_CLIP.full = frame->pvr_snapshot[3];
	lookupCtx.rend.fb_W_LINESTRIDE = frame->pvr_snapshot[4];
	lookupCtx.rend.fog_clamp_min.full = frame->pvr_snapshot[7];
	lookupCtx.rend.fog_clamp_max.full = frame->pvr_snapshot[8];
	lookupCtx.rend.framebufferWidth = frame->pvr_snapshot[9];
	lookupCtx.rend.framebufferHeight = frame->pvr_snapshot[10];
	lookupCtx.rend.clearFramebuffer = frame->pvr_snapshot[11] != 0;
	float fz; memcpy(&fz, &frame->pvr_snapshot[12], 4);
	lookupCtx.rend.fZ_max = fz;

	// Palette update
	pal_needs_update = true;
	palette_update();
	renderer->updatePalette = true;
	renderer->updateFogTable = true;

	// Run ta_parse on looked-up TA commands
	renderer->Process(&lookupCtx);
	rc = lookupCtx.rend;

	// Log stats
	static int totalFrames = 0;
	totalFrames++;
	if (totalFrames % 60 == 0)
	{
		float hitRate = _lookupHits * 100.0f / (_lookupHits + _lookupMisses);
		printf("[LOOKUP] Replay: %d frames | hits=%d misses=%d (%.1f%% hit rate)\n",
			totalFrames, _lookupHits, _lookupMisses, hitRate);
	}

	return true;
}

}  // namespace maplecast_lookup_test
