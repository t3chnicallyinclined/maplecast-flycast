/*
	Competitive HUD — implementation.

	Reads telemetry from:
	  • maplecast_input_sink::getStats()  — E2E latency, send rate,
	     redundant sends, failover count, primary/backup status
	  • maplecast_mirror::getClientStats() — TA frame arrival timing,
	     decode cost, dirty page count

	All reads are atomic loads — zero locking, safe to call every frame.
	Renders into 3 small ImGui windows in the bottom-left corner.
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

// Section visibility. Defaults: NETWORK + LATENCY on, INPUT off.
// Atomic so the F-key toggle handler can flip them safely.
static std::atomic<bool> _showNetwork{true};
static std::atomic<bool> _showLatency{true};
static std::atomic<bool> _showInput{false};

// ── Network grading ───────────────────────────────────────────────────
//
// Quick visual indicator: S/A/B/C/F based on RTT + jitter + loss.
// Thresholds match docs/COMPETITIVE-CLIENT.md "Network Grading" table.
struct Grade {
	const char* letter;
	ImVec4 color;
};

static Grade computeGrade(double rtt_ms, double jitter_ms, double loss_pct)
{
	if (rtt_ms < 15.0  && jitter_ms < 0.5 && loss_pct < 0.001)
		return {"S", ImVec4(0.40f, 1.00f, 0.40f, 1.0f)};  // bright green
	if (rtt_ms < 30.0  && jitter_ms < 1.0 && loss_pct < 0.1)
		return {"A", ImVec4(0.50f, 1.00f, 0.50f, 1.0f)};  // green
	if (rtt_ms < 60.0  && jitter_ms < 2.0 && loss_pct < 0.5)
		return {"B", ImVec4(0.90f, 0.90f, 0.30f, 1.0f)};  // yellow
	if (rtt_ms < 100.0 && jitter_ms < 5.0 && loss_pct < 2.0)
		return {"C", ImVec4(1.00f, 0.60f, 0.30f, 1.0f)};  // orange
	return {"F", ImVec4(1.00f, 0.30f, 0.30f, 1.0f)};       // red
}

// ── Layout helpers ────────────────────────────────────────────────────

static const ImGuiWindowFlags HUD_WINDOW_FLAGS =
	ImGuiWindowFlags_NoDecoration |
	ImGuiWindowFlags_NoMove |
	ImGuiWindowFlags_NoResize |
	ImGuiWindowFlags_AlwaysAutoResize |
	ImGuiWindowFlags_NoSavedSettings |
	ImGuiWindowFlags_NoInputs |        // critical: no mouse capture
	ImGuiWindowFlags_NoFocusOnAppearing |
	ImGuiWindowFlags_NoNav;

// ── Sections ──────────────────────────────────────────────────────────

static void drawNetworkSection(float& yCursor)
{
	auto stats = maplecast_input_sink::getStats();

	// We don't have a direct RTT number from the input sink — use E2E EMA
	// as a proxy (it's the round trip from button press to TA frame
	// arrival, which IS the network RTT plus a small frame quantization).
	double rtt = stats.e2eEmaMs;
	double jitter = (stats.e2eMaxMs - stats.e2eMinMs) > 0
	              ? (stats.e2eMaxMs - stats.e2eMinMs) / 4.0  // rough p95-p50
	              : 0.0;
	double loss = 0.0;  // TODO: compute from input ACK gaps

	Grade g = computeGrade(rtt, jitter, loss);

	ImGui::SetNextWindowPos(ImVec2(8, yCursor), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.78f);
	ImGui::Begin("##hud_network", nullptr, HUD_WINDOW_FLAGS);

	ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "NETWORK");
	ImGui::Separator();

	// Grade — big, color-coded
	ImGui::Text("Grade:  ");
	ImGui::SameLine();
	ImGui::TextColored(g.color, "%s", g.letter);

	// Server identity (primary + backup)
	if (stats.onBackupServer) {
		ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "[FAILOVER ACTIVE]");
	}
	if (stats.hasBackup) {
		ImGui::Text("Standby: ready");
	}

	// Numerics
	if (stats.e2eProbes > 0) {
		ImGui::Text("RTT:    %.1fms (last %.1fms)", rtt, stats.e2eLastMs);
		ImGui::Text("Range:  %.1f - %.1fms", stats.e2eMinMs, stats.e2eMaxMs);
	} else {
		ImGui::TextDisabled("RTT:    waiting for input...");
	}
	ImGui::Text("Send:   %u pps", stats.sendRateHz);
	if (stats.failovers > 0) {
		ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
		                   "Failovers: %llu",
		                   (unsigned long long)stats.failovers);
	}

	float h = ImGui::GetWindowHeight();
	ImGui::End();
	yCursor += h + 4;
}

static void drawLatencySection(float& yCursor)
{
	auto stats = maplecast_input_sink::getStats();

	ImGui::SetNextWindowPos(ImVec2(8, yCursor), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.78f);
	ImGui::Begin("##hud_latency", nullptr, HUD_WINDOW_FLAGS);

	ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "LATENCY");
	ImGui::Separator();

	if (stats.e2eProbes > 0) {
		// E2E breakdown (we only know total — break it down by knowledge)
		ImGui::Text("E2E:    %.1fms", stats.e2eLastMs);
		ImGui::Text("EMA:    %.1fms", stats.e2eEmaMs);
		ImGui::Text("Probes: %llu", (unsigned long long)stats.e2eProbes);
	} else {
		ImGui::TextDisabled("Press a button to start measuring...");
	}

	ImGui::Text("Sent:   %llu (+%llu redundant)",
		(unsigned long long)stats.packetsSent,
		(unsigned long long)stats.redundantSends);

	float h = ImGui::GetWindowHeight();
	ImGui::End();
	yCursor += h + 4;
}

static void drawInputSection(float& yCursor)
{
	// TODO: read kcode[] history (last 30 frames) and render a horizontal
	// strip showing button presses over time. For now just show the bare
	// counters.
	auto stats = maplecast_input_sink::getStats();

	ImGui::SetNextWindowPos(ImVec2(8, yCursor), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.78f);
	ImGui::Begin("##hud_input", nullptr, HUD_WINDOW_FLAGS);

	ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "INPUT");
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
	// Only draw in client mode — no point in server mode (no display)
	if (!maplecast_mirror::isClient()) return;

	// Cursor in screen-space pixels, top-down placement
	float y = 8.0f;

	if (_showNetwork.load(std::memory_order_relaxed)) drawNetworkSection(y);
	if (_showLatency.load(std::memory_order_relaxed)) drawLatencySection(y);
	if (_showInput.load(std::memory_order_relaxed))   drawInputSection(y);

	// Phase 8: Note highway (combo trainer). Draws at the bottom-center.
	// Cheap no-op when no combo is active.
	note_highway::draw();
}

void toggleNetwork() { _showNetwork.store(!_showNetwork.load()); }
void toggleLatency() { _showLatency.store(!_showLatency.load()); }
void toggleInput()   { _showInput.store(!_showInput.load()); }

void toggleAll()
{
	bool any = _showNetwork.load() || _showLatency.load() || _showInput.load();
	bool newState = !any;
	_showNetwork.store(newState);
	_showLatency.store(newState);
	_showInput.store(newState);
}

} // namespace gui_competitive_hud
