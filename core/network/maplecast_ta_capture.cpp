/*
	MapleCast TA Capture — match TA polygons to characters by screen position,
	extract UV coordinates for pixel-perfect sprite rendering.
*/
#include "maplecast_ta_capture.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/Renderer_if.h"
#include "rend/TexCache.h"

#include <cmath>
#include <cstdio>
#include <mutex>

namespace maplecast_ta_capture
{

static rend_context* _lastRendContext = nullptr;
static std::mutex _rendMutex;
static SpriteFrame _frames[6] = {};

void setRendContext(void* ctx) {
	std::lock_guard<std::mutex> lock(_rendMutex);
	_lastRendContext = (rend_context*)ctx;
}

void captureFrame(const float screen_x[6], const float screen_y[6], const bool active[6])
{
	// Only capture every 3rd frame to reduce overhead
	static int _skipCount = 0;
	if (++_skipCount % 3 != 0) return;

	// Reset
	for (int i = 0; i < 6; i++)
		_frames[i].found = false;

	std::lock_guard<std::mutex> lock(_rendMutex);
	if (!_lastRendContext) return;
	rend_context& rc = *_lastRendContext;

	// Search all polygon lists (opaque + punch-through + translucent)
	const std::vector<PolyParam>* lists[3] = {
		&rc.global_param_op,
		&rc.global_param_pt,
		&rc.global_param_tr,
	};

	for (int charIdx = 0; charIdx < 6; charIdx++)
	{
		if (!active[charIdx]) continue;
		if (_frames[charIdx].found) continue;

		float cx = screen_x[charIdx];
		float cy = screen_y[charIdx];

		float bestDist = 999999.f;
		const PolyParam* bestPoly = nullptr;

		for (int li = 0; li < 3; li++)
		{
			const auto& list = *lists[li];
			for (const auto& pp : list)
			{
				if (!pp.pcw.Texture || !pp.texture) continue;
				if (pp.count < 3) continue;

				// Get the center of this polygon's screen-space vertices
				float avgX = 0, avgY = 0;
				int nverts = 0;
				for (u32 j = pp.first; j < pp.first + pp.count && j < rc.idx.size(); j++)
				{
					u32 vi = rc.idx[j];
					if (vi < rc.verts.size())
					{
						avgX += rc.verts[vi].x;
						avgY += rc.verts[vi].y;
						nverts++;
					}
				}
				if (nverts == 0) continue;
				avgX /= nverts;
				avgY /= nverts;

				float dist = std::abs(avgX - cx) + std::abs(avgY - cy);
				if (dist < bestDist && dist < 100.f)  // within 100px
				{
					bestDist = dist;
					bestPoly = &pp;
				}
			}
		}

		if (bestPoly)
		{
			// Extract UV bounding box from this polygon's vertices
			float uMin = 1e9f, vMin = 1e9f, uMax = -1e9f, vMax = -1e9f;
			for (u32 j = bestPoly->first; j < bestPoly->first + bestPoly->count && j < rc.idx.size(); j++)
			{
				u32 vi = rc.idx[j];
				if (vi < rc.verts.size())
				{
					const Vertex& v = rc.verts[vi];
					if (v.u < uMin) uMin = v.u;
					if (v.v < vMin) vMin = v.v;
					if (v.u > uMax) uMax = v.u;
					if (v.v > vMax) vMax = v.v;
				}
			}

			_frames[charIdx].u0 = uMin;
			_frames[charIdx].v0 = vMin;
			_frames[charIdx].u1 = uMax;
			_frames[charIdx].v1 = vMax;
			_frames[charIdx].tex_width = bestPoly->texture->width;
			_frames[charIdx].tex_height = bestPoly->texture->height;
			_frames[charIdx].tex_addr = bestPoly->texture->startAddress;
			_frames[charIdx].found = true;
		}
	}
}

const SpriteFrame& getFrame(int slot)
{
	return _frames[slot < 6 ? slot : 0];
}

}  // namespace maplecast_ta_capture
