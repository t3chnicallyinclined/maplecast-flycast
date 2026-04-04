/*
	MapleCast Rend Replay v2 — record rend_context frames to disk.

	Dumps 600 frames of geometry + texture references to a binary file.
	A standalone WebGL/WebGPU renderer can load this file + the texture atlas
	from visual_cache/ to replay the match without any emulator.

	File format (rend_recording.bin):
	  Header: "MREC" (4) + version (u32) + frame_count (u32) + fps (u32)
	  Per frame:
	    frame_size (u32) — total bytes for this frame (for seeking)
	    num_verts (u32) + verts (num_verts × 20 bytes: x,y,z,u,v)
	    num_colors (u32) + colors (num_verts × 8 bytes: rgba + spec_rgba)
	    num_indices (u32) + indices (num_indices × u32)
	    num_polys_op (u32) + polys_op (each: first(u32) count(u32) tsp(u32) tcw(u32) isp(u32) tileclip(u32))
	    num_polys_pt (u32) + polys_pt (same format)
	    num_polys_tr (u32) + polys_tr (same format)
	    hw_regs: fZ_max(f32) fb_width(u32) fb_height(u32) clearFB(u8)

	Also dumps unique textures as PNG files in rend_textures/ for the WebGL client.

	MAPLECAST_REND_REPLAY=1 to enable.
*/
#include "types.h"
#include "maplecast_rend_replay.h"
#include "maplecast_gamestate.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/sh4/sh4_mem.h"
#include "rend/TexCache.h"
#include "rend/gles/gles.h"
#include <GL/gl.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <vector>
#include <unordered_set>
#include <sys/stat.h>

extern Renderer* renderer;

namespace maplecast_rend_replay
{

static const int RECORD_FRAMES = 600;  // 10 seconds at 60fps

static std::atomic<bool> _active{false};
static bool _recording = true;
static int _recordCount = 0;
static bool _waitMsgShown = false;
static FILE* _outFile = nullptr;
static long _headerPos = 0;

// Track unique textures we've dumped
static std::unordered_set<uint64_t> _dumpedTextures;
static uint32_t _texCount = 0;

// Poly params serialized format: 24 bytes each
struct SerializedPoly {
	u32 first;
	u32 count;
	u32 tsp_full;   // TSP word — texture/blend params
	u32 tcw_full;   // TCW word — texture address/format (= texture ID for lookup)
	u32 isp_full;   // ISP word — Z test, culling
	u32 pcw_full;   // PCW word — list type, texture flag, etc.
};

static void writeU32(FILE* f, u32 v) { fwrite(&v, 4, 1, f); }
static void writeF32(FILE* f, float v) { fwrite(&v, 4, 1, f); }
static void writeU8(FILE* f, u8 v) { fwrite(&v, 1, 1, f); }

static void dumpTexture(const PolyParam& pp)
{
	if (!pp.pcw.Texture || !pp.texture) return;

	// Key by TCW only (same texture identity regardless of TSP filtering)
	uint64_t key = pp.tcw.full;
	if (_dumpedTextures.count(key)) return;
	_dumpedTextures.insert(key);

	TextureCacheData* tex = (TextureCacheData*)pp.texture;
	if (tex->texID == 0 || tex->width == 0 || tex->height == 0) return;

	// Read RGBA pixels directly from the GPU texture
	int w = tex->width;
	int h = tex->height;
	std::vector<uint8_t> pixels(w * h * 4);

	glBindTexture(GL_TEXTURE_2D, tex->texID);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

	// Write as raw RGBA with 4-byte header (width u16, height u16)
	char fname[256];
	snprintf(fname, sizeof(fname), "rend_textures/tex_%08x.rgba", pp.tcw.full);
	FILE* f = fopen(fname, "wb");
	if (f)
	{
		uint16_t ww = (uint16_t)w, hh = (uint16_t)h;
		fwrite(&ww, 2, 1, f);
		fwrite(&hh, 2, 1, f);
		fwrite(pixels.data(), 1, w * h * 4, f);
		fclose(f);
	}

	_texCount++;
}

static void writePolyList(FILE* f, const std::vector<PolyParam>& params)
{
	u32 count = (u32)params.size();
	writeU32(f, count);
	for (const auto& pp : params)
	{
		SerializedPoly sp;
		sp.first = pp.first;
		sp.count = pp.count;
		sp.tsp_full = pp.tsp.full;
		sp.tcw_full = pp.tcw.full;
		sp.isp_full = pp.isp.full;
		sp.pcw_full = pp.pcw.full;
		fwrite(&sp, sizeof(sp), 1, f);

		// Dump texture if we haven't seen it
		dumpTexture(pp);
	}
}

void init()
{
	_active = true;
	_recording = true;
	_recordCount = 0;
	_waitMsgShown = false;
	_dumpedTextures.clear();
	_texCount = 0;

	mkdir("rend_textures", 0755);

	_outFile = fopen("rend_recording.bin", "wb");
	if (!_outFile)
	{
		printf("[REND_REPLAY] ERROR: cannot create rend_recording.bin\n");
		_active = false;
		return;
	}

	// Write header (frame_count will be patched later)
	fwrite("MREC", 4, 1, _outFile);
	writeU32(_outFile, 1);            // version
	_headerPos = ftell(_outFile);
	writeU32(_outFile, 0);            // frame_count (placeholder)
	writeU32(_outFile, 60);           // fps

	printf("[REND_REPLAY] === INITIALIZED ===\n");
	printf("[REND_REPLAY] Recording %d frames to rend_recording.bin\n", RECORD_FRAMES);
	printf("[REND_REPLAY] Textures saved to rend_textures/\n");
	printf("[REND_REPLAY] Get into a match and fight!\n");
}

bool active()
{
	return _active;
}

bool replaying()
{
	return false;  // v2: disk recording only, no live replay
}

bool tick(rend_context& rc)
{
	if (!_active || !_recording || !_outFile) return false;
	if (rc.isRTT) return false;

	// Wait for match
	uint8_t in_match = (uint8_t)addrspace::read8(0x8C289624);
	if (!in_match)
	{
		if (!_waitMsgShown)
		{
			printf("[REND_REPLAY] Waiting for match to start...\n");
			_waitMsgShown = true;
		}
		return false;
	}

	// Record frame start position for size prefix
	long frameStart = ftell(_outFile);
	writeU32(_outFile, 0);  // placeholder for frame_size

	// Vertices: x, y, z, u, v (20 bytes each)
	u32 numVerts = (u32)rc.verts.size();
	writeU32(_outFile, numVerts);
	for (const auto& v : rc.verts)
	{
		writeF32(_outFile, v.x);
		writeF32(_outFile, v.y);
		writeF32(_outFile, v.z);
		writeF32(_outFile, v.u);
		writeF32(_outFile, v.v);
	}

	// Colors: col[4] + spc[4] = 8 bytes each
	writeU32(_outFile, numVerts);
	for (const auto& v : rc.verts)
	{
		fwrite(v.col, 4, 1, _outFile);
		fwrite(v.spc, 4, 1, _outFile);
	}

	// Indices
	u32 numIdx = (u32)rc.idx.size();
	writeU32(_outFile, numIdx);
	fwrite(rc.idx.data(), 4, numIdx, _outFile);

	// Polygon lists
	writePolyList(_outFile, rc.global_param_op);
	writePolyList(_outFile, rc.global_param_pt);
	writePolyList(_outFile, rc.global_param_tr);

	// Hardware registers
	writeF32(_outFile, rc.fZ_max);
	writeU32(_outFile, rc.framebufferWidth);
	writeU32(_outFile, rc.framebufferHeight);
	writeU8(_outFile, rc.clearFramebuffer ? 1 : 0);

	// Patch frame size
	long frameEnd = ftell(_outFile);
	u32 frameSize = (u32)(frameEnd - frameStart - 4);
	fseek(_outFile, frameStart, SEEK_SET);
	writeU32(_outFile, frameSize);
	fseek(_outFile, frameEnd, SEEK_SET);

	_recordCount++;

	if (_recordCount % 60 == 0)
		printf("[REND_REPLAY] Recording: %d/%d frames (%d%%) | %u unique textures\n",
			_recordCount, RECORD_FRAMES, 100 * _recordCount / RECORD_FRAMES, _texCount);

	if (_recordCount >= RECORD_FRAMES)
	{
		_recording = false;

		// Patch frame count in header
		fseek(_outFile, _headerPos, SEEK_SET);
		writeU32(_outFile, (u32)_recordCount);
		fflush(_outFile);
		fclose(_outFile);
		_outFile = nullptr;

		long fileSize = frameEnd;

		// Write texture manifest for WebGL loader
		FILE* mf = fopen("rend_textures/manifest.json", "w");
		if (mf)
		{
			fprintf(mf, "{\n");
			bool first = true;
			for (uint64_t tcw : _dumpedTextures)
			{
				char fname[64];
				snprintf(fname, sizeof(fname), "tex_%08x.rgba", (uint32_t)tcw);
				if (!first) fprintf(mf, ",\n");
				fprintf(mf, "  \"%u\": {\"file\": \"%s\"}", (uint32_t)tcw, fname);
				first = false;
			}
			fprintf(mf, "\n}\n");
			fclose(mf);
		}

		printf("[REND_REPLAY] === RECORDING COMPLETE ===\n");
		printf("[REND_REPLAY] %d frames saved to rend_recording.bin (%.1f MB)\n",
			_recordCount, fileSize / (1024.0 * 1024.0));
		printf("[REND_REPLAY] %u unique textures saved to rend_textures/\n", _texCount);
		printf("[REND_REPLAY] Ready for standalone WebGL playback!\n");
	}

	return false;  // don't interfere with live rendering
}

}  // namespace maplecast_rend_replay
