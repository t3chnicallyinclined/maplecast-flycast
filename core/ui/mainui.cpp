/*
	Copyright 2020 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "mainui.h"
#include "hw/pvr/Renderer_if.h"
#include "gui.h"
#include "oslib/oslib.h"
#include "wsi/context.h"
#include "cfg/option.h"
#include "emulator.h"
#include "imgui_driver.h"
#include "profiler/fc_profiler.h"
#include "oslib/i18n.h"
#include "network/maplecast_mirror.h"
#include "network/maplecast_palette.h"

#include <chrono>
#include <thread>
#include <cstdlib>

#ifdef USE_SDL
#include <SDL.h>
#endif

static bool mainui_enabled;
u32 MainFrameCount;
static bool forceReinit;

bool mainui_rend_frame()
{
	FC_PROFILE_SCOPE;

	os_DoEvents();
	os_UpdateInputState();

	// Mirror client: skip the ROM selection GUI on first frame, but
	// allow the user to open settings (Back/Select button) afterwards.
	if (maplecast_mirror::isClient())
	{
		static bool firstFrame = true;
		if (firstFrame && gui_is_open()) {
			gui_setState(GuiState::Closed);
			firstFrame = false;
		}
	}

	if (gui_is_open())
	{
		try {
			gui_display_ui();
		} catch (const FlycastException& e) {
			forceReinit = true;
			return false;
		}
#ifndef TARGET_IPHONE
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
#endif
	}
	else if (maplecast_mirror::isClient())
	{
		// === MapleCast mirror client render loop ===
		//
		// What I learned the hard way diagnosing the stutter:
		//
		//   1. The decode path (clientReceive + renderer->Process) runs on
		//      this render thread and takes ~15-18 ms per frame. That's
		//      already near the full 60 Hz budget.
		//   2. The outer loop (see mainui_loop below) calls
		//      imguiDriver->present() → SDL_GL_SwapWindow which blocks on
		//      SwapInterval. On a 240 Hz panel flycast sets SwapInterval=4
		//      (= 16.67 ms) and on a 60 Hz panel it's 1 (= 16.67 ms).
		//   3. Total loop time ≈ 18 ms decode + 16.67 ms swap ≈ 34 ms →
		//      effective ~30 fps, dropping every other server frame, which
		//      is exactly what "choppy" feels like.
		//   4. The browser WASM client doesn't have this problem because
		//      its decode runs on a web worker in parallel with the main
		//      thread's rAF-paced present — the two halves overlap.
		//
		// Proper fix is "run decode on a separate GL context in parallel
		// with present," which is a real refactor. The interim fix is to
		// minimize the per-iteration cost so decode can happen at ~60 Hz
		// unblocked by swap: in mirror-client mode we set SwapInterval=0
		// (tear is imperceptible at 4 ms scan-out on a 240 Hz panel) so
		// Present returns immediately and the loop cadence is set by the
		// decode path instead of by two back-to-back blockers.

		static rend_context mirrorCtx;
		static bool _swapIntervalOverridden = false;
		if (!_swapIntervalOverridden)
		{
			// One-shot: disable vsync for the mirror-client render loop.
			// SDL_GL_SetSwapInterval returns -1 if the platform doesn't
			// support it — we ignore that, it's best-effort.
#ifdef USE_SDL
			SDL_GL_SetSwapInterval(0);
#endif
			_swapIntervalOverridden = true;
			printf("[MIRROR] render loop: SwapInterval=0 (vsync off) to let decode pace the loop\n");
		}

		bool vramDirty = false;

		// Drain ALL pending frames in one iteration. clientReceive returns
		// false when there's nothing new — the while loop keeps up if we
		// fall behind, and runs exactly once in the common case.
		bool drained = false;
		while (maplecast_mirror::clientReceive(mirrorCtx, vramDirty))
			drained = true;

		if (drained)
		{
			// Apply client-side palette overrides AFTER the TA stream
			// wrote the server's palette but BEFORE the renderer reads it.
			// Zero flicker, zero server involvement.
			maplecast_palette::applyClientOverrides();

			bool isScreen = renderer->Render();
			if (isScreen)
			{
				// Draw the ImGui overlay AFTER the game render but BEFORE
				// Present swaps the buffer — so the overlay is composited
				// on top of the TA frame in the same back buffer.
				gui_displayMirrorDebug();
				renderer->Present();
			}
		}
		else
		{
			// No new server frame this iteration — still pump the overlay
			// so the gear icon + settings panel stay responsive.
			gui_displayMirrorDebug();
		}
	}
	else
	{
		try {
			if (!emu.render())
				return false;
			if (config::ProfilerEnabled && config::ProfilerDrawToGUI)
				gui_display_profiler();
		} catch (const RendererException& e) {
			gui_error(i18n::Ts("Renderer error:") + "\n" + e.what() + "\n\n"
					+ i18n::Ts("The game has been paused but it is recommended to restart Flycast"));
			rend_term_renderer();
			if (!rend_init_renderer())
				ERROR_LOG(RENDERER, "Renderer re-initialization failed");
			gui_open_settings();
			return false;
		} catch (const FlycastException& e) {
			gui_stop_game(e.what());
			return false;
		}
	}
	MainFrameCount++;

	return true;
}

void mainui_init()
{
	if (!rend_init_renderer()) {
		ERROR_LOG(RENDERER, "Renderer initialization failed");
		gui_error(i18n::T("Renderer initialization failed.\nPlease select a different graphics API"));
	}
	// MapleCast client mode init moved to emulator.cpp (after save state loads)
}

void mainui_term()
{
	rend_term_renderer();
}

void mainui_loop(bool forceStart)
{
	ThreadName _("Flycast-rend");
	if (forceStart)
		mainui_enabled = true;
	mainui_init();
	RenderType currentRenderer = config::RendererType;

	while (mainui_enabled)
	{
		fc_profiler::startThread("main");

		const bool headless = maplecast_mirror::isHeadless();

		if (mainui_rend_frame() && imguiDriver != nullptr)
		{
			try {
				imguiDriver->present();
			} catch (const FlycastException& e) {
				forceReinit = true;
			}
		}
		// Headless mode has no window → no imguiDriver. That's fine.
		// Only treat a null driver as an error on GUI builds.
		if (imguiDriver == nullptr && !headless)
			forceReinit = true;

		if (!headless && (config::RendererType != currentRenderer || forceReinit))
		{
			mainui_term();
			int prevApi = isOpenGL(currentRenderer) ? 0 : isVulkan(currentRenderer) ? 1 : currentRenderer == RenderType::DirectX9 ? 2 : 3;
			int newApi = isOpenGL(config::RendererType) ? 0 : isVulkan(config::RendererType) ? 1 : config::RendererType == RenderType::DirectX9 ? 2 : 3;
			if (newApi != prevApi || forceReinit)
			{
				try {
					switchRenderApi();
				} catch (const FlycastException& e) {
					ERROR_LOG(RENDERER, "switchRenderApi failed: %s", e.what());
					if (prevApi == newApi)
						// fatal
						throw;
					// try to go back to the previous API
					config::RendererType = currentRenderer;
					try {
						switchRenderApi();
					} catch (const FlycastException& e) {
						ERROR_LOG(RENDERER, "Falling back to previous renderer also failed: %s", e.what());
						// fatal
						throw;
					}
				}
			}
			mainui_init();
			forceReinit = false;
			currentRenderer = config::RendererType;
		}

		fc_profiler::endThread(config::ProfilerFrameWarningTime);
	}

	mainui_term();
}

void mainui_start()
{
	mainui_enabled = true;
}

void mainui_stop()
{
	mainui_enabled = false;
}

void mainui_reinit()
{
	forceReinit = true;
}
