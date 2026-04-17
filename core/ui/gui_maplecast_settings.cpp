/*
	MapleCast Settings Panel — implementation.

	Matches the browser's GRAPHICS tab (king.html / webgpu-test.html).
	All settings map directly to flycast config:: variables and take
	effect immediately — no restart needed.
*/
#include "gui_maplecast_settings.h"
#include "cfg/option.h"
#include "network/maplecast_mirror.h"
#include "network/maplecast_input_sink.h"
#include "network/maplecast_input_server.h"

#include <imgui.h>
#include <atomic>
#include <cmath>
#include <cstdio>

namespace gui_maplecast_settings
{

static std::atomic<bool> _open{false};

// ── Preset definitions ───────────────────────────────────────────────

static void applyPresetPerformance()
{
	config::RenderResolution = 480;
	config::AnisotropicFiltering = 1;
	config::TextureFiltering = 0;
	config::PerPixelLayers = 4;
	config::Fog = false;
	config::ModifierVolumes = false;
	config::PerStripSorting = false;
	printf("[settings] Preset: PERFORMANCE\n");
}

static void applyPresetArcade()
{
	config::RenderResolution = 480;
	config::AnisotropicFiltering = 1;
	config::TextureFiltering = 0;
	config::PerPixelLayers = 8;
	config::Fog = true;
	config::ModifierVolumes = false;
	config::PerStripSorting = false;
	printf("[settings] Preset: ARCADE\n");
}

static void applyPresetMaxQuality()
{
	config::RenderResolution = 1920;
	config::AnisotropicFiltering = 16;
	config::TextureFiltering = 2;
	config::PerPixelLayers = 32;
	config::Fog = true;
	config::ModifierVolumes = true;
	config::PerStripSorting = true;
	printf("[settings] Preset: MAX QUALITY\n");
}

// ── Gear button (always visible, top-right) ──────────────────────────

static void drawGearButton()
{
	float btnSize = 36.0f;
	float pad = 10.0f;
	ImGui::SetNextWindowPos(
		ImVec2(ImGui::GetIO().DisplaySize.x - btnSize - pad, pad),
		ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(
		_open.load(std::memory_order_relaxed) ? 0.85f : 0.45f);
	ImGui::Begin("##gear", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
		ImGuiWindowFlags_AlwaysAutoResize);

	bool isOpen = _open.load(std::memory_order_relaxed);
	if (isOpen)
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.8f, 0.9f));
	else
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 0.7f));

	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.4f, 0.7f, 1.0f));

	if (ImGui::Button("S", ImVec2(btnSize, btnSize)))
		_open.store(!isOpen, std::memory_order_relaxed);

	ImGui::PopStyleColor(3);
	ImGui::End();
}

// ── Drawing ──────────────────────────────────────────────────────────

void draw()
{
	if (!maplecast_mirror::isClient()) return;

	// Always draw the settings button
	drawGearButton();

	if (!_open.load(std::memory_order_relaxed)) return;

	bool open = true;
	ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 350, 8),
	                        ImGuiCond_FirstUseEver);
	ImGui::Begin("MapleCast Settings", &open,
	             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

	if (!open) {
		_open.store(false, std::memory_order_relaxed);
		ImGui::End();
		return;
	}

	// ── Presets ──────────────────────────────────────────────────
	ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "PRESETS");
	ImGui::Separator();
	if (ImGui::Button("Performance", ImVec2(100, 0))) applyPresetPerformance();
	ImGui::SameLine();
	if (ImGui::Button("Arcade", ImVec2(100, 0))) applyPresetArcade();
	ImGui::SameLine();
	if (ImGui::Button("Max Quality", ImVec2(100, 0))) applyPresetMaxQuality();
	ImGui::Spacing();

	// ── Graphics ────────────────────────────────────────────────
	ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "GRAPHICS");
	ImGui::Separator();

	// Resolution
	{
		static const int resOptions[] = { 480, 720, 960, 1440, 1920 };
		static const char* resLabels[] = { "480p (Native)", "720p", "960p", "1440p", "1920p (4x)" };
		int curRes = config::RenderResolution;
		int curIdx = 0;
		for (int i = 0; i < 5; i++)
			if (resOptions[i] == curRes) curIdx = i;
		if (ImGui::Combo("Resolution", &curIdx, resLabels, 5))
			config::RenderResolution = resOptions[curIdx];
	}

	// Anisotropic Filtering
	{
		static const int anisoOptions[] = { 1, 2, 4, 8, 16 };
		static const char* anisoLabels[] = { "Off", "2x", "4x", "8x", "16x" };
		int cur = config::AnisotropicFiltering;
		int curIdx = 0;
		for (int i = 0; i < 5; i++)
			if (anisoOptions[i] == cur) curIdx = i;
		if (ImGui::Combo("Aniso Filter", &curIdx, anisoLabels, 5))
			config::AnisotropicFiltering = anisoOptions[curIdx];
	}

	// Texture Filtering
	{
		static const char* texLabels[] = { "Default", "Nearest", "Linear" };
		int cur = config::TextureFiltering;
		if (ImGui::Combo("Tex Filter", &cur, texLabels, 3))
			config::TextureFiltering = cur;
	}

	// Transparency Layers
	{
		static const int layerOptions[] = { 4, 8, 16, 32 };
		static const char* layerLabels[] = { "4 (Fast)", "8", "16", "32 (Best)" };
		int cur = config::PerPixelLayers;
		int curIdx = 0;
		for (int i = 0; i < 4; i++)
			if (layerOptions[i] == cur) curIdx = i;
		if (ImGui::Combo("Transparency", &curIdx, layerLabels, 4))
			config::PerPixelLayers = layerOptions[curIdx];
	}

	// Toggles
	{
		bool fog = config::Fog;
		if (ImGui::Checkbox("Fog", &fog))
			config::Fog = fog;

		ImGui::SameLine();
		bool modvol = config::ModifierVolumes;
		if (ImGui::Checkbox("Modifier Volumes", &modvol))
			config::ModifierVolumes = modvol;

		bool pstrip = config::PerStripSorting;
		if (ImGui::Checkbox("Per-Strip Sorting", &pstrip))
			config::PerStripSorting = pstrip;
	}

	ImGui::Spacing();

	// ── Network (players only) ──────────────────────────────────
	if (!std::getenv("MAPLECAST_SPECTATE"))
	{
		ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "NETWORK (Player)");
		ImGui::Separator();

		// Latch policy
		{
			// Read current policy for slot 0
			bool isConsistency = false;
			if (maplecast_input::active())
				isConsistency = (maplecast_input::getLatchPolicy(0) ==
				                 maplecast_input::LatchPolicy::ConsistencyFirst);

			if (ImGui::RadioButton("Latency First", !isConsistency)) {
				if (maplecast_input::active())
					maplecast_input::setLatchPolicy(0, maplecast_input::LatchPolicy::LatencyFirst);
			}
			ImGui::SameLine();
			if (ImGui::RadioButton("Consistency First", isConsistency)) {
				if (maplecast_input::active())
					maplecast_input::setLatchPolicy(0, maplecast_input::LatchPolicy::ConsistencyFirst);
			}
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
				isConsistency ? "Edge-preserving, +1f latency" : "Instant read, 0 added latency");
		}

		// Input sink stats
		auto stats = maplecast_input_sink::getStats();
		ImGui::Text("Packets: %llu sent (%u pps)",
			(unsigned long long)stats.packetsSent, stats.sendRateHz);
		if (stats.e2eProbes > 0)
			ImGui::Text("E2E: %.1fms avg", stats.e2eEmaMs);
	}
	else
	{
		ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "SPECTATOR");
		ImGui::Separator();
		ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Read-only — no input forwarded");
	}

	ImGui::Spacing();

	// ── Info ─────────────────────────────────────────────────────
	ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "` toggle | Tab flycast settings | F1-F3 HUD");

	ImGui::End();
}

void toggle()
{
	_open.store(!_open.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

bool isOpen()
{
	return _open.load(std::memory_order_relaxed);
}

} // namespace gui_maplecast_settings
