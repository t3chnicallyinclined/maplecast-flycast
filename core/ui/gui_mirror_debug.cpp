/*
	MapleCast Mirror Client — Debug Overlay.

	See header for the split-function rationale. This TU owns the draw
	body (drawContent()), a telemetry history for the scrolling graphs,
	and the visibility toggle state. It reaches into the other MapleCast
	subsystems via their public APIs only:

	  - maplecast_mirror::getClientStats()         // video WS + decode stats
	  - maplecast_mirror::requestClientVideoReconnect()
	  - maplecast_audio_client::getStats()          // audio WS stats
	  - maplecast_audio_client::setEnabled()
	  - maplecast_audio_client::requestReconnect()
	  - maplecast_input::getLatchPolicy/setLatchPolicy
	  - maplecast_input::getGuardUs/setGuardUs
	  - config::AudioVolume                         // shared volume slider
*/
#include "gui_mirror_debug.h"
#include "network/maplecast_mirror.h"
#include "network/maplecast_audio_client.h"
#include "network/maplecast_input_server.h"
#include "audio/audiostream.h"
#include "cfg/option.h"

#include <imgui.h>

#include <atomic>
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace gui_mirror_debug
{

// ---- Visibility ----
static std::atomic<bool> _visible{false};

bool isVisible() { return _visible.load(std::memory_order_relaxed); }
void setVisible(bool v) { _visible.store(v, std::memory_order_relaxed); }
void toggleVisible() { _visible.store(!_visible.load(std::memory_order_relaxed), std::memory_order_relaxed); }

// ---- Telemetry history for the graphs ----
//
// We keep fixed-size ring buffers of the most recent samples. ImGui's
// PlotLines can consume a callback that wraps around for us, which saves
// a memmove per frame. 240 samples at ~60 Hz = 4 seconds of history.
static constexpr int HISTORY_LEN = 240;

struct History {
	std::array<float, HISTORY_LEN> samples{};
	int writeIdx = 0;
	int count = 0;

	void push(float v) {
		samples[writeIdx] = v;
		writeIdx = (writeIdx + 1) % HISTORY_LEN;
		if (count < HISTORY_LEN) count++;
	}

	// Callback compatible with ImGui::PlotLines. The caller passes the
	// array base and a starting offset; here we store the offset in the
	// `offset` arg to PlotLines so the plot draws in chronological order.
	static float getter(void* data, int idx) {
		History* h = static_cast<History*>(data);
		return h->samples[(h->writeIdx + idx) % HISTORY_LEN];
	}
};

static History _videoArrivalHistory;
static History _audioArrivalHistory;
static History _decodeHistory;

// ---- Log ring ----
// Bounded in-memory log the overlay can show without tailing stdout. Other
// subsystems can push lines via logLine().
static constexpr int LOG_LINES = 128;
static std::mutex _logMutex;
static std::array<std::string, LOG_LINES> _log;
static int _logWriteIdx = 0;
static int _logCount = 0;
static bool _logAutoScroll = true;

void logLine(const char* text)
{
	if (!text) return;
	std::lock_guard<std::mutex> lock(_logMutex);
	_log[_logWriteIdx] = text;
	_logWriteIdx = (_logWriteIdx + 1) % LOG_LINES;
	if (_logCount < LOG_LINES) _logCount++;
}

// ---- Snapshot tracking across frames ----
//
// Some derived metrics (packet rate, byte rate) need the previous snapshot
// to compute a delta. We keep a single previous snapshot here. It's updated
// once per draw call, which is good enough for a human-readable overlay.
struct PrevSnapshot {
	uint64_t packets = 0;
	uint64_t bytes = 0;
	uint64_t audioPackets = 0;
	uint64_t audioBytes = 0;
	double   lastWallClock = 0.0;
};
static PrevSnapshot _prev;

static double nowSeconds()
{
	return ImGui::GetTime();  // monotonic since ImGui context creation
}

// ---- Helpers ----

static const char* latchPolicyName(maplecast_input::LatchPolicy p)
{
	return p == maplecast_input::LatchPolicy::ConsistencyFirst ? "ConsistencyFirst" : "LatencyFirst";
}

static void drawLatchPolicyButtons(int slot)
{
	using maplecast_input::LatchPolicy;
	LatchPolicy cur = maplecast_input::getLatchPolicy(slot);

	ImGui::Text("Slot %d: %s", slot, latchPolicyName(cur));
	ImGui::SameLine();

	char btnLatency[32];
	char btnConsistency[32];
	snprintf(btnLatency, sizeof(btnLatency), "LatencyFirst##%d", slot);
	snprintf(btnConsistency, sizeof(btnConsistency), "ConsistencyFirst##%d", slot);

	if (cur == LatchPolicy::LatencyFirst)
		ImGui::BeginDisabled();
	if (ImGui::SmallButton(btnLatency))
		maplecast_input::setLatchPolicy(slot, LatchPolicy::LatencyFirst);
	if (cur == LatchPolicy::LatencyFirst)
		ImGui::EndDisabled();

	ImGui::SameLine();

	if (cur == LatchPolicy::ConsistencyFirst)
		ImGui::BeginDisabled();
	if (ImGui::SmallButton(btnConsistency))
		maplecast_input::setLatchPolicy(slot, LatchPolicy::ConsistencyFirst);
	if (cur == LatchPolicy::ConsistencyFirst)
		ImGui::EndDisabled();
}

// ---- Section drawers ----

static void drawConnectionSection(const maplecast_mirror::ClientStats& vs,
                                   const maplecast_audio_client::Stats& as)
{
	if (!ImGui::CollapsingHeader("Connection", ImGuiTreeNodeFlags_DefaultOpen))
		return;

	// Video WS status
	{
		ImGui::Text("Video WS:");
		ImGui::SameLine();
		if (vs.wsConnected) {
			ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "CONNECTED");
		} else {
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "DISCONNECTED");
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Reconnect Video")) {
			maplecast_mirror::requestClientVideoReconnect();
			logLine("[overlay] video reconnect requested");
		}
	}

	// Audio WS status
	{
		ImGui::Text("Audio WS:");
		ImGui::SameLine();
		if (as.connected) {
			ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "CONNECTED");
		} else {
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "DISCONNECTED");
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Reconnect Audio")) {
			maplecast_audio_client::requestReconnect();
			logLine("[overlay] audio reconnect requested");
		}
	}

	ImGui::Separator();
}

static void drawVideoSection(const maplecast_mirror::ClientStats& s)
{
	if (!ImGui::CollapsingHeader("Video telemetry", ImGuiTreeNodeFlags_DefaultOpen))
		return;

	// Rates (packets/sec, bytes/sec) — computed from delta since last draw.
	const double now = nowSeconds();
	double dt = now - _prev.lastWallClock;
	if (dt <= 0.0) dt = 1.0;  // first frame / clock hiccup — avoid divide-by-zero

	const double pps = (s.packetsReceived - _prev.packets) / dt;
	const double Bps = (s.bytesReceived   - _prev.bytes)   / dt;

	ImGui::Text("Frame: %llu   Packets: %llu   PPS: %.1f",
		(unsigned long long)s.frameCount,
		(unsigned long long)s.packetsReceived,
		pps);
	ImGui::Text("Bytes recv: %.2f MB   Rate: %.0f kB/s",
		s.bytesReceived / (1024.0 * 1024.0),
		Bps / 1024.0);
	ImGui::Text("Arrival interval: %.2f ms EMA, %.2f ms peak",
		s.arrivalEmaUs / 1000.0,
		s.arrivalMaxUs / 1000.0);
	ImGui::Text("Decode: %.2f ms last, %.2f ms EMA",
		s.lastDecodeUs / 1000.0,
		s.decodeEmaUs  / 1000.0);
	ImGui::Text("Last frame: TA %u B, dirty %u pages, vram %s",
		s.lastTaSize, s.lastDirtyPages, s.lastVramDirty ? "dirty" : "clean");

	if (ImGui::SmallButton("Reset peaks##video")) {
		maplecast_mirror::resetClientStatsPeaks();
		logLine("[overlay] video peaks reset");
	}

	// Plots — arrival interval and decode time, both in milliseconds
	_videoArrivalHistory.push((float)(s.arrivalEmaUs / 1000.0));
	_decodeHistory.push((float)(s.lastDecodeUs / 1000.0));

	ImGui::PlotLines("Arrival (ms)",
		&History::getter, &_videoArrivalHistory,
		_videoArrivalHistory.count, 0,
		nullptr, 0.0f, 40.0f, ImVec2(0, 60));

	ImGui::PlotLines("Decode (ms)",
		&History::getter, &_decodeHistory,
		_decodeHistory.count, 0,
		nullptr, 0.0f, 10.0f, ImVec2(0, 60));

	ImGui::Separator();
}

static void drawAudioSection(const maplecast_audio_client::Stats& s)
{
	if (!ImGui::CollapsingHeader("Audio telemetry", ImGuiTreeNodeFlags_DefaultOpen))
		return;

	const double now = nowSeconds();
	double dt = now - _prev.lastWallClock;
	if (dt <= 0.0) dt = 1.0;
	const double pps = (s.packetsReceived - _prev.audioPackets) / dt;
	const double Bps = (s.bytesReceived   - _prev.audioBytes)   / dt;

	ImGui::Text("Packets: %llu   PPS: %.1f   Seq: %u",
		(unsigned long long)s.packetsReceived, pps, s.lastSeq);
	ImGui::Text("Dropped: %llu   Push fails: %llu",
		(unsigned long long)s.packetsDropped,
		(unsigned long long)s.pushFailures);
	ImGui::Text("Bytes: %.2f MB   Rate: %.0f kB/s",
		s.bytesReceived / (1024.0 * 1024.0),
		Bps / 1024.0);
	ImGui::Text("Arrival interval: %.2f ms EMA, %.2f ms peak",
		s.arrivalIntervalEmaUs / 1000.0,
		s.arrivalIntervalMaxUs / 1000.0);

	bool enabled = maplecast_audio_client::isEnabled();
	if (ImGui::Checkbox("Enable audio receive", &enabled)) {
		maplecast_audio_client::setEnabled(enabled);
		logLine(enabled ? "[overlay] audio receive ENABLED"
		                : "[overlay] audio receive DISABLED");
	}

	// Local playback volume. config::AudioVolume is an integer option
	// measured in dB-ish units; we expose it as a raw slider. Any value
	// change is picked up by WriteSample on the server emu thread, but
	// on the client PushExternalAudio doesn't apply gain, so this slider
	// is effective via AudioBackend's downstream driver mixer.
	int volume = config::AudioVolume.get();
	if (ImGui::SliderInt("Volume", &volume, 0, 100)) {
		config::AudioVolume.set(volume);
	}

	if (ImGui::SmallButton("Reset peaks##audio")) {
		maplecast_audio_client::resetPeaks();
		logLine("[overlay] audio peaks reset");
	}

	_audioArrivalHistory.push((float)(s.arrivalIntervalEmaUs / 1000.0));
	ImGui::PlotLines("Audio arrival (ms)",
		&History::getter, &_audioArrivalHistory,
		_audioArrivalHistory.count, 0,
		nullptr, 0.0f, 40.0f, ImVec2(0, 60));

	ImGui::Separator();
}

static void drawInputSection()
{
	if (!ImGui::CollapsingHeader("Input latch"))
		return;

	// In mirror client mode, only show the local player's slot.
	// On the server, show both.
	if (maplecast_mirror::isClient()) {
		// TODO: read actual local slot from MAPLECAST_PLAYER_SLOT env
		int localSlot = 0;
		if (const char* s = std::getenv("MAPLECAST_PLAYER_SLOT"))
			localSlot = std::atoi(s);
		drawLatchPolicyButtons(localSlot);
	} else {
		drawLatchPolicyButtons(0);
		drawLatchPolicyButtons(1);
	}

	int64_t guardUs = maplecast_input::getGuardUs();
	int guardInt = (int)guardUs;
	if (ImGui::SliderInt("Guard window (µs)", &guardInt, 0, 2000))
		maplecast_input::setGuardUs((int64_t)guardInt);

	ImGui::TextDisabled("Guard window only affects ConsistencyFirst slots.");

	ImGui::Separator();
}

// ---- Settings section ----
//
// Video and audio output knobs that are safe to change at runtime in
// mirror-client mode. The full flycast settings menu has many more options
// (GGPO netplay, BIOS paths, emulator timings, renderer backend, etc.) but
// most of them either make no sense on a client that isn't running the
// emulator, or would break the mirror pipeline if toggled.
//
// Load-bearing overrides from emulator.cpp:1077-1091 that we deliberately
// DO NOT expose:
//   - ThreadedRendering (must stay off — clientReceive runs on the render
//     thread)
//   - Fog, ModifierVolumes, UseMipmaps, PerPixelLayers (performance
//     safety rails for the client path)
//   - RendererType (changing the GL/Vulkan backend needs a full renderer
//     teardown + re-init that the mirror client isn't set up for)
//   - VSync (we force it off at runtime for render-loop pacing — see
//     mainui.cpp SwapInterval override)
//   - AudioBackend (we force "pulse" to dodge SDL2's stateless resampler
//     crackle on PipeWire hosts). Backend switching IS safe once the
//     emulator is running — see the Audio subsection below which exposes
//     it anyway for users who want to override.

// ==== Presets (matching WASM panel) ====
static void applyPresetPerformance()
{
	config::RenderResolution = 480;
	config::PerPixelLayers = 4;
	config::Fog = false;
	config::ModifierVolumes = false;
	config::AnisotropicFiltering = 1;
	config::TextureFiltering = 0;
	config::UseMipmaps = false;
	gui_mirror_debug::logLine("[preset] Max Performance applied");
}

static void applyPresetArcade()
{
	config::RenderResolution = 960;
	config::PerPixelLayers = 8;
	config::Fog = true;
	config::ModifierVolumes = true;
	config::AnisotropicFiltering = 4;
	config::TextureFiltering = 0;
	config::UseMipmaps = false;
	gui_mirror_debug::logLine("[preset] Arcade applied");
}

static void applyPresetMaxQuality()
{
	config::RenderResolution = 1920;
	config::PerPixelLayers = 32;
	config::Fog = true;
	config::ModifierVolumes = true;
	config::AnisotropicFiltering = 16;
	config::TextureFiltering = 2;  // linear
	config::UseMipmaps = true;
	gui_mirror_debug::logLine("[preset] Max Quality applied");
}

static void drawSettingsSection()
{
	if (!ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen))
		return;

	// ==== Presets ====
	ImGui::Text("Presets:");
	ImGui::SameLine();
	if (ImGui::SmallButton("Performance"))
		applyPresetPerformance();
	ImGui::SameLine();
	if (ImGui::SmallButton("Arcade"))
		applyPresetArcade();
	ImGui::SameLine();
	if (ImGui::SmallButton("Max Quality"))
		applyPresetMaxQuality();
	ImGui::Separator();

	// ==== Video ====
	if (ImGui::TreeNodeEx("Video", ImGuiTreeNodeFlags_DefaultOpen))
	{
		// Render resolution. Mirror the set of choices from
		// settings_video.cpp around line 155 — these are the native render
		// resolutions flycast supports. RenderResolution is an int that
		// represents the vertical pixel count (e.g. 480, 960, 1440, 1920).
		static const int RES_CHOICES[] = { 480, 720, 960, 1080, 1440, 1920, 2160 };
		static const char* RES_LABELS[] = {
			"480p (native)", "720p", "960p (2x)", "1080p",
			"1440p (3x)", "1920p (4x)", "2160p (4.5x)"
		};
		int cur = config::RenderResolution.get();
		int selected = 0;
		for (size_t i = 0; i < sizeof(RES_CHOICES)/sizeof(int); i++) {
			if (RES_CHOICES[i] == cur) { selected = (int)i; break; }
		}
		if (ImGui::Combo("Resolution", &selected, RES_LABELS,
			sizeof(RES_LABELS)/sizeof(const char*)))
		{
			config::RenderResolution = RES_CHOICES[selected];
			logLine("[overlay] render resolution changed");
		}

		bool widescreen = config::Widescreen.get();
		if (ImGui::Checkbox("Widescreen (16:9)", &widescreen))
			config::Widescreen.set(widescreen);

		{
			// Super-widescreen and screen stretching are grayed out if
			// Widescreen is off — matches flycast's own settings UI.
			bool disabled = !config::Widescreen.get();
			if (disabled) ImGui::BeginDisabled();
			bool superWide = config::SuperWidescreen.get();
			if (ImGui::Checkbox("Super Widescreen", &superWide))
				config::SuperWidescreen.set(superWide);
			if (disabled) ImGui::EndDisabled();
		}

		int stretch = config::ScreenStretching.get();
		if (ImGui::SliderInt("H. stretch (%)", &stretch, 100, 250))
			config::ScreenStretching.set(stretch);

		bool integerScale = config::IntegerScale.get();
		if (ImGui::Checkbox("Integer scaling", &integerScale))
			config::IntegerScale.set(integerScale);

		bool linear = config::LinearInterpolation.get();
		if (ImGui::Checkbox("Linear interpolation", &linear))
			config::LinearInterpolation.set(linear);

		bool rotate = config::Rotate90.get();
		if (ImGui::Checkbox("Rotate 90°", &rotate))
			config::Rotate90.set(rotate);

		// Texture filtering — 0=default, 1=nearest, 2=linear
		int texFilter = config::TextureFiltering.get();
		if (ImGui::Combo("Texture filter", &texFilter,
			"Default\0Nearest\0Linear\0"))
		{
			config::TextureFiltering.set(texFilter);
		}

		// Anisotropic filtering — flycast allows 1, 2, 4, 8, 16
		static const int ANISO_CHOICES[] = { 1, 2, 4, 8, 16 };
		static const char* ANISO_LABELS[] = { "Off", "2x", "4x", "8x", "16x" };
		int curAniso = config::AnisotropicFiltering.get();
		int anisoSel = 0;
		for (size_t i = 0; i < sizeof(ANISO_CHOICES)/sizeof(int); i++) {
			if (ANISO_CHOICES[i] == curAniso) { anisoSel = (int)i; break; }
		}
		if (ImGui::Combo("Anisotropic", &anisoSel, ANISO_LABELS,
			sizeof(ANISO_LABELS)/sizeof(const char*)))
		{
			config::AnisotropicFiltering.set(ANISO_CHOICES[anisoSel]);
		}

		// Renderer quality options (matching WASM panel)
		bool fog = config::Fog.get();
		if (ImGui::Checkbox("Fog", &fog))
			config::Fog.set(fog);

		bool modVol = config::ModifierVolumes.get();
		if (ImGui::Checkbox("Modifier volumes", &modVol))
			config::ModifierVolumes.set(modVol);

		bool mipmaps = config::UseMipmaps.get();
		if (ImGui::Checkbox("Mipmaps", &mipmaps))
			config::UseMipmaps.set(mipmaps);

		// Transparency layers — 4, 8, 16, 32
		static const int LAYER_CHOICES[] = { 4, 8, 16, 32 };
		static const char* LAYER_LABELS[] = { "4", "8", "16", "32" };
		int curLayers = config::PerPixelLayers.get();
		int layerSel = 0;
		for (size_t i = 0; i < sizeof(LAYER_CHOICES)/sizeof(int); i++) {
			if (LAYER_CHOICES[i] == curLayers) { layerSel = (int)i; break; }
		}
		if (ImGui::Combo("Transparency layers", &layerSel, LAYER_LABELS,
			sizeof(LAYER_LABELS)/sizeof(const char*)))
		{
			config::PerPixelLayers.set(LAYER_CHOICES[layerSel]);
		}

		ImGui::Separator();
		ImGui::TextDisabled("Post-processing (CRT, bloom, scanlines)");
		ImGui::TextDisabled("coming soon — requires custom GLSL shaders");

		ImGui::Separator();
		bool showFps = config::ShowFPS.get();
		if (ImGui::Checkbox("Show FPS counter", &showFps))
			config::ShowFPS.set(showFps);

		ImGui::TreePop();
	}

	// ==== Audio ====
	if (ImGui::TreeNodeEx("Audio", ImGuiTreeNodeFlags_DefaultOpen))
	{
		// Audio backend dropdown. Shows the backend flycast currently has
		// selected and lets the user switch at runtime. Switching calls
		// TermAudio() + InitAudio() via a one-shot action button so we
		// don't accidentally tear down playback mid-slider-drag.
		const std::string cur = config::AudioBackend.get();
		ImGui::Text("Backend (current): %s", cur.c_str());

		// Pre-fill with the likely-available backends. "null" is always
		// registered; others depend on compile flags.
		static const char* BACKENDS[] = { "pulse", "alsa", "sdl2", "libao", "null" };
		static int backendSel = 0;
		// Sync the selector with whatever's currently selected on first draw
		static bool backendSelInit = false;
		if (!backendSelInit) {
			for (int i = 0; i < (int)(sizeof(BACKENDS)/sizeof(const char*)); i++) {
				if (cur == BACKENDS[i]) { backendSel = i; break; }
			}
			backendSelInit = true;
		}
		ImGui::Combo("Switch to", &backendSel, BACKENDS,
			sizeof(BACKENDS)/sizeof(const char*));
		ImGui::SameLine();
		if (ImGui::SmallButton("Apply##backend"))
		{
			// Teardown + re-init in place. The mirror client's audio
			// receive thread notices the new backend via its next push
			// call because PushExternalAudio reads currentBackend fresh.
			config::AudioBackend.set(BACKENDS[backendSel]);
			TermAudio();
			InitAudio();
			logLine("[overlay] audio backend switched");
		}
		ImGui::TextDisabled("Switch triggers TermAudio + InitAudio.");

		ImGui::TreePop();
	}

	ImGui::Separator();
}

static void drawLogSection()
{
	if (!ImGui::CollapsingHeader("Log"))
		return;

	ImGui::Checkbox("Autoscroll", &_logAutoScroll);
	ImGui::SameLine();
	if (ImGui::SmallButton("Clear")) {
		std::lock_guard<std::mutex> lock(_logMutex);
		_logWriteIdx = 0;
		_logCount = 0;
		for (auto& l : _log) l.clear();
	}

	ImGui::BeginChild("##log_scroll", ImVec2(0, 180), true,
		ImGuiWindowFlags_HorizontalScrollbar);

	{
		std::lock_guard<std::mutex> lock(_logMutex);
		const int start = _logCount < LOG_LINES ? 0 : _logWriteIdx;
		for (int i = 0; i < _logCount; i++) {
			const int idx = (start + i) % LOG_LINES;
			ImGui::TextUnformatted(_log[idx].c_str());
		}
	}

	if (_logAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
		ImGui::SetScrollHereY(1.0f);

	ImGui::EndChild();
}

// ---- Public entry point ----

void drawContent()
{
	// Pull one consistent-ish snapshot of both telemetry sources at the
	// top of the frame. These are cheap atomic reads.
	const auto vs = maplecast_mirror::getClientStats();
	const auto as = maplecast_audio_client::getStats();

	// Position the window on the top-right by default. User can drag it.
	const ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 420.0f, 20.0f),
	                         ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(400.0f, 0.0f),
	                          ImGuiCond_FirstUseEver);

	ImGui::Begin("MapleCast Settings (F1 to toggle)",
		nullptr,
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

	drawConnectionSection(vs, as);
	drawVideoSection(vs);
	drawAudioSection(as);
	drawInputSection();
	drawSettingsSection();
	drawLogSection();

	ImGui::End();

	// Update previous snapshot for rate calculations on next frame.
	_prev.packets        = vs.packetsReceived;
	_prev.bytes          = vs.bytesReceived;
	_prev.audioPackets   = as.packetsReceived;
	_prev.audioBytes     = as.bytesReceived;
	_prev.lastWallClock  = nowSeconds();
}

} // namespace gui_mirror_debug
