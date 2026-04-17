/*
	Game Data Overlay — implementation.

	MVC2 button mapping (Dreamcast active-low):
	  LP=A(bit2) MP=B(bit1) HP=C(bit0)  — punch row
	  LK=X(bit10) MK=Y(bit9) HK=Z(bit8) — kick row
	  Directions: Up(4) Down(5) Left(6) Right(7)
	  Start(3)

	Input history: 120-frame ring buffer (~2 seconds at 60fps).
	Each entry stores the full button state + triggers. Rendered as a
	scrolling strip where each column is one frame, colored cells show
	which buttons were pressed.
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

// MVC2 character name table (indexed by character_id 0-55)
static const char* MVC2_CHAR_NAMES[] = {
	"Ryu","Zangief","Guile","Morrigan","Anakaris","Strider","Cyclops",
	"Wolverine","Psylocke","Iceman","Rogue","Capt America","Spider-Man",
	"Hulk","Venom","Dr Doom","Tron Bonne","Jill","Hayato","Ruby Heart",
	"SonSon","Amingo","Marrow","Cable","Abyss1","Abyss2","Abyss3",
	"Chun-Li","Megaman","Roll","Akuma","B.B.Hood","Felicia","Charlie",
	"Sakura","Dan","Cammy","Dhalsim","M.Bison","Ken","Gambit",
	"Juggernaut","Storm","Sabretooth","Magneto","Shuma","War Machine",
	"Silver Samurai","Omega Red","Spiral","Colossus","Iron Man",
	"Sentinel","Blackheart","Thanos","Jin"
};
static const int MVC2_CHAR_COUNT = sizeof(MVC2_CHAR_NAMES) / sizeof(MVC2_CHAR_NAMES[0]);

static const char* charName(uint8_t id) {
	return (id < MVC2_CHAR_COUNT) ? MVC2_CHAR_NAMES[id] : "???";
}

namespace gui_game_overlay
{

static std::atomic<bool> _showGameData{false};
static std::atomic<bool> _showInput{false};

// ── Input history ring buffer ────────────────────────────────────────
static constexpr int HISTORY_SIZE = 120; // 2 seconds at 60fps
struct InputFrame {
	uint16_t buttons; // active-low
	uint8_t  lt, rt;
};
static InputFrame _history[HISTORY_SIZE];
static int _historyHead = 0;
static std::mutex _historyMtx;
static uint32_t _totalInputFrames = 0;

// ── Layout ───────────────────────────────────────────────────────────
static const ImGuiWindowFlags OVL_FLAGS =
	ImGuiWindowFlags_NoDecoration |
	ImGuiWindowFlags_NoMove |
	ImGuiWindowFlags_NoResize |
	ImGuiWindowFlags_AlwaysAutoResize |
	ImGuiWindowFlags_NoSavedSettings |
	ImGuiWindowFlags_NoInputs |
	ImGuiWindowFlags_NoFocusOnAppearing |
	ImGuiWindowFlags_NoNav;

static const ImVec4 P1_COLOR = ImVec4(0.3f, 0.6f, 1.0f, 1.0f);
static const ImVec4 P2_COLOR = ImVec4(1.0f, 0.4f, 0.3f, 1.0f);
static const ImVec4 HEADER   = ImVec4(0.6f, 0.85f, 1.0f, 1.0f);
static const ImVec4 DIM      = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);

// ── Game Data Section ────────────────────────────────────────────────

static void drawHealthBar(float x, float y, float w, float h,
                           uint8_t health, uint8_t redHealth, ImVec4 color)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	// Background
	dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
	                  IM_COL32(40, 40, 40, 200));
	// Red health (recoverable damage)
	if (redHealth > 0) {
		float rw = w * (redHealth / 144.0f);
		dl->AddRectFilled(ImVec2(x, y), ImVec2(x + rw, y + h),
		                  IM_COL32(180, 40, 40, 200));
	}
	// Current health
	if (health > 0) {
		float hw = w * (health / 144.0f);
		dl->AddRectFilled(ImVec2(x, y), ImVec2(x + hw, y + h),
		                  ImGui::ColorConvertFloat4ToU32(color));
	}
	// Border
	dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h),
	            IM_COL32(200, 200, 200, 180));
}

static void drawGameData()
{
	maplecast_gamestate::GameState gs;
	if (!maplecast_mirror::getClientGameState(gs)) return;
	if (!gs.in_match) return;

	float dispW = ImGui::GetIO().DisplaySize.x;
	ImGui::SetNextWindowPos(ImVec2(dispW / 2 - 200, 4), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.7f);
	ImGui::Begin("##game_data", nullptr, OVL_FLAGS);

	// Top bar: P1 chars | timer | P2 chars
	auto& p1 = gs.chars[0]; // P1 point
	auto& p2 = gs.chars[1]; // P2 point

	ImGui::TextColored(P1_COLOR, "%-10s", charName(p1.character_id));
	ImGui::SameLine(160);
	ImGui::Text("%02d", gs.game_timer);
	ImGui::SameLine(200);
	ImGui::TextColored(P2_COLOR, "%10s", charName(p2.character_id));

	// Health bars
	ImVec2 wpos = ImGui::GetWindowPos();
	float barY = ImGui::GetCursorScreenPos().y;
	drawHealthBar(wpos.x + 8, barY, 150, 10, p1.health, p1.red_health, P1_COLOR);
	drawHealthBar(wpos.x + 200, barY, 150, 10, p2.health, p2.red_health, P2_COLOR);
	ImGui::Dummy(ImVec2(0, 14));

	// Assists
	ImGui::TextColored(DIM, " %s / %s",
		charName(gs.chars[2].character_id),
		charName(gs.chars[4].character_id));
	ImGui::SameLine(200);
	ImGui::TextColored(DIM, "%s / %s",
		charName(gs.chars[3].character_id),
		charName(gs.chars[5].character_id));

	// Meter + combo
	ImGui::Text("Meter: %d", gs.p1_meter_level);
	ImGui::SameLine(200);
	ImGui::Text("Meter: %d", gs.p2_meter_level);

	if (gs.p1_combo > 1) {
		ImGui::TextColored(ImVec4(1,1,0,1), "%d HITS!", gs.p1_combo);
	}
	if (gs.p2_combo > 1) {
		ImGui::SameLine(200);
		ImGui::TextColored(ImVec4(1,1,0,1), "%d HITS!", gs.p2_combo);
	}

	ImGui::End();
}

// ── Input Display Section ────────────────────────────────────────────

// Draw a single button cell — filled if pressed, outlined if not
static void drawBtn(ImDrawList* dl, float x, float y, float sz,
                     const char* label, bool pressed)
{
	ImU32 fill = pressed ? IM_COL32(80, 180, 255, 220) : IM_COL32(40, 40, 40, 120);
	ImU32 border = pressed ? IM_COL32(200, 230, 255, 255) : IM_COL32(100, 100, 100, 150);
	dl->AddRectFilled(ImVec2(x, y), ImVec2(x + sz, y + sz), fill, 3.0f);
	dl->AddRect(ImVec2(x, y), ImVec2(x + sz, y + sz), border, 3.0f);
	if (label[0]) {
		ImVec2 tsz = ImGui::CalcTextSize(label);
		dl->AddText(ImVec2(x + (sz - tsz.x) * 0.5f, y + (sz - tsz.y) * 0.5f),
		            pressed ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 150, 150, 180),
		            label);
	}
}

// Draw a direction indicator (stick/dpad)
static void drawStick(ImDrawList* dl, float cx, float cy, float r,
                       bool up, bool down, bool left, bool right)
{
	// Background circle
	dl->AddCircleFilled(ImVec2(cx, cy), r, IM_COL32(40, 40, 40, 150), 16);
	dl->AddCircle(ImVec2(cx, cy), r, IM_COL32(100, 100, 100, 180), 16);

	// Direction dot
	float dx = 0, dy = 0;
	if (left)  dx -= 1;
	if (right) dx += 1;
	if (up)    dy -= 1;
	if (down)  dy += 1;
	if (dx != 0 || dy != 0) {
		float len = sqrtf(dx * dx + dy * dy);
		dx = dx / len * r * 0.6f;
		dy = dy / len * r * 0.6f;
		dl->AddCircleFilled(ImVec2(cx + dx, cy + dy), r * 0.35f,
		                    IM_COL32(80, 180, 255, 255), 12);
	} else {
		// Neutral — small center dot
		dl->AddCircleFilled(ImVec2(cx, cy), r * 0.15f,
		                    IM_COL32(100, 100, 100, 150), 8);
	}
}

static void drawInputDisplay()
{
	InputFrame cur;
	{
		std::lock_guard<std::mutex> lk(_historyMtx);
		if (_totalInputFrames == 0) return;
		int idx = (_historyHead - 1 + HISTORY_SIZE) % HISTORY_SIZE;
		cur = _history[idx];
	}

	float dispH = ImGui::GetIO().DisplaySize.y;
	ImGui::SetNextWindowPos(ImVec2(8, dispH - 180), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.7f);
	ImGui::Begin("##input_display", nullptr, OVL_FLAGS);

	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 wp = ImGui::GetWindowPos();

	// Buttons are active-low: bit=0 means pressed
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

	float sz = 22.0f;
	float pad = 3.0f;
	float ox = wp.x + 8;
	float oy = wp.y + 8;

	// Stick
	drawStick(dl, ox + 20, oy + 20, 18, up, down, left, right);

	// MVC2 layout: LP MP HP / LK MK HK
	float bx = ox + 50;
	drawBtn(dl, bx,              oy,        sz, "LP", a);  // A = LP
	drawBtn(dl, bx + sz + pad,   oy,        sz, "MP", b);  // B = MP
	drawBtn(dl, bx + 2*(sz+pad), oy,        sz, "HP", c);  // C = HP
	drawBtn(dl, bx,              oy+sz+pad, sz, "LK", x);  // X = LK
	drawBtn(dl, bx + sz + pad,   oy+sz+pad, sz, "MK", y);  // Y = MK
	drawBtn(dl, bx + 2*(sz+pad), oy+sz+pad, sz, "HK", z);  // Z = HK

	// Triggers
	ImGui::Dummy(ImVec2(bx + 3*(sz+pad) - ox + 8, 2*(sz+pad) + 8));
	if (cur.lt > 10)
		ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "A1:%d", cur.lt);
	else
		ImGui::TextColored(DIM, "A1");
	ImGui::SameLine();
	if (cur.rt > 10)
		ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "A2:%d", cur.rt);
	else
		ImGui::TextColored(DIM, "A2");

	// ── Scrolling input history ──────────────────────────────────
	// Each column = 1 frame, each row = 1 button. 60 columns visible.
	// Pressed cells are filled, empty cells are dark.
	ImGui::Separator();
	ImGui::TextColored(HEADER, "INPUT HISTORY");

	float histX = ImGui::GetCursorScreenPos().x;
	float histY = ImGui::GetCursorScreenPos().y;
	int visibleFrames = 60;
	float cellW = 3.0f;
	float cellH = 4.0f;
	float rowGap = 1.0f;

	// Button rows: Up Down Left Right LP MP HP LK MK HK
	static const uint16_t ROW_BITS[] = {
		DC_DPAD_UP, DC_DPAD_DOWN, DC_DPAD_LEFT, DC_DPAD_RIGHT,
		DC_BTN_A, DC_BTN_B, DC_BTN_C,
		DC_BTN_X, DC_BTN_Y, DC_BTN_Z
	};
	static const int NUM_ROWS = 10;

	{
		std::lock_guard<std::mutex> lk(_historyMtx);
		int available = (_totalInputFrames < HISTORY_SIZE)
		              ? _totalInputFrames : HISTORY_SIZE;
		int start = (available < visibleFrames) ? 0 : available - visibleFrames;

		for (int col = 0; col < visibleFrames && col < available; col++) {
			int ringIdx = (_historyHead - available + start + col + HISTORY_SIZE) % HISTORY_SIZE;
			InputFrame& f = _history[ringIdx];
			float cx = histX + col * (cellW + 1);
			for (int row = 0; row < NUM_ROWS; row++) {
				float cy = histY + row * (cellH + rowGap);
				bool pressed = !(f.buttons & ROW_BITS[row]);
				ImU32 color = pressed
					? IM_COL32(80, 200, 255, 220)
					: IM_COL32(30, 30, 30, 80);
				dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + cellW, cy + cellH), color);
			}
		}
	}
	// Reserve space for the history strip
	ImGui::Dummy(ImVec2(visibleFrames * (cellW + 1),
	                     NUM_ROWS * (cellH + rowGap)));

	ImGui::End();
}

// ── Public API ───────────────────────────────────────────────────────

void draw()
{
	if (!maplecast_mirror::isClient()) return;
	if (_showGameData.load(std::memory_order_relaxed)) drawGameData();
	if (_showInput.load(std::memory_order_relaxed))    drawInputDisplay();
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
