/*
	MapleCast Client Mode — renderer-only playback.

	Loads rend_recording.bin (geometry) + rend_textures/*.rgba (sprites)
	and plays them through flycast's real OpenGL renderer.

	No SH4 CPU. No ROM. No VRAM. Just the renderer drawing recorded frames
	with pre-loaded textures. Pixel perfect because it IS flycast's renderer.
*/
#include "types.h"
#include "maplecast_client.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/Renderer_if.h"
#include "rend/TexCache.h"
#include "rend/gles/gles.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <unordered_map>
#include <dirent.h>
#include <sys/stat.h>

extern Renderer* renderer;

namespace maplecast_client
{

static bool _active = false;

// Pre-parsed frame data
struct RecordedFrame {
	std::vector<Vertex> verts;
	std::vector<u32> idx;

	struct SerializedPoly {
		u32 first, count;
		u32 tsp_full, tcw_full, isp_full, pcw_full;
	};
	std::vector<SerializedPoly> polys_op;
	std::vector<SerializedPoly> polys_pt;
	std::vector<SerializedPoly> polys_tr;

	f32 fZ_max;
	u32 fbW, fbH;
	bool clearFB;
};

static std::vector<RecordedFrame> _frames;
static int _currentFrame = 0;
static int _frameCount = 0;

// Pre-loaded textures: TCW -> TextureCacheData with valid GPU texID
static std::unordered_map<u32, GLuint> _textures;  // TCW -> GL texture ID

// TA context for feeding the renderer
static TA_context _clientCtx;

// ==================== TEXTURE LOADING ====================

static bool loadTextures(const char* dir)
{
	DIR* d = opendir(dir);
	if (!d) {
		printf("[CLIENT] Cannot open texture directory: %s\n", dir);
		return false;
	}

	int loaded = 0;
	struct dirent* ent;
	while ((ent = readdir(d)) != nullptr)
	{
		std::string name = ent->d_name;
		if (name.size() < 5 || name.substr(name.size()-5) != ".rgba")
			continue;

		// Parse TCW from filename: tex_XXXXXXXX.rgba
		if (name.substr(0, 4) != "tex_") continue;
		u32 tcw = (u32)strtoul(name.substr(4, 8).c_str(), nullptr, 16);

		std::string path = std::string(dir) + "/" + name;
		FILE* f = fopen(path.c_str(), "rb");
		if (!f) continue;

		u16 w, h;
		fread(&w, 2, 1, f);
		fread(&h, 2, 1, f);

		std::vector<u8> rgba(w * h * 4);
		size_t read = fread(rgba.data(), 1, rgba.size(), f);
		fclose(f);
		if (read != rgba.size()) continue;

		// Upload to OpenGL
		GLuint texID;
		glGenTextures(1, &texID);
		glBindTexture(GL_TEXTURE_2D, texID);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

		_textures[tcw] = texID;
		loaded++;
	}
	closedir(d);

	printf("[CLIENT] Loaded %d textures from %s\n", loaded, dir);
	return loaded > 0;
}

// ==================== RECORDING LOADING ====================

static bool loadRecording(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f) {
		printf("[CLIENT] Cannot open recording: %s\n", path);
		return false;
	}

	char magic[4];
	fread(magic, 4, 1, f);
	if (memcmp(magic, "MREC", 4) != 0) {
		printf("[CLIENT] Invalid recording file\n");
		fclose(f);
		return false;
	}

	u32 version, frameCount, fps;
	fread(&version, 4, 1, f);
	fread(&frameCount, 4, 1, f);
	fread(&fps, 4, 1, f);

	printf("[CLIENT] Recording: %u frames at %u fps, version %u\n", frameCount, fps, version);

	_frames.resize(frameCount);
	for (u32 fr = 0; fr < frameCount; fr++)
	{
		u32 frameSize;
		fread(&frameSize, 4, 1, f);
		long frameStart = ftell(f);

		RecordedFrame& rf = _frames[fr];

		// Vertices: x,y,z,u,v (20 bytes each)
		u32 numVerts;
		fread(&numVerts, 4, 1, f);
		rf.verts.resize(numVerts);
		for (u32 i = 0; i < numVerts; i++)
		{
			float vals[5];
			fread(vals, 4, 5, f);
			rf.verts[i].x = vals[0];
			rf.verts[i].y = vals[1];
			rf.verts[i].z = vals[2];
			rf.verts[i].u = vals[3];
			rf.verts[i].v = vals[4];
			// Clear other fields
			memset(rf.verts[i].col, 0, 4);
			memset(rf.verts[i].spc, 0, 4);
			rf.verts[i].u1 = rf.verts[i].v1 = 0;
			memset(rf.verts[i].col1, 0, 4);
			memset(rf.verts[i].spc1, 0, 4);
			rf.verts[i].nx = rf.verts[i].ny = rf.verts[i].nz = 0;
		}

		// Colors: col[4] + spc[4] = 8 bytes each
		u32 numColors;
		fread(&numColors, 4, 1, f);
		for (u32 i = 0; i < numColors && i < numVerts; i++)
		{
			fread(rf.verts[i].col, 1, 4, f);
			fread(rf.verts[i].spc, 1, 4, f);
		}
		// Skip excess if any
		if (numColors > numVerts)
			fseek(f, (numColors - numVerts) * 8, SEEK_CUR);

		// Indices
		u32 numIdx;
		fread(&numIdx, 4, 1, f);
		rf.idx.resize(numIdx);
		fread(rf.idx.data(), 4, numIdx, f);

		// Poly params (24 bytes each)
		auto readPolys = [&](std::vector<RecordedFrame::SerializedPoly>& polys) {
			u32 count;
			fread(&count, 4, 1, f);
			polys.resize(count);
			for (u32 i = 0; i < count; i++)
				fread(&polys[i], 24, 1, f);
		};
		readPolys(rf.polys_op);
		readPolys(rf.polys_pt);
		readPolys(rf.polys_tr);

		// HW regs
		fread(&rf.fZ_max, 4, 1, f);
		fread(&rf.fbW, 4, 1, f);
		fread(&rf.fbH, 4, 1, f);
		u8 clearFB;
		fread(&clearFB, 1, 1, f);
		rf.clearFB = clearFB != 0;

		fseek(f, frameStart + frameSize, SEEK_SET);
	}

	fclose(f);
	_frameCount = frameCount;
	printf("[CLIENT] Loaded %d frames\n", _frameCount);
	return true;
}

// ==================== BUILD REND_CONTEXT FROM RECORDED FRAME ====================

// Dummy texture cache entries for mapping TCW -> GL texture
// We create lightweight TextureCacheData objects that just hold the texID
struct DummyTexture : public BaseTextureCacheData {
	GLuint glTexID;
	DummyTexture(TSP tsp, TCW tcw, GLuint id, u16 w, u16 h)
		: BaseTextureCacheData(tsp, tcw, 0)
	{
		glTexID = id;
		width = w;
		height = h;
	}
};

// We can't easily create TextureCacheData objects outside the cache.
// Instead, hook into the renderer at a lower level — populate rend_context
// and call Process+Render manually, with texture pointers resolved.

static std::unordered_map<u32, TextureCacheData*> _texCacheEntries;

static void buildRendContext(const RecordedFrame& rf, rend_context& rc)
{
	rc.Clear();

	rc.fZ_max = rf.fZ_max;
	rc.isRTT = false;
	rc.clearFramebuffer = rf.clearFB;
	rc.framebufferWidth = rf.fbW;
	rc.framebufferHeight = rf.fbH;

	// Tile clip — standard 640x480 = 20x15 tiles
	rc.ta_GLOB_TILE_CLIP.tile_x_num = (rf.fbW / 32) - 1;
	rc.ta_GLOB_TILE_CLIP.tile_y_num = (rf.fbH / 32) - 1;

	// Scaler — standard
	rc.scaler_ctl.full = 0;
	rc.scaler_ctl.vscalefactor = 0x400;  // 1.0

	// Framebuffer clip
	rc.fb_X_CLIP.min = 0;
	rc.fb_X_CLIP.max = rf.fbW - 1;
	rc.fb_Y_CLIP.min = 0;
	rc.fb_Y_CLIP.max = rf.fbH - 1;

	// Copy geometry
	rc.verts = rf.verts;
	rc.idx = rf.idx;

	// Build poly params with texture pointers
	auto buildPolyList = [&](const std::vector<RecordedFrame::SerializedPoly>& src,
		std::vector<PolyParam>& dst) {
		dst.resize(src.size());
		for (size_t i = 0; i < src.size(); i++)
		{
			dst[i].init();
			dst[i].first = src[i].first;
			dst[i].count = src[i].count;
			dst[i].tsp.full = src[i].tsp_full;
			dst[i].tcw.full = src[i].tcw_full;
			dst[i].isp.full = src[i].isp_full;
			dst[i].pcw.full = src[i].pcw_full;

			// Resolve texture from flycast's texture cache.
			// VRAM is populated from save state — GetTexture reads VRAM, decodes, uploads to GPU.
			dst[i].texture = nullptr;
			if (dst[i].pcw.Texture)
				dst[i].texture = renderer->GetTexture(dst[i].tsp, dst[i].tcw);
		}
	};

	buildPolyList(rf.polys_op, rc.global_param_op);
	buildPolyList(rf.polys_pt, rc.global_param_pt);
	buildPolyList(rf.polys_tr, rc.global_param_tr);

	// Single render pass
	rc.render_passes.clear();
	RenderPass pass{};
	pass.op_count = (u32)rc.global_param_op.size();
	pass.pt_count = (u32)rc.global_param_pt.size();
	pass.tr_count = (u32)rc.global_param_tr.size();
	pass.mvo_count = 0;
	pass.mvo_tr_count = 0;
	pass.sorted_tr_count = 0;
	pass.autosort = false;
	pass.z_clear = true;
	pass.mv_op_tr_shared = false;
	rc.render_passes.push_back(pass);
}

// ==================== PUBLIC API ====================

bool init()
{
	const char* recPath = std::getenv("MAPLECAST_REC_PATH");
	const char* texPath = std::getenv("MAPLECAST_TEX_PATH");
	if (!recPath) recPath = "rend_recording.bin";
	if (!texPath) texPath = "rend_textures";

	printf("[CLIENT] === MAPLECAST CLIENT MODE ===\n");
	printf("[CLIENT] Recording: %s\n", recPath);
	printf("[CLIENT] Textures: %s\n", texPath);

	if (!loadRecording(recPath)) return false;
	if (!loadTextures(texPath)) {
		printf("[CLIENT] WARNING: no textures loaded, rendering untextured\n");
	}

	// Initialize the TA context
	_clientCtx.Alloc();

	_active = true;
	_currentFrame = 0;

	printf("[CLIENT] === READY — %d frames, %zu textures ===\n",
		_frameCount, _textures.size());
	return true;
}

bool active()
{
	return _active;
}

void renderFrame()
{
	if (!_active || _frameCount == 0) return;

	// Build rend_context from recorded frame
	buildRendContext(_frames[_currentFrame], _clientCtx.rend);

	// Set the renderer's context pointer directly — skip Process() entirely.
	// Process() calls ta_parse which reads raw TA command data we don't have.
	// We already have the finished rend_context, just point the renderer at it.
	gl.rendContext = &_clientCtx.rend;

	// Render the frame using flycast's real OpenGL renderer
	renderer->Render();

	_currentFrame = (_currentFrame + 1) % _frameCount;
}

}  // namespace maplecast_client
