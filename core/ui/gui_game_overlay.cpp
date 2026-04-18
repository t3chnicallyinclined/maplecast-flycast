/*
	Game Data Overlay — MVC2 input display.

	!! IMPORTANT — NEVER HARDCODE BUTTON MAPPINGS !!
	Read button state from the SERVER's game state broadcast (GSTA).
	The server reads kcode[]/lt[]/rt[] — the same values the game uses.

	MVC2 has 4 attack buttons + 2 assists (NOT 6 like Street Fighter):
	  DC_BTN_X = LP (Light Punch)
	  DC_BTN_Y = HP (Heavy Punch)
	  DC_BTN_A = LK (Light Kick)
	  DC_BTN_B = HK (Heavy Kick)
	  LT       = A1 (Assist 1)  — RB on Xbox → DC LT
	  RT       = A2 (Assist 2)  — LB on Xbox → DC RT
	Source: docs/WEBGPU-RENDERER.md "MVC2 button layout"

	Layout on screen matches arcade panel:
	  LP  HP  A1
	  LK  HK  A2
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
static std::atomic<bool> _showInput{true};

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

// ── Stick arrow for direction ────────────────────────────────────────
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
	return "N";
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

// ── Stick direction indicator ────────────────────────────────────────
static void drawStick(ImDrawList* dl, float cx, float cy, float r,
                       bool up, bool down, bool left, bool right)
{
	dl->AddCircleFilled(ImVec2(cx, cy), r, IM_COL32(30, 30, 40, 180), 16);
	dl->AddCircle(ImVec2(cx, cy), r, IM_COL32(80, 80, 100, 200), 16);
	float dx = 0, dy = 0;
	if (left)  dx -= 1;
	if (right) dx += 1;
	if (up)    dy -= 1;
	if (down)  dy += 1;
	if (dx != 0 || dy != 0) {
		float len = sqrtf(dx * dx + dy * dy);
		dx = dx / len * r * 0.6f;
		dy = dy / len * r * 0.6f;
		dl->AddCircleFilled(ImVec2(cx + dx, cy + dy), r * 0.4f,
		                    IM_COL32(80, 180, 255, 255), 10);
	}
}

// RT trigger inversion detection — some controllers rest at 255
static bool _rtInverted = false;
static bool _rtDetected = false;

// Button colors
static const ImU32 COL_PUNCH  = IM_COL32(80, 160, 255, 230);
static const ImU32 COL_KICK   = IM_COL32(255, 140, 60, 230);
static const ImU32 COL_ASSIST = IM_COL32(160, 80, 220, 230);
static const ImU32 COL_DIR    = IM_COL32(80, 200, 255, 220);

// ── Current input state (big, at bottom of panel) ────────────────────
static void drawCurrentInput(ImDrawList* dl, ImVec2 wp,
                              uint16_t buttons, uint8_t sLt, uint8_t sRt)
{
	bool up    = !(buttons & DC_DPAD_UP);
	bool down  = !(buttons & DC_DPAD_DOWN);
	bool left  = !(buttons & DC_DPAD_LEFT);
	bool right = !(buttons & DC_DPAD_RIGHT);

	// MVC2: X=LP, Y=HP, A=LK, B=HK, LT=A1, RT=A2
	// Source: docs/WEBGPU-RENDERER.md "MVC2 button layout"
	bool lp = !(buttons & DC_BTN_X);
	bool hp = !(buttons & DC_BTN_Y);
	bool lk = !(buttons & DC_BTN_A);
	bool hk = !(buttons & DC_BTN_B);
	bool a1 = sLt > 128;
	// RT may be inverted (rests at 255, pressed=0) on some controllers.
	if (!_rtDetected && sRt > 200) { _rtInverted = true; _rtDetected = true; }
	if (!_rtDetected && sRt < 50)  { _rtInverted = false; _rtDetected = true; }
	bool a2 = _rtInverted ? (sRt < 128) : (sRt > 128);

	float r = 11.0f;
	float gap = 5.0f;
	float sp = r * 2 + gap;

	// Stick
	float sx = wp.x + 28;
	float sy = wp.y + 10;
	drawStick(dl, sx, sy, 16, up, down, left, right);

	// MVC2 arcade layout:
	//   LP  HP  A1
	//   LK  HK  A2
	float bx = wp.x + 60;
	float by = sy - r;
	drawDot(dl, bx,        by,      r, lp, COL_PUNCH);
	drawDot(dl, bx + sp,   by,      r, hp, COL_PUNCH);
	drawDot(dl, bx + sp*2, by,      r, a1, COL_ASSIST);
	drawDot(dl, bx,        by + sp, r, lk, COL_KICK);
	drawDot(dl, bx + sp,   by + sp, r, hk, COL_KICK);
	drawDot(dl, bx + sp*2, by + sp, r, a2, COL_ASSIST);

	// Labels above top row
	auto label = [&](float cx, float cy, const char* txt) {
		ImVec2 ts = ImGui::CalcTextSize(txt);
		dl->AddText(ImVec2(cx - ts.x/2, cy - ts.y/2),
		            IM_COL32(180, 180, 200, 200), txt);
	};
	label(bx,        by - r - 6, "LP");
	label(bx + sp,   by - r - 6, "HP");
	label(bx + sp*2, by - r - 6, "A1");
}

// ── Scrolling input log (vertical, newest at bottom) ─────────────────
static void drawInputLog(ImDrawList* dl, ImVec2 pos, float width, float height)
{
	float rowH = 14.0f;
	float dotR = 4.0f;
	float colW = 12.0f;
	float stickX = pos.x + 16;
	float btnStartX = pos.x + 36;

	std::lock_guard<std::mutex> lk(_historyMtx);
	int available = (_totalInputFrames < HISTORY_SIZE)
	              ? _totalInputFrames : HISTORY_SIZE;
	if (available == 0) return;

	int visibleRows = (int)(height / rowH);

	for (int i = 0; i < visibleRows && i < available; i++) {
		int ringIdx = (_historyHead - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
		InputFrame& f = _history[ringIdx];
		float rowY = pos.y + height - (i + 1) * rowH;
		if (rowY < pos.y) break;

		if (i > 0)
			dl->AddLine(ImVec2(pos.x + 4, rowY + rowH),
			            ImVec2(pos.x + width - 4, rowY + rowH),
			            IM_COL32(60, 60, 80, 40));

		float cy = rowY + rowH * 0.5f;

		// Stick direction
		const char* arrow = stickArrow(f.buttons);
		bool hasDir = strcmp(arrow, "N") != 0;
		dl->AddText(ImVec2(stickX - 4, cy - 6),
		            hasDir ? COL_DIR : IM_COL32(50, 50, 60, 80), arrow);

		// MVC2: X=LP, Y=HP, A=LK, B=HK, LT=A1, RT=A2
		float bx = btnStartX;
		drawDot(dl, bx,            cy, dotR, !(f.buttons & DC_BTN_X), COL_PUNCH);   // LP
		drawDot(dl, bx + colW,     cy, dotR, !(f.buttons & DC_BTN_Y), COL_PUNCH);   // HP
		drawDot(dl, bx + colW * 2, cy, dotR, f.lt > 128, COL_ASSIST);               // A1
		drawDot(dl, bx + colW * 3, cy, dotR, !(f.buttons & DC_BTN_A), COL_KICK);    // LK
		drawDot(dl, bx + colW * 4, cy, dotR, !(f.buttons & DC_BTN_B), COL_KICK);    // HK
		drawDot(dl, bx + colW * 5, cy, dotR, _rtInverted ? (f.rt < 128) : (f.rt > 128), COL_ASSIST); // A2
	}
}

// ── Main draw ────────────────────────────────────────────────────────
static void drawInputDisplay()
{
	// Read button state. In server/local mode, read directly from RAM.
	// In client mode, read from GSTA broadcast.
	// NEVER hardcode button mappings — always read from authoritative source.
	maplecast_gamestate::GameState gs;
	bool hasGS;
	if (maplecast_mirror::isServer()) {
		// Local/server: read game state directly from DC RAM
		maplecast_gamestate::readGameState(gs);
		hasGS = true;
	} else {
		// Mirror client: read from GSTA broadcast
		hasGS = maplecast_mirror::getClientGameState(gs);
	}

	uint16_t buttons = hasGS ? gs.p1_buttons : 0xFFFF;
	uint8_t sLt = hasGS ? gs.p1_lt : 0;
	uint8_t sRt = hasGS ? gs.p1_rt : 0;

	// Record server state into history for the scrolling log
	{
		std::lock_guard<std::mutex> lk(_historyMtx);
		bool changed = (_totalInputFrames == 0);
		if (!changed && _totalInputFrames > 0) {
			int prev = (_historyHead - 1 + HISTORY_SIZE) % HISTORY_SIZE;
			changed = (_history[prev].buttons != buttons ||
			           _history[prev].lt != sLt ||
			           _history[prev].rt != sRt);
		}
		if (changed) {
			_history[_historyHead] = { buttons, sLt, sRt };
			_historyHead = (_historyHead + 1) % HISTORY_SIZE;
			_totalInputFrames++;
		}
	}

	float dispW = ImGui::GetIO().DisplaySize.x;
	float dispH = ImGui::GetIO().DisplaySize.y;
	float panelW = 160;
	float panelH = dispH * 0.55f;

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

	// Column header
	ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 0.8f),
		" Dir LP HP A1 LK HK A2");

	// Current state (big buttons at bottom)
	float curAreaH = 40;
	ImVec2 curPos(wp.x, wp.y + wh - curAreaH - 8);
	drawCurrentInput(dl, curPos, buttons, sLt, sRt);

	// Separator
	dl->AddLine(ImVec2(wp.x + 8, wp.y + wh - curAreaH - 12),
	            ImVec2(wp.x + ww - 8, wp.y + wh - curAreaH - 12),
	            IM_COL32(80, 120, 200, 150));

	// Scrolling log (above current state)
	float logTop = wp.y + ImGui::GetFrameHeight() + 18;
	float logH = wh - curAreaH - ImGui::GetFrameHeight() - 26;
	drawInputLog(dl, ImVec2(wp.x, logTop), ww, logH);

	ImGui::End();
}

// ── Public API ───────────────────────────────────────────────────────

void draw()
{
	if (!maplecast_mirror::isActive()) return;
	if (_showInput.load(std::memory_order_relaxed)) drawInputDisplay();
}

void toggleGameData() { _showGameData.store(!_showGameData.load()); }
void toggleInput()    { _showInput.store(!_showInput.load()); }

void recordInput(uint16_t buttons, uint8_t lt, uint8_t rt)
{
	// Legacy path — kept for compatibility but the overlay now reads
	// from the server's GSTA broadcast directly.
	(void)buttons; (void)lt; (void)rt;
}

} // namespace gui_game_overlay
