/*
	MapleCast Client-Side Palette Override — implementation.
*/
#include "maplecast_palette.h"
#include "hw/pvr/pvr_regs.h"

#include <mutex>
#include <vector>
#include <cstring>

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

	// Write directly to pvr_regs palette RAM memory.
	// This is the client's LOCAL copy — not the server's.
	// PALETTE_RAM is at pvr_regs offset 0x1000, each entry is 4 bytes.
	for (auto& ov : _overrides) {
		for (int i = 0; i < (int)ov.colors.size(); i++) {
			int idx = ov.startIndex + i;
			if (idx < 0 || idx >= 1024) continue;
			u32 addr = PALETTE_RAM_START_addr + idx * 4;
			PvrReg(addr, u32) = ov.colors[i];
		}
	}
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
