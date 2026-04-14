/*
	Note Highway — implementation.

	For the MVP we draw 6 vertical lanes at the bottom-center of the
	screen, one per face button (X A B Y plus L/R triggers). Upcoming
	inputs scroll DOWNWARD from the top of the highway toward the hit
	zone at the bottom. When a note crosses the hit zone, the player
	should have pressed the matching button — frame proximity
	determines score tier.

	Data flow:
	  replay_reader holds the loaded .mccombo/.mcrec input log.
	  We scan the upcoming N entries (within ~2 seconds of current
	  frame), diff against previous state to find button-press edges,
	  and draw them as colored rectangles on the highway.
	  Player's own input comes through onPlayerInput() (called from
	  input_sink whenever a button changes).
*/
#include "note_highway.h"

#include <atomic>
#include <array>
#include <cstdio>
#include <mutex>
#include <vector>

#include <imgui.h>

#include "network/replay_reader.h"
#include "network/maplecast_mirror.h"

namespace note_highway
{

// ── Config ────────────────────────────────────────────────────────────

// Lanes map Dreamcast button bits → visual column.
// MVC2 typical mapping: A/B/C = LP/MP/HP, X/Y/Z = LK/MK/HK (6 face buttons).
// DC bit layout (active-low on the wire, but we flip when scanning):
//   DC_BTN_A = 0x04, DC_BTN_B = 0x02, DC_BTN_X = 0x0400, DC_BTN_Y = 0x0200,
//   DC_BTN_C = 0x0008, DC_BTN_Z = 0x0800.
struct LaneDef {
    const char* label;
    uint16_t    mask;     // DC bit mask (active-HIGH after flipping)
    ImU32       color;
};

static const LaneDef LANES[] = {
    {"LP", 0x0004, IM_COL32(255, 100, 100, 255)},  // A = Light Punch (red)
    {"MP", 0x0002, IM_COL32(255, 200, 100, 255)},  // B = Medium Punch (orange)
    {"HP", 0x0200, IM_COL32(255, 255, 100, 255)},  // Y = Heavy Punch (yellow)
    {"LK", 0x0400, IM_COL32(100, 255, 100, 255)},  // X = Light Kick (green)
    {"MK", 0x0008, IM_COL32(100, 200, 255, 255)},  // C = Medium Kick (blue)
    {"HK", 0x0800, IM_COL32(200, 100, 255, 255)},  // Z = Heavy Kick (purple)
};
static const int NUM_LANES = sizeof(LANES) / sizeof(LANES[0]);

// Lookahead window — we show notes up to this many frames in the future
static const int LOOKAHEAD_FRAMES = 120;  // 2 seconds at 60 fps

// Hit zone tolerance (frame distance from hit line to current frame)
static const int PERFECT_FRAMES = 2;   // ±2 frames
static const int GREAT_FRAMES   = 5;   // ±5 frames
static const int GOOD_FRAMES    = 10;  // ±10 frames

// Visual config
static const float HIGHWAY_WIDTH   = 420.0f;
static const float LANE_WIDTH      = HIGHWAY_WIDTH / (float)6;  // 70px per lane
static const float HIGHWAY_HEIGHT  = 240.0f;
static const float NOTE_HEIGHT     = 20.0f;
static const float HIT_ZONE_Y      = HIGHWAY_HEIGHT - 30.0f;

// ── State ─────────────────────────────────────────────────────────────

static std::atomic<bool> _active{false};
static std::mutex        _scoreMtx;
static Score             _score{};

// Previous button state (active-high format) — used to detect press edges
static uint16_t _prevExpectedButtons = 0;

// For scoring: the list of expected-note events we've seen but not yet
// "landed" (player hasn't responded). Pruned when they age past GOOD_FRAMES.
struct PendingNote {
    uint64_t frame;   // target frame
    uint16_t mask;    // which button mask triggers this note
};
static std::vector<PendingNote> _pending;
static std::mutex _pendingMtx;

// ── Helpers ───────────────────────────────────────────────────────────

// Flip DC active-low buttons to active-high (1 = pressed) so bitmasks
// feel natural in the UI code.
static inline uint16_t toActiveHigh(uint16_t dc_active_low) {
    return (uint16_t)~dc_active_low;
}

// ── Main draw ─────────────────────────────────────────────────────────

void draw() {
    if (!_active.load(std::memory_order_relaxed)) return;
    if (!maplecast_replay::isOpen()) return;

    const ImGuiIO& io = ImGui::GetIO();
    float screenW = io.DisplaySize.x;
    float screenH = io.DisplaySize.y;

    // Bottom-center positioning
    float highwayX = (screenW - HIGHWAY_WIDTH) * 0.5f;
    float highwayY = screenH - HIGHWAY_HEIGHT - 40.0f;

    ImGui::SetNextWindowPos(ImVec2(highwayX, highwayY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(HIGHWAY_WIDTH, HIGHWAY_HEIGHT + 30.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.65f);

    ImGui::Begin("##note_highway", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();

    // ── Draw lane backgrounds + labels ──
    for (int i = 0; i < NUM_LANES; i++) {
        float x = origin.x + i * LANE_WIDTH;
        dl->AddRectFilled(
            ImVec2(x + 2, origin.y),
            ImVec2(x + LANE_WIDTH - 2, origin.y + HIGHWAY_HEIGHT),
            IM_COL32(20, 20, 30, 120));
        // Label at top
        dl->AddText(
            ImVec2(x + LANE_WIDTH * 0.5f - 8, origin.y + 4),
            IM_COL32(150, 150, 150, 200),
            LANES[i].label);
    }

    // ── Draw hit zone line ──
    dl->AddLine(
        ImVec2(origin.x, origin.y + HIT_ZONE_Y),
        ImVec2(origin.x + HIGHWAY_WIDTH, origin.y + HIT_ZONE_Y),
        IM_COL32(255, 255, 255, 180),
        2.0f);

    // ── Scan input log ahead for notes ──
    uint64_t curFrame = maplecast_mirror::currentFrame();
    const auto& inputLog = maplecast_replay::info();  // meta
    (void)inputLog;

    // We need raw access to the log entries. The reader doesn't expose
    // them directly — for MVP, we'll scan via a new accessor added to
    // replay_reader. For now the visualization is placeholder.
    // TODO: add maplecast_replay::peekUpcoming(startFrame, nFrames) that
    // returns the input events in that window.

    // ── Score display ──
    Score s;
    { std::lock_guard<std::mutex> lk(_scoreMtx); s = _score; }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + HIGHWAY_HEIGHT + 4));
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f),
        "PERFECT %u", s.perfect);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.7f, 1.0f),
        "GREAT %u", s.great);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f),
        "GOOD %u", s.good);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
        "MISS %u", s.miss);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
        "STREAK %u (best %u)", s.streak, s.best_streak);

    ImGui::End();
}

// ── Public API ────────────────────────────────────────────────────────

void toggle() {
    _active.store(!_active.load(std::memory_order_relaxed), std::memory_order_relaxed);
}
bool isActive() { return _active.load(std::memory_order_relaxed); }

void onPlayerInput(uint16_t buttons, uint8_t /*lt*/, uint8_t /*rt*/) {
    if (!_active.load(std::memory_order_relaxed)) return;

    // Convert DC active-low to active-high
    uint16_t hi = toActiveHigh(buttons);
    uint16_t prev = _prevExpectedButtons;
    uint16_t pressed = hi & ~prev;  // 1→0 edges (new presses)
    _prevExpectedButtons = hi;

    if (pressed == 0) return;

    uint64_t curFrame = maplecast_mirror::currentFrame();

    // For each pending note that the player's press matches, compute
    // tier based on frame distance.
    std::lock_guard<std::mutex> lk(_pendingMtx);
    std::lock_guard<std::mutex> sl(_scoreMtx);
    for (auto it = _pending.begin(); it != _pending.end(); ) {
        if ((it->mask & pressed) == 0) { ++it; continue; }

        int64_t dist = (int64_t)curFrame - (int64_t)it->frame;
        int64_t absDist = dist < 0 ? -dist : dist;

        if (absDist <= PERFECT_FRAMES) {
            _score.perfect++;
            _score.streak++;
        } else if (absDist <= GREAT_FRAMES) {
            _score.great++;
            _score.streak++;
        } else if (absDist <= GOOD_FRAMES) {
            _score.good++;
            _score.streak++;
        } else {
            // Shouldn't happen — pruning should've removed this
            _score.miss++;
            _score.streak = 0;
        }
        if (_score.streak > _score.best_streak)
            _score.best_streak = _score.streak;

        it = _pending.erase(it);
        break;  // one press consumes one pending note
    }
}

Score currentScore() {
    std::lock_guard<std::mutex> lk(_scoreMtx);
    return _score;
}

void resetScore() {
    std::lock_guard<std::mutex> lk(_scoreMtx);
    _score = {};
}

} // namespace note_highway
