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
#include "network/maplecast_state_replica.h"
#include "network/maplecast_palette.h"
#include "gui_maplecast_settings.h"
#include "gui_game_overlay.h"

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

	// Mirror client + state-replica: suppress the startup ROM-selection GUI until
	// it's actually closed, then stop interfering (so Back-button settings still work).
	if (maplecast_mirror::isClient() || maplecast_state_replica::active())
	{
		static bool startupGuiDismissed = false;
		if (!startupGuiDismissed) {
			if (gui_is_open()) {
				gui_setState(GuiState::Closed);
			} else {
				startupGuiDismissed = true;
			}
		}
	}

	if (gui_is_open())
	{
		// Debug: confirm we're entering the gui_display_ui branch when
		// gui_state is non-Closed. Fires once per second max to avoid spam.
		static int64_t _lastGuiDbg = 0;
		int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		if (now_us - _lastGuiDbg > 1000000) {
			_lastGuiDbg = now_us;
			printf("[mainui] gui_is_open=true (state=%d) -> gui_display_ui()\n",
				(int)gui_state);
			fflush(stdout);
		}
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
			//
			// MAPLECAST_CLIENT_VSYNC=1 keeps vsync ON for A/B latency
			// testing — present blocks for the next display refresh, adds
			// 0-16ms of display latency, eliminates tearing.
			const bool wantVsync = std::getenv("MAPLECAST_CLIENT_VSYNC") != nullptr;
			const int swapInterval = wantVsync ? 1 : 0;
#ifdef USE_SDL
			SDL_GL_SetSwapInterval(swapInterval);
#endif
			_swapIntervalOverridden = true;
			printf("[MIRROR] render loop: SwapInterval=%d (vsync %s) %s\n",
				swapInterval,
				wantVsync ? "ON" : "OFF",
				wantVsync ? "— blocks on display refresh"
				          : "— decode paces the loop");
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
		// State-replica phase-1: poll for MCSV each frame so the mode switch
		// fires as soon as the server's savestate arrives. frameInject() is a
		// no-op until MCSV is queued; when it applies it, setClientRendering(false)
		// clears isClient() and the next iteration falls to the emu.render() branch.
		// Pump for ANY active replica (not just no-ROM): with a ROM loaded we're
		// still isClient() during phase 1, but the run() loop that would otherwise
		// call frameInject() is bypassed by this mirror-render branch — so without
		// this the queued MCSV never applies and we're stuck rendering the mirror.
		if (maplecast_state_replica::active())
			maplecast_state_replica::frameInject();
	}
	else
	{
		try {
			if (!emu.render())
				return false;
			if (config::ProfilerEnabled && config::ProfilerDrawToGUI)
				gui_display_profiler();
			// MapleCast overlays in server/local mode — same overlays the
			// mirror client shows, now available when running the game locally.
			// Skip in headless mode: there's no ImGui context / imguiDriver, so
			// gui_displayMirrorDebug() → gui_newFrame() null-derefs ~380ms after
			// emu thread spawn. The original 8ff71a9ca was for local-with-display
			// play; the imguiDriver check restores headless safety.
			if (maplecast_mirror::isServer() && imguiDriver != nullptr)
				gui_displayMirrorDebug();
		} catch (const RendererException& e) {
			gui_error(i18n::Ts("Renderer error:") + "\n" + e.what() + "\n\n"
					+ i18n::Ts("The game has been paused but it is recommended to restart Flycast"));
			rend_term_renderer();
			if (!rend_init_renderer())
				ERROR_LOG(RENDERER, "Renderer re-initialization failed");
			gui_open_settings();
			return false;
		} catch (const FlycastException& e) {
			printf("[state-replica] CRASH exception: %s\n", e.what());
			fflush(stdout);
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
