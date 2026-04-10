/*
	MapleCast Client-Side Palette Override — implementation.

	Writes to the client's local PALETTE_RAM in pvr_regs and sets
	pal_needs_update so the renderer rebuilds palette32_ram (the
	RGBA32 palette texture) from our overridden values. This is the
	same path the game uses — we just substitute different colors.
*/
#include "maplecast_palette.h"
#include "hw/pvr/pvr_regs.h"

#include <mutex>
#include <vector>
#include <cstring>

extern bool pal_needs_update;

namespace maplecast_palette
{

struct Override {
	int startIndex;
	std::vector<uint16_t> colors;
};

static std::mutex _mutex;
static std::vector<Override> _overrides;

void applyClientOverrides()
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (_overrides.empty()) return;

	bool changed = false;
	for (auto& ov : _overrides) {
		for (int i = 0; i < (int)ov.colors.size(); i++) {
			int idx = ov.startIndex + i;
			if (idx < 0 || idx >= 1024) continue;
			u32 addr = PALETTE_RAM_START_addr + idx * 4;
			u32 newVal = ov.colors[i];
			if (PvrReg(addr, u32) != newVal) {
				PvrReg(addr, u32) = newVal;
				changed = true;
			}
		}
	}
	// Tell the renderer to rebuild the palette texture from our
	// overridden PALETTE_RAM values.
	if (changed)
		pal_needs_update = true;
}

void setOverride(int index, const uint16_t* colors, int count)
{
	std::lock_guard<std::mutex> lock(_mutex);
	Override ov;
	ov.startIndex = index;
	ov.colors.assign(colors, colors + count);
	_overrides.push_back(std::move(ov));
}

void clearOverrides()
{
	std::lock_guard<std::mutex> lock(_mutex);
	_overrides.clear();
}

bool hasOverrides()
{
	std::lock_guard<std::mutex> lock(_mutex);
	return !_overrides.empty();
}

}
