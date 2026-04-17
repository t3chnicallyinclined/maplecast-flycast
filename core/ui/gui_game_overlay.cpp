/*
	Game Data Overlay — training-mode style input display.

	Vertical scrolling input log: newest input at bottom, scrolls upward.
	Each row shows: [stick direction] [LP MP HP LK MK HK] [A1 A2]
	Buttons light up as circles when pressed. Stick shown as numpad notation.

	Style matches fighting game training mode overlays (like the screenshot
	reference with inputs streaming upward).
*/
#include "gui_game_overlay.h"
#include "network/maplecast_mirror.h"
#include "network/maplecast_gamestate.h"
#include "input/gamepad.h"

#include <imgui.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace gui_game_overlay
{

static std::atomic<bool> _showGameData{false};
static std::atomic<bool> _showInput{true};  // on by default

// ── Input history ring buffer ────────────────────────────────────────
static constexpr int HISTORY_SIZE = 120;
struct InputFrame {
	uint16_t buttons;
	uint8_t  lt, rt;
};
static InputFrame _history[HISTORY_SIZE];
static int _historyHead = 0;
static std::mutex _historyMtx;
static uint32_t _totalInputFrames = 0;

// ── Numpad stick notation ────────────────────────────────────────────
// Fighting game standard: 5=neutral, 6=forward, 2=down, etc.
// We show as arrow characters.
static const char* stickNotation(uint16_t buttons)
{
	bool up    = !(buttons & DC_DPAD_UP);
	bool down  = !(buttons & DC_DPAD_DOWN);
	bool left  = !(buttons & DC_DPAD_LEFT);
	bool right = !(buttons & DC_DPAD_RIGHT);

	if (up && left)    return "7";
	if (up && right)   return "9";
	if (up)            return "8";
	if (down && left)  return "1";
	if (down && right) return "3";
	if (down)          return "2";
	if (left)          return "4";
	if (right)         return "6";
	return "5";
}

// Arrow character for stick direction
static const char* stickArrow(uint16_t buttons)
{
	bool up    = !(buttons & DC_DPAD_UP);
	bool down  = !(buttons & DC_DPAD_DOWN);
	bool left  = !(buttons & DC_DPAD_LEFT);
	bool right = !(buttons & DC_DPAD_RIGHT);

	if (up && left)    return "\xe2\x86\x96"; // ↖
	if (up && right)   return "\xe2\x86\x97"; // ↗
	if (up)            return "\xe2\x86\x91"; // ↑
	if (down && left)  return "\xe2\x86\x99"; // ↙
	if (down && right) return "\xe2\x86\x98"; // ↘
	if (down)          return "\xe2\x86\x93"; // ↓
	if (left)          return "\xe2\x86\x90"; // ←
	if (right)         return "\xe2\x86\x92"; // →
	return "\xc2\xb7";  // · (neutral)
}

// ── Small filled circle for button indicators ────────────────────────
static void drawDot(ImDrawList* dl, float cx, float cy, float r,
                     bool pressed, ImU32 activeColor)
{
	if (pressed) {
		dl->AddCircleFilled(ImVec2(cx, cy), r, activeColor, 12);
		dl->AddCircle(ImVec2(cx, cy), r, IM_COL32(255, 255, 255, 200), 12, 1.5f);
	} else {
		dl->AddCircleFilled(ImVec2(cx, cy), r, IM_COL32(40, 40, 50, 100), 12);
	}
}

// ── Current input state (big, at bottom) ─────────────────────────────
static void drawCurrentInput(ImDrawList* dl, ImVec2 wp, float winW,
                              const InputFrame& cur)
{
	bool up    = !(cur.buttons & DC_DPAD_UP);
	bool down  = !(cur.buttons & DC_DPAD_DOWN);
	bool left  = !(cur.buttons & DC_DPAD_LEFT);
	bool right = !(cur.buttons & DC_DPAD_RIGHT);
	bool a = !(cur.buttons & DC_BTN_A);
	bool b = !(cur.buttons & DC_BTN_B);
	bool c = !(cur.buttons & DC_BTN_C);
	bool x = !(cur.buttons & DC_BTN_X);
	bool y = !(cur.buttons & DC_BTN_Y);
	bool z = !(cur.buttons & DC_BTN_Z);

	float r = 11.0f;
	float gap = 5.0f;
	float sp = r * 2 + gap;

	// Stick (left side)
	float stickR = 16.0f;
	float sx = wp.x + 28;
	float sy = wp.y + 10;
	dl->AddCircleFilled(ImVec2(sx, sy), stickR, IM_COL32(30, 30, 40, 180), 16);
	dl->AddCircle(ImVec2(sx, sy), stickR, IM_COL32(80, 80, 100, 200), 16);
	float dx = 0, dy = 0;
	if (left)  dx -= 1;
	if (right) dx += 1;
	if (up)    dy -= 1;
	if (down)  dy += 1;
	if (dx != 0 || dy != 0) {
		float len = sqrtf(dx * dx + dy * dy);
		dx = dx / len * stickR * 0.6f;
		dy = dy / len * stickR * 0.6f;
		dl->AddCircleFilled(ImVec2(sx + dx, sy + dy), stickR * 0.4f,
		                    IM_COL32(80, 180, 255, 255), 10);
	}

	// Buttons
	ImU32 colLP = IM_COL32(80, 160, 255, 230);
	ImU32 colMP = IM_COL32(60, 120, 220, 230);
	ImU32 colHP = IM_COL32(40, 80, 200, 230);
	ImU32 colLK = IM_COL32(255, 140, 60, 230);
	ImU32 colMK = IM_COL32(230, 100, 40, 230);
	ImU32 colHK = IM_COL32(200, 60, 30, 230);
	ImU32 colA  = IM_COL32(160, 80, 220, 230);

	float bx = wp.x + 60;
	float by = sy - r;
	drawDot(dl, bx,        by,      r, a, colLP);
	drawDot(dl, bx + sp,   by,      r, b, colMP);
	drawDot(dl, bx + sp*2, by,      r, c, colHP);
	drawDot(dl, bx,        by + sp, r, x, colLK);
	drawDot(dl, bx + sp,   by + sp, r, y, colMK);
	drawDot(dl, bx + sp*2, by + sp, r, z, colHK);

	// Assists
	float ax = bx + sp * 3 + 8;
	drawDot(dl, ax, by,      r * 0.8f, cur.lt > 30, colA);
	drawDot(dl, ax, by + sp, r * 0.8f, cur.rt > 30, colA);

	// Labels
	ImU32 lblCol = IM_COL32(180, 180, 200, 200);
	auto label = [&](float cx2, float cy2, const char* txt) {
		ImVec2 ts = ImGui::CalcTextSize(txt);
		dl->AddText(ImVec2(cx2 - ts.x/2, cy2 - ts.y/2), lblCol, txt);
	};
	label(bx,        by - r - 6, "LP");
	label(bx + sp,   by - r - 6, "MP");
	label(bx + sp*2, by - r - 6, "HP");
	label(ax, by - r * 0.8f - 6, "A1");
	label(ax, by + sp + r * 0.8f + 4, "A2");
}

// ── Scrolling input log (vertical, newest at bottom) ─────────────────
static void drawInputLog(ImDrawList* dl, ImVec2 pos, float width, float height)
{
	float rowH = 14.0f;
	int visibleRows = (int)(height / rowH);
	float dotR = 4.0f;
	float colW = 12.0f;

	// Button column positions
	float stickX = pos.x + 16;
	float btnStartX = pos.x + 36;

	// Colors for the log dots (smaller, dimmer than current state)
	ImU32 colP = IM_COL32(80, 140, 255, 200);   // punch
	ImU32 colK = IM_COL32(240, 120, 50, 200);    // kick
	ImU32 colA = IM_COL32(140, 70, 200, 200);    // assist
	ImU32 colDir = IM_COL32(80, 200, 255, 220);  // direction text

	std::lock_guard<std::mutex> lk(_historyMtx);
	int available = (_totalInputFrames < HISTORY_SIZE)
	              ? _totalInputFrames : HISTORY_SIZE;
	if (available == 0) return;

	// Draw from bottom (newest) to top (oldest)
	int drawn = 0;
	for (int i = 0; i < visibleRows && i < available; i++) {
		int ringIdx = (_historyHead - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
		InputFrame& f = _history[ringIdx];

		float rowY = pos.y + height - (i + 1) * rowH;
		if (rowY < pos.y) break;

		// Separator line (subtle)
		if (i > 0)
			dl->AddLine(ImVec2(pos.x + 4, rowY + rowH),
			            ImVec2(pos.x + width - 4, rowY + rowH),
			            IM_COL32(60, 60, 80, 60));

		float cy = rowY + rowH * 0.5f;

		// Stick direction (arrow character)
		const char* arrow = stickArrow(f.buttons);
		bool hasDir = strcmp(arrow, "\xc2\xb7") != 0;
		dl->AddText(ImVec2(stickX - 4, cy - 6),
		            hasDir ? colDir : IM_COL32(50, 50, 60, 100), arrow);

		// Button dots: LP MP HP LK MK HK A1 A2
		float bx = btnStartX;
		drawDot(dl, bx,            cy, dotR, !(f.buttons & DC_BTN_A), colP);
		drawDot(dl, bx + colW,     cy, dotR, !(f.buttons & DC_BTN_B), colP);
		drawDot(dl, bx + colW * 2, cy, dotR, !(f.buttons & DC_BTN_C), colP);
		drawDot(dl, bx + colW * 3, cy, dotR, !(f.buttons & DC_BTN_X), colK);
		drawDot(dl, bx + colW * 4, cy, dotR, !(f.buttons & DC_BTN_Y), colK);
		drawDot(dl, bx + colW * 5, cy, dotR, !(f.buttons & DC_BTN_Z), colK);
		drawDot(dl, bx + colW * 6 + 4, cy, dotR * 0.8f, f.lt > 30, colA);
		drawDot(dl, bx + colW * 7 + 4, cy, dotR * 0.8f, f.rt > 30, colA);

		drawn++;
	}
}

// ── Draw entry points ────────────────────────────────────────────────

static void drawInputDisplay()
{
	InputFrame cur;
	{
		std::lock_guard<std::mutex> lk(_historyMtx);
		if (_totalInputFrames == 0) return;
		int idx = (_historyHead - 1 + HISTORY_SIZE) % HISTORY_SIZE;
		cur = _history[idx];
	}

	float dispW = ImGui::GetIO().DisplaySize.x;
	float dispH = ImGui::GetIO().DisplaySize.y;
	float panelW = 180;
	float panelH = dispH * 0.6f;

	ImGui::SetNextWindowPos(ImVec2(dispW - panelW - 8, dispH - panelH - 8),
	                        ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowBgAlpha(0.75f);

	bool showInp = true;
	ImGui::Begin("Input", &showInp,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoNav);
	if (!showInp) { _showInput.store(false); ImGui::End(); return; }

	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 wp = ImGui::GetWindowPos();
	float ww = ImGui::GetWindowWidth();
	float wh = ImGui::GetWindowHeight();

	// Current state (bottom of window, big buttons)
	float curAreaH = 40;
	ImVec2 curPos(wp.x, wp.y + wh - curAreaH - 8);
	drawCurrentInput(dl, curPos, ww, cur);

	// Separator
	dl->AddLine(ImVec2(wp.x + 8, wp.y + wh - curAreaH - 12),
	            ImVec2(wp.x + ww - 8, wp.y + wh - curAreaH - 12),
	            IM_COL32(80, 120, 200, 150));

	// Scrolling log (above current state)
	float logTop = wp.y + ImGui::GetFrameHeight() + 4;
	float logH = wh - curAreaH - ImGui::GetFrameHeight() - 20;
	drawInputLog(dl, ImVec2(wp.x, logTop), ww, logH);

	// Column headers at top
	ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 0.8f),
		"  Dir LP MP HP LK MK HK");

	ImGui::End();
}

// ── Public API ───────────────────────────────────────────────────────

void draw()
{
	if (!maplecast_mirror::isClient()) return;
	if (_showInput.load(std::memory_order_relaxed)) drawInputDisplay();
}

void toggleGameData() { _showGameData.store(!_showGameData.load()); }
void toggleInput()    { _showInput.store(!_showInput.load()); }

void recordInput(uint16_t buttons, uint8_t lt, uint8_t rt)
{
	std::lock_guard<std::mutex> lk(_historyMtx);
	_history[_historyHead] = { buttons, lt, rt };
	_historyHead = (_historyHead + 1) % HISTORY_SIZE;
	_totalInputFrames++;
}

} // namespace gui_game_overlay
