/*
	MapleCast Settings Window — separate SDL window, created on main thread.

	The window + renderer are created on the main thread (init) to avoid
	SDL thread-safety issues. The UI is drawn once per main loop iteration
	via tick(), also on the main thread. This is called from mainui_loop
	alongside the game render, so it's zero-thread-conflict.
*/
#include "settings_window.h"
#include "network/maplecast_mirror.h"
#include "cfg/option.h"

#include <imgui.h>
#include <SDL.h>

#include <atomic>
#include <cstdio>
#include <cfloat>

namespace settings_window
{

static bool           _inited = false;
static SDL_Window*    _win = nullptr;
static SDL_Renderer*  _rend = nullptr;
static SDL_Texture*   _fontTex = nullptr;
static ImGuiContext*  _ctx = nullptr;
static ImGuiContext*  _prevCtx = nullptr;
static bool           _visible = false;

static void applyPresetPerformance()
{
	config::RenderResolution = 480;
	config::PerPixelLayers = 4;
	config::Fog = false;
	config::ModifierVolumes = false;
	config::AnisotropicFiltering = 1;
	config::TextureFiltering = 0;
}
static void applyPresetArcade()
{
	config::RenderResolution = 960;
	config::PerPixelLayers = 8;
	config::Fog = true;
	config::ModifierVolumes = true;
	config::AnisotropicFiltering = 4;
	config::TextureFiltering = 0;
}
static void applyPresetMaxQuality()
{
	config::RenderResolution = 1920;
	config::PerPixelLayers = 32;
	config::Fog = true;
	config::ModifierVolumes = true;
	config::AnisotropicFiltering = 16;
	config::TextureFiltering = 2;
}

static void renderDrawData()
{
	ImDrawData* dd = ImGui::GetDrawData();
	if (!dd || !_rend) return;

	SDL_SetRenderDrawColor(_rend, 25, 25, 25, 255);
	SDL_RenderClear(_rend);

	for (int n = 0; n < dd->CmdListsCount; n++) {
		const ImDrawList* cl = dd->CmdLists[n];
		const ImDrawVert* vtx = cl->VtxBuffer.Data;
		const ImDrawIdx* idx = cl->IdxBuffer.Data;

		for (int ci = 0; ci < cl->CmdBuffer.Size; ci++) {
			const ImDrawCmd& cmd = cl->CmdBuffer[ci];
			if (cmd.UserCallback) { cmd.UserCallback(cl, &cmd); continue; }

			SDL_Rect clip = {
				(int)cmd.ClipRect.x, (int)cmd.ClipRect.y,
				(int)(cmd.ClipRect.z - cmd.ClipRect.x),
				(int)(cmd.ClipRect.w - cmd.ClipRect.y)
			};
			SDL_RenderSetClipRect(_rend, &clip);

			SDL_RenderGeometryRaw(_rend, (SDL_Texture*)cmd.GetTexID(),
				&vtx->pos.x, sizeof(ImDrawVert),
				(const SDL_Color*)&vtx->col, sizeof(ImDrawVert),
				&vtx->uv.x, sizeof(ImDrawVert),
				cl->VtxBuffer.Size,
				idx + cmd.IdxOffset, cmd.ElemCount, sizeof(ImDrawIdx));
		}
	}

	SDL_RenderSetClipRect(_rend, nullptr);
	SDL_RenderPresent(_rend);
}

void init()
{
	if (_inited) return;

	_win = SDL_CreateWindow("MapleCast Settings",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		420, 650, SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
	if (!_win) {
		printf("[settings] SDL_CreateWindow failed: %s\n", SDL_GetError());
		return;
	}

	_rend = SDL_CreateRenderer(_win, -1, SDL_RENDERER_SOFTWARE);
	if (!_rend) {
		printf("[settings] SDL_CreateRenderer failed: %s\n", SDL_GetError());
		SDL_DestroyWindow(_win); _win = nullptr;
		return;
	}

	// Save the main ImGui context, create our own
	_prevCtx = ImGui::GetCurrentContext();
	_ctx = ImGui::CreateContext();
	ImGui::SetCurrentContext(_ctx);
	ImGui::StyleColorsDark();
	ImGui::GetStyle().WindowRounding = 6.0f;
	ImGui::GetStyle().FrameRounding = 4.0f;

	// Build font texture
	ImGuiIO& io = ImGui::GetIO();
	unsigned char* pixels; int w, h;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
	_fontTex = SDL_CreateTexture(_rend,
		SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
	SDL_UpdateTexture(_fontTex, nullptr, pixels, w * 4);
	SDL_SetTextureBlendMode(_fontTex, SDL_BLENDMODE_BLEND);
	io.Fonts->SetTexID((ImTextureID)(intptr_t)_fontTex);

	// Restore the main ImGui context
	ImGui::SetCurrentContext(_prevCtx);

	_visible = true;
	_inited = true;
	printf("[settings] window ready\n");
}

// Called once per main loop iteration from mainui_loop.
// Switches ImGui context, draws, switches back. Fast.
void tick()
{
	if (!_inited || !_visible) return;

	// Switch to our ImGui context
	ImGuiContext* mainCtx = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(_ctx);

	ImGuiIO& io = ImGui::GetIO();

	// Feed mouse state
	int mx, my;
	Uint32 mstate = SDL_GetMouseState(&mx, &my);
	Uint32 flags = SDL_GetWindowFlags(_win);
	if (flags & SDL_WINDOW_MOUSE_FOCUS) {
		io.AddMousePosEvent((float)mx, (float)my);
		io.AddMouseButtonEvent(0, (mstate & SDL_BUTTON_LMASK) != 0);
		io.AddMouseButtonEvent(1, (mstate & SDL_BUTTON_RMASK) != 0);
	} else {
		io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
	}

	int ww, wh;
	SDL_GetWindowSize(_win, &ww, &wh);
	io.DisplaySize = ImVec2((float)ww, (float)wh);
	io.DeltaTime = 1.0f / 60.0f;

	ImGui::NewFrame();

	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImVec2((float)ww, (float)wh));

	ImGui::Begin("##settings", nullptr,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

	ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "MapleCast Settings");
	ImGui::Separator();

	ImGui::Text("Presets:");
	ImGui::SameLine();
	if (ImGui::SmallButton("Performance")) applyPresetPerformance();
	ImGui::SameLine();
	if (ImGui::SmallButton("Arcade")) applyPresetArcade();
	ImGui::SameLine();
	if (ImGui::SmallButton("Max Quality")) applyPresetMaxQuality();
	ImGui::Separator();

	if (ImGui::CollapsingHeader("Video", ImGuiTreeNodeFlags_DefaultOpen)) {
		static const int RES[] = { 480, 720, 960, 1080, 1440, 1920, 2160 };
		static const char* RES_L[] = { "480p", "720p", "960p (2x)", "1080p", "1440p (3x)", "1920p (4x)", "2160p" };
		int cur = config::RenderResolution.get();
		int sel = 0;
		for (int i = 0; i < 7; i++) if (RES[i] == cur) { sel = i; break; }
		if (ImGui::Combo("Resolution", &sel, RES_L, 7))
			config::RenderResolution = RES[sel];

		int texF = config::TextureFiltering.get();
		if (ImGui::Combo("Texture filter", &texF, "Default\0Nearest\0Linear\0"))
			config::TextureFiltering.set(texF);

		static const int ANISO[] = { 1, 2, 4, 8, 16 };
		static const char* ANISO_L[] = { "Off", "2x", "4x", "8x", "16x" };
		int curA = config::AnisotropicFiltering.get();
		int aS = 0;
		for (int i = 0; i < 5; i++) if (ANISO[i] == curA) { aS = i; break; }
		if (ImGui::Combo("Anisotropic", &aS, ANISO_L, 5))
			config::AnisotropicFiltering.set(ANISO[aS]);

		static const int LAY[] = { 4, 8, 16, 32 };
		static const char* LAY_L[] = { "4", "8", "16", "32" };
		int curL = config::PerPixelLayers.get();
		int lS = 0;
		for (int i = 0; i < 4; i++) if (LAY[i] == curL) { lS = i; break; }
		if (ImGui::Combo("Transparency layers", &lS, LAY_L, 4))
			config::PerPixelLayers.set(LAY[lS]);

		bool fog = config::Fog.get(); if (ImGui::Checkbox("Fog", &fog)) config::Fog.set(fog);
		bool mod = config::ModifierVolumes.get(); if (ImGui::Checkbox("Modifier volumes", &mod)) config::ModifierVolumes.set(mod);
		bool mip = config::UseMipmaps.get(); if (ImGui::Checkbox("Mipmaps", &mip)) config::UseMipmaps.set(mip);
		bool ws = config::Widescreen.get(); if (ImGui::Checkbox("Widescreen", &ws)) config::Widescreen.set(ws);
		bool lin = config::LinearInterpolation.get(); if (ImGui::Checkbox("Linear interp", &lin)) config::LinearInterpolation.set(lin);
		bool fps = config::ShowFPS.get(); if (ImGui::Checkbox("Show FPS", &fps)) config::ShowFPS.set(fps);
	}

	ImGui::Separator();
	ImGui::TextDisabled("Post-processing (CRT, bloom) — coming soon");

	ImGui::Separator();
	if (ImGui::CollapsingHeader("Connection", ImGuiTreeNodeFlags_DefaultOpen)) {
		auto vs = maplecast_mirror::getClientStats();
		ImGui::Text("Stream: %s", vs.wsConnected ? "Connected" : "Disconnected");
		ImGui::Text("Frames: %llu", (unsigned long long)vs.frameCount);
		ImGui::Text("Data: %.1f MB", (double)vs.bytesReceived / (1024.0*1024.0));
	}

	ImGui::End();
	ImGui::Render();
	renderDrawData();

	// Restore main ImGui context
	ImGui::SetCurrentContext(mainCtx);
}

void show()  { if (_win) { SDL_ShowWindow(_win); _visible = true; } }
void hide()  { if (_win) { SDL_HideWindow(_win); _visible = false; } }
void toggle() { _visible ? hide() : show(); }

void shutdown()
{
	if (!_inited) return;
	ImGui::SetCurrentContext(_ctx);
	ImGui::DestroyContext(_ctx);
	_ctx = nullptr;
	if (_fontTex) { SDL_DestroyTexture(_fontTex); _fontTex = nullptr; }
	if (_rend) { SDL_DestroyRenderer(_rend); _rend = nullptr; }
	if (_win) { SDL_DestroyWindow(_win); _win = nullptr; }
	_inited = false;
}

}
