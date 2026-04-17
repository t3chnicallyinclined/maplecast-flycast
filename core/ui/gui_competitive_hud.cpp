/*
	Competitive HUD — implementation.

	Matches the WebGPU test page diagnostics panel. Four sections:

	  F1 — PIPELINE: decode, render, process, E2E, stream bandwidth
	  F2 — NETWORK:  grade, RTT, jitter, send rate, failover status
	  F3 — INPUT:    button/trigger changes, packets sent, redundancy
	  F12 — toggle all

	Reads telemetry from:
	  • maplecast_input_sink::getStats()  — E2E latency, send rate,
	     redundant sends, failover count, primary/backup status
	  • maplecast_mirror::getClientStats() — TA frame arrival timing,
	     decode cost, dirty page count, bandwidth

	All reads are atomic loads — zero locking, safe to call every frame.
	Renders into small ImGui windows in the top-left corner.
	Windows have NoInputs flag so they never steal mouse from the gear.
*/
#include "gui_competitive_hud.h"
#include "note_highway.h"

#include <atomic>
#include <cstdio>

#include <imgui.h>

#include "network/maplecast_input_sink.h"
#include "network/maplecast_mirror.h"

namespace gui_competitive_hud
{

// Section visibility. Defaults: all on so user sees diagnostics immediately.
static std::atomic<bool> _showPipeline{true};
static std::atomic<bool> _showNetwork{true};
static std::atomic<bool> _showInput{false};

// ── Network grading ───────────────────────────────────────────────────
struct Grade {
	const char* letter;
	ImVec4 color;
};

static Grade computeGrade(double rtt_ms, double jitter_ms, double loss_pct)
{
	// Thresholds tuned for fighting games: 16.67ms = 1 frame @ 60fps.
	// S = sub-frame, A = 1-frame, B = 2-frame, C = playable, F = unplayable.
	if (rtt_ms < 8.0   && jitter_ms < 1.0 && loss_pct < 0.1)
		return {"S", ImVec4(0.40f, 1.00f, 0.40f, 1.0f)};
	if (rtt_ms < 20.0  && jitter_ms < 3.0 && loss_pct < 0.5)
		return {"A", ImVec4(0.50f, 1.00f, 0.50f, 1.0f)};
	if (rtt_ms < 40.0  && jitter_ms < 5.0 && loss_pct < 1.0)
		return {"B", ImVec4(0.90f, 0.90f, 0.30f, 1.0f)};
	if (rtt_ms < 80.0  && jitter_ms < 10.0 && loss_pct < 3.0)
		return {"C", ImVec4(1.00f, 0.60f, 0.30f, 1.0f)};
	return {"F", ImVec4(1.00f, 0.30f, 0.30f, 1.0f)};
}

// ── Layout helpers ────────────────────────────────────────────────────

static const ImGuiWindowFlags HUD_WINDOW_FLAGS =
	ImGuiWindowFlags_NoDecoration |
	ImGuiWindowFlags_NoMove |
	ImGuiWindowFlags_NoResize |
	ImGuiWindowFlags_AlwaysAutoResize |
	ImGuiWindowFlags_NoSavedSettings |
	ImGuiWindowFlags_NoInputs |
	ImGuiWindowFlags_NoFocusOnAppearing |
	ImGuiWindowFlags_NoNav;

static const ImVec4 HEADER_COLOR  = ImVec4(0.6f, 0.85f, 1.0f, 1.0f);
static const ImVec4 DIM_COLOR     = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
static const ImVec4 WARN_COLOR    = ImVec4(1.0f, 0.7f, 0.3f, 1.0f);

// ── Sections ──────────────────────────────────────────────────────────

static void drawPipelineSection(float& yCursor)
{
	auto mirror = maplecast_mirror::getClientStats();
	auto input  = maplecast_input_sink::getStats();

	ImGui::SetNextWindowPos(ImVec2(8, yCursor), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.78f);
	ImGui::Begin("##hud_pipeline", nullptr, HUD_WINDOW_FLAGS);

	ImGui::TextColored(HEADER_COLOR, "PIPELINE");
	ImGui::Separator();

	// Decode time (zstd decompress + TA delta apply)
	double decodeMs = mirror.decodeEmaUs / 1000.0;
	ImGui::Text("Decode:  %.2fms", decodeMs);

	// Frame arrival interval (should be ~16.67ms for 60fps)
	double arrivalMs = mirror.arrivalEmaUs / 1000.0;
	// Jitter = how far the EMA is from the ideal 16.67ms target
	double jitterMs = arrivalMs > 0 ? arrivalMs - 16.667 : 0.0;
	if (jitterMs < 0) jitterMs = -jitterMs;

	// Render FPS from arrival interval
	double renderFps = arrivalMs > 0 ? 1000.0 / arrivalMs : 0.0;
	ImGui::Text("FPS:     %.0f (%.1fms/f, jitter %.1fms)", renderFps, arrivalMs, jitterMs);

	// TA size + dirty pages
	ImGui::Text("TA:      %u bytes", mirror.lastTaSize);
	ImGui::Text("Dirty:   %u pages%s", mirror.lastDirtyPages,
	            mirror.lastVramDirty ? " [VRAM]" : "");

	// Stream bandwidth (bytes received → Mbps over ~1s window)
	static uint64_t prevBytes = 0;
	static uint64_t prevFrame = 0;
	static double streamMbps = 0.0;
	if (mirror.frameCount > prevFrame + 60) {
		uint64_t deltaBytes = mirror.bytesReceived - prevBytes;
		streamMbps = (deltaBytes * 8.0) / 1000000.0; // per ~1s
		prevBytes = mirror.bytesReceived;
		prevFrame = mirror.frameCount;
	}
	ImGui::Text("Stream:  %.1f Mbps", streamMbps);

	// E2E latency (button press → visual change on screen)
	if (input.e2eProbes > 0) {
		ImGui::Text("E2E:     %.1fms (avg %.1fms)", input.e2eLastMs, input.e2eEmaMs);
	} else {
		ImGui::TextColored(DIM_COLOR, "E2E:     press a button...");
	}

	// Frames received
	ImGui::TextColored(DIM_COLOR, "Frames:  %llu", (unsigned long long)mirror.frameCount);

	// Connection status
	if (!mirror.wsConnected)
		ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "DISCONNECTED");

	float h = ImGui::GetWindowHeight();
	ImGui::End();
	yCursor += h + 4;
}

static void drawNetworkSection(float& yCursor)
{
	auto stats = maplecast_input_sink::getStats();

	double rtt = stats.e2eEmaMs;
	// Jitter from arrival EMA deviation, not E2E min/max (which are session peaks)
	auto mirror = maplecast_mirror::getClientStats();
	double arrivalMs = mirror.arrivalEmaUs / 1000.0;
	double jitter = arrivalMs > 0 ? arrivalMs - 16.667 : 0.0;
	if (jitter < 0) jitter = -jitter;
	double loss = 0.0;

	Grade g = computeGrade(rtt, jitter, loss);

	ImGui::SetNextWindowPos(ImVec2(8, yCursor), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.78f);
	ImGui::Begin("##hud_network", nullptr, HUD_WINDOW_FLAGS);

	ImGui::TextColored(HEADER_COLOR, "NETWORK");
	ImGui::SameLine(0, 16);
	ImGui::TextColored(g.color, "%s", g.letter);
	ImGui::Separator();

	if (stats.e2eProbes > 0) {
		ImGui::Text("RTT:     %.1fms (last %.1fms)", rtt, stats.e2eLastMs);
		ImGui::Text("Jitter:  %.1fms", jitter);
	} else {
		ImGui::TextColored(DIM_COLOR, "RTT:     waiting...");
	}

	ImGui::Text("Send:    %u pps", stats.sendRateHz);
	ImGui::Text("Sent:    %llu (+%llu dup)",
		(unsigned long long)stats.packetsSent,
		(unsigned long long)stats.redundantSends);

	if (stats.onBackupServer)
		ImGui::TextColored(WARN_COLOR, "[FAILOVER ACTIVE]");
	if (stats.hasBackup)
		ImGui::TextColored(DIM_COLOR, "Standby: ready");
	if (stats.failovers > 0)
		ImGui::TextColored(WARN_COLOR, "Failovers: %llu",
		                   (unsigned long long)stats.failovers);

	float h = ImGui::GetWindowHeight();
	ImGui::End();
	yCursor += h + 4;
}

static void drawInputSection(float& yCursor)
{
	auto stats = maplecast_input_sink::getStats();

	ImGui::SetNextWindowPos(ImVec2(8, yCursor), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.78f);
	ImGui::Begin("##hud_input", nullptr, HUD_WINDOW_FLAGS);

	ImGui::TextColored(HEADER_COLOR, "INPUT");
	ImGui::Separator();
	ImGui::Text("Buttons: %llu changes", (unsigned long long)stats.buttonChanges);
	ImGui::Text("Trigger: %llu changes", (unsigned long long)stats.triggerChanges);

	float h = ImGui::GetWindowHeight();
	ImGui::End();
	yCursor += h + 4;
}

// ── Public API ────────────────────────────────────────────────────────

void draw()
{
	if (!maplecast_mirror::isClient()) return;

	float y = 8.0f;

	if (_showPipeline.load(std::memory_order_relaxed)) drawPipelineSection(y);
	if (_showNetwork.load(std::memory_order_relaxed))  drawNetworkSection(y);
	if (_showInput.load(std::memory_order_relaxed))    drawInputSection(y);

	note_highway::draw();
}

void toggleNetwork() { _showPipeline.store(!_showPipeline.load()); }
void toggleLatency() { _showNetwork.store(!_showNetwork.load()); }
void toggleInput()   { _showInput.store(!_showInput.load()); }

void toggleAll()
{
	bool any = _showPipeline.load() || _showNetwork.load() || _showInput.load();
	bool newState = !any;
	_showPipeline.store(newState);
	_showNetwork.store(newState);
	_showInput.store(newState);
}

} // namespace gui_competitive_hud
