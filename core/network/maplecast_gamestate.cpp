/*
	MapleCast Game State — reads MVC2 state from Dreamcast RAM every frame.
	80-240 bytes per frame instead of 29,000 bytes of H.264.
*/
#include "maplecast_gamestate.h"
#include "hw/sh4/sh4_mem.h"

namespace maplecast_gamestate
{

// MVC2 Dreamcast RAM addresses (DC virtual: 0x8C000000 + offset)
// All verified from flycast-dojo-training, lord-yoshi trainer, libretro .cht, Codebreaker codes

// Character struct bases (stride = 0x5A4, interleaved P1C1, P2C1, P1C2, P2C2, P1C3, P2C3)
static const uint32_t CHAR_BASE[] = {
	0x8C268340,  // P1 Character 1 (point)
	0x8C2688E4,  // P2 Character 1 (point)
	0x8C268E88,  // P1 Character 2 (assist 1)
	0x8C26942C,  // P2 Character 2 (assist 1)
	0x8C2699D0,  // P1 Character 3 (assist 2)
	0x8C269F74,  // P2 Character 3 (assist 2)
};

// Character struct field offsets
static const uint32_t OFF_ACTIVE          = 0x000;
static const uint32_t OFF_CHAR_ID         = 0x001;
static const uint32_t OFF_POS_X           = 0x034;  // float
static const uint32_t OFF_POS_Y           = 0x038;  // float
static const uint32_t OFF_SCREEN_X        = 0x0E0;  // float
static const uint32_t OFF_SCREEN_Y        = 0x0E4;  // float
static const uint32_t OFF_VEL_X           = 0x05C;  // float
static const uint32_t OFF_VEL_Y           = 0x060;  // float
static const uint32_t OFF_FACING          = 0x110;
static const uint32_t OFF_SPRITE_ID       = 0x144;  // u16
static const uint32_t OFF_ANIM_STATE      = 0x1D0;  // u16
static const uint32_t OFF_ANIM_TIMER      = 0x142;  // u16
static const uint32_t OFF_HEALTH          = 0x420;
static const uint32_t OFF_RED_HEALTH      = 0x424;
static const uint32_t OFF_SPECIAL_MOVE    = 0x1E9;
static const uint32_t OFF_ASSIST_TYPE     = 0x4C9;
static const uint32_t OFF_PALETTE         = 0x52D;
static const uint32_t OFF_ANIM_POINTER    = 0x168;  // pointer to animation table
// Hidden state discovered by RAM autopsy (rend_diff v2)
static const uint32_t OFF_SUB_ANIM_PHASE  = 0x502;  // sub-animation phase counter
static const uint32_t OFF_CHAR_LINK_PTR   = 0x00C;  // linked list pointer between chars

// Global state addresses
static const uint32_t ADDR_IN_MATCH       = 0x8C289624;
static const uint32_t ADDR_TIMER          = 0x8C289630;
static const uint32_t ADDR_STAGE          = 0x8C289638;
static const uint32_t ADDR_CAMERA_X       = 0x8C1F9CD8;  // float
static const uint32_t ADDR_CAMERA_Y       = 0x8C1F9CDC;  // float
static const uint32_t ADDR_P1_METER_FILL  = 0x8C289646;  // u16
static const uint32_t ADDR_P2_METER_FILL  = 0x8C289648;  // u16
static const uint32_t ADDR_P1_METER_LVL   = 0x8C28964A;
static const uint32_t ADDR_P2_METER_LVL   = 0x8C28964B;
static const uint32_t ADDR_P1_COMBO       = 0x8C289670;  // u16
static const uint32_t ADDR_P2_COMBO       = 0x8C289672;  // u16
static const uint32_t ADDR_FRAME_CTR      = 0x8C3496B0;  // u32
// Hidden state discovered by RAM autopsy (rend_diff v2)
// NOTE: stage_anim(0x8C1F9D80), render_interp/phase(0x8C1F9D8C-98) are frame-deterministic
// and sync naturally between server+client — excluded from state
static const uint32_t ADDR_FIGHT_TICK     = 0x8C268250;  // u8: fight engine logic counter
static const uint32_t ADDR_MATCH_SUB      = 0x8C289621;  // u8: match sub-state
static const uint32_t ADDR_ROUND_CTR      = 0x8C28962B;  // u8: round/sub-timer

// Helper: read float from DC memory
static float readFloat(uint32_t addr)
{
	uint32_t raw = addrspace::read32(addr);
	float f;
	memcpy(&f, &raw, 4);
	return f;
}

void readGameState(GameState& state)
{
	// Global state
	state.in_match      = (uint8_t)addrspace::read8(ADDR_IN_MATCH);
	state.game_timer    = (uint8_t)addrspace::read8(ADDR_TIMER);
	state.stage_id      = (uint8_t)addrspace::read8(ADDR_STAGE);
	state.camera_x      = readFloat(ADDR_CAMERA_X);
	state.camera_y      = readFloat(ADDR_CAMERA_Y);
	state.p1_meter_fill = (uint16_t)addrspace::read16(ADDR_P1_METER_FILL);
	state.p2_meter_fill = (uint16_t)addrspace::read16(ADDR_P2_METER_FILL);
	state.p1_meter_level = (uint8_t)addrspace::read8(ADDR_P1_METER_LVL);
	state.p2_meter_level = (uint8_t)addrspace::read8(ADDR_P2_METER_LVL);
	state.p1_combo      = (uint16_t)addrspace::read16(ADDR_P1_COMBO);
	state.p2_combo      = (uint16_t)addrspace::read16(ADDR_P2_COMBO);
	state.frame_counter = addrspace::read32(ADDR_FRAME_CTR);

	// Character states
	for (int i = 0; i < 6; i++)
	{
		uint32_t base = CHAR_BASE[i];
		CharacterState& c = state.chars[i];

		c.active          = (uint8_t)addrspace::read8(base + OFF_ACTIVE);
		c.character_id    = (uint8_t)addrspace::read8(base + OFF_CHAR_ID);
		c.pos_x           = readFloat(base + OFF_POS_X);
		c.pos_y           = readFloat(base + OFF_POS_Y);
		c.screen_x        = readFloat(base + OFF_SCREEN_X);
		c.screen_y        = readFloat(base + OFF_SCREEN_Y);
		c.vel_x           = readFloat(base + OFF_VEL_X);
		c.vel_y           = readFloat(base + OFF_VEL_Y);
		c.facing_right    = (uint8_t)addrspace::read8(base + OFF_FACING);
		c.sprite_id       = (uint16_t)addrspace::read16(base + OFF_SPRITE_ID);
		c.animation_state = (uint16_t)addrspace::read16(base + OFF_ANIM_STATE);
		c.anim_timer      = (uint16_t)addrspace::read16(base + OFF_ANIM_TIMER);
		c.health          = (uint8_t)addrspace::read8(base + OFF_HEALTH);
		c.red_health      = (uint8_t)addrspace::read8(base + OFF_RED_HEALTH);
		c.special_move_id = (uint8_t)addrspace::read8(base + OFF_SPECIAL_MOVE);
		c.assist_type     = (uint8_t)addrspace::read8(base + OFF_ASSIST_TYPE);
		c.palette_id      = (uint8_t)addrspace::read8(base + OFF_PALETTE);
		c.anim_pointer    = addrspace::read32(base + OFF_ANIM_POINTER);
	}
}

// Write float to DC memory
static void writeFloat(uint32_t addr, float f)
{
	uint32_t raw;
	memcpy(&raw, &f, 4);
	addrspace::write32(addr, raw);
}

// Write game state INTO Flycast's emulated RAM — exact reverse of readGameState
void writeGameState(const GameState& state)
{
	// Global state
	addrspace::write8(ADDR_IN_MATCH, state.in_match);
	addrspace::write8(ADDR_TIMER, state.game_timer);
	addrspace::write8(ADDR_STAGE, state.stage_id);
	writeFloat(ADDR_CAMERA_X, state.camera_x);
	writeFloat(ADDR_CAMERA_Y, state.camera_y);
	addrspace::write16(ADDR_P1_METER_FILL, state.p1_meter_fill);
	addrspace::write16(ADDR_P2_METER_FILL, state.p2_meter_fill);
	addrspace::write8(ADDR_P1_METER_LVL, state.p1_meter_level);
	addrspace::write8(ADDR_P2_METER_LVL, state.p2_meter_level);
	addrspace::write16(ADDR_P1_COMBO, state.p1_combo);
	addrspace::write16(ADDR_P2_COMBO, state.p2_combo);
	addrspace::write32(ADDR_FRAME_CTR, state.frame_counter);

	// Character states
	for (int i = 0; i < 6; i++)
	{
		uint32_t base = CHAR_BASE[i];
		const CharacterState& c = state.chars[i];

		addrspace::write8(base + OFF_ACTIVE, c.active);
		addrspace::write8(base + OFF_CHAR_ID, c.character_id);
		writeFloat(base + OFF_POS_X, c.pos_x);
		writeFloat(base + OFF_POS_Y, c.pos_y);
		writeFloat(base + OFF_SCREEN_X, c.screen_x);
		writeFloat(base + OFF_SCREEN_Y, c.screen_y);
		writeFloat(base + OFF_VEL_X, c.vel_x);
		writeFloat(base + OFF_VEL_Y, c.vel_y);
		addrspace::write8(base + OFF_FACING, c.facing_right);
		addrspace::write16(base + OFF_SPRITE_ID, c.sprite_id);
		addrspace::write16(base + OFF_ANIM_STATE, c.animation_state);
		addrspace::write16(base + OFF_ANIM_TIMER, c.anim_timer);
		addrspace::write8(base + OFF_HEALTH, c.health);
		addrspace::write8(base + OFF_RED_HEALTH, c.red_health);
		addrspace::write8(base + OFF_SPECIAL_MOVE, c.special_move_id);
		addrspace::write8(base + OFF_ASSIST_TYPE, c.assist_type);
		addrspace::write8(base + OFF_PALETTE, c.palette_id);
	}
}

static void writeU8(uint8_t* buf, int& off, uint8_t v) { buf[off++] = v; }
static void writeU16(uint8_t* buf, int& off, uint16_t v) { memcpy(buf + off, &v, 2); off += 2; }
static void writeU32(uint8_t* buf, int& off, uint32_t v) { memcpy(buf + off, &v, 4); off += 4; }
static void writeF32(uint8_t* buf, int& off, float v) { memcpy(buf + off, &v, 4); off += 4; }

int serialize(const GameState& state, uint8_t* buf, int maxLen)
{
	if (maxLen < WIRE_SIZE) return 0;
	int off = 0;

	// Global state (5 bytes)
	writeU8(buf, off, state.in_match);       // 0
	writeU8(buf, off, state.game_timer);     // 1
	writeU8(buf, off, state.stage_id);       // 2
	writeU8(buf, off, state.p1_meter_level); // 3
	writeU8(buf, off, state.p2_meter_level); // 4

	// Global u16/u32/float fields (16 bytes)
	writeU16(buf, off, state.p1_combo);      // 5
	writeU16(buf, off, state.p2_combo);      // 7
	writeU16(buf, off, state.p1_meter_fill); // 9
	writeU16(buf, off, state.p2_meter_fill); // 11
	writeF32(buf, off, state.camera_x);      // 13
	writeF32(buf, off, state.camera_y);      // 17
	writeU32(buf, off, state.frame_counter); // 21

	// 6 characters × 38 bytes each (228 bytes) starting at offset 25
	for (int i = 0; i < 6; i++)
	{
		const CharacterState& c = state.chars[i];
		writeU8(buf, off, c.active);           // +0
		writeU8(buf, off, c.character_id);     // +1
		writeU8(buf, off, c.facing_right);     // +2
		writeU8(buf, off, c.health);           // +3
		writeU8(buf, off, c.red_health);       // +4
		writeU8(buf, off, c.special_move_id);  // +5
		writeU8(buf, off, c.assist_type);      // +6
		writeU8(buf, off, c.palette_id);       // +7
		writeF32(buf, off, c.pos_x);           // +8
		writeF32(buf, off, c.pos_y);           // +12
		writeF32(buf, off, c.screen_x);        // +16
		writeF32(buf, off, c.screen_y);        // +20
		writeF32(buf, off, c.vel_x);           // +24
		writeF32(buf, off, c.vel_y);           // +28
		writeU16(buf, off, c.sprite_id);       // +32
		writeU16(buf, off, c.animation_state); // +34
		writeU16(buf, off, c.anim_timer);      // +36
		// total: 38 bytes per character
	}

	return off;  // should be WIRE_SIZE = 253
}

// Deserialize from network bytes back to GameState — exact reverse of serialize
static uint8_t readBufU8(const uint8_t* buf, int& off) { return buf[off++]; }
static uint16_t readBufU16(const uint8_t* buf, int& off) { uint16_t v; memcpy(&v, buf + off, 2); off += 2; return v; }
static uint32_t readBufU32(const uint8_t* buf, int& off) { uint32_t v; memcpy(&v, buf + off, 4); off += 4; return v; }
static float readBufF32(const uint8_t* buf, int& off) { float v; memcpy(&v, buf + off, 4); off += 4; return v; }

void deserialize(const uint8_t* buf, int len, GameState& state)
{
	if (len < WIRE_SIZE) return;
	int off = 0;

	state.in_match       = readBufU8(buf, off);
	state.game_timer     = readBufU8(buf, off);
	state.stage_id       = readBufU8(buf, off);
	state.p1_meter_level = readBufU8(buf, off);
	state.p2_meter_level = readBufU8(buf, off);
	state.p1_combo       = readBufU16(buf, off);
	state.p2_combo       = readBufU16(buf, off);
	state.p1_meter_fill  = readBufU16(buf, off);
	state.p2_meter_fill  = readBufU16(buf, off);
	state.camera_x       = readBufF32(buf, off);
	state.camera_y       = readBufF32(buf, off);
	state.frame_counter  = readBufU32(buf, off);

	for (int i = 0; i < 6; i++)
	{
		CharacterState& c = state.chars[i];
		c.active          = readBufU8(buf, off);
		c.character_id    = readBufU8(buf, off);
		c.facing_right    = readBufU8(buf, off);
		c.health          = readBufU8(buf, off);
		c.red_health      = readBufU8(buf, off);
		c.special_move_id = readBufU8(buf, off);
		c.assist_type     = readBufU8(buf, off);
		c.palette_id      = readBufU8(buf, off);
		c.pos_x           = readBufF32(buf, off);
		c.pos_y           = readBufF32(buf, off);
		c.screen_x        = readBufF32(buf, off);
		c.screen_y        = readBufF32(buf, off);
		c.vel_x           = readBufF32(buf, off);
		c.vel_y           = readBufF32(buf, off);
		c.sprite_id       = readBufU16(buf, off);
		c.animation_state = readBufU16(buf, off);
		c.anim_timer      = readBufU16(buf, off);
	}
}

// === Player name patching ===
// RAM layout at 0x8CBBC316:
//   +0: "PLAYER" (6 bytes) — shared prefix for both players
//   +6: \0\0 (2 bytes padding)
//   +8: "1" (1 byte) — P1 number string
//   +9: \0\0\0 (3 bytes padding)
//   +12: "2" (1 byte) — P2 number string
//   +13: \0\0\0 (3 bytes padding)
//
// Strategy: blank out "PLAYER" prefix, write player name into the "1"/"2" field.
// The "1"/"2" field has 4 bytes (including null). With "PLAYER" blanked,
// the display becomes just the 3-char tag. Not ideal but works.
//
// Better: overwrite the full 12 bytes (PLAYER + padding + number) per player.
// But "PLAYER" is shared. So we write "      \0\0" (spaces) over PLAYER,
// then write player name (up to 3 chars) into the number slot.

// Player name patching — two approaches:
//
// Approach 1: Patch "PLAYER" prefix + number strings (limited to 3 chars)
//   0x8CBBC316: "PLAYER\0\0" (8 bytes)
//   0x8CBBC31E: "1\0\0\0" / "2\0\0\0" (4 bytes each)
//
// Approach 2: Patch the "PLAYER%d" format string used by pause/VS screen
//   0x8CBBC982: "PLAYER%d\0\0\0\0" (12 bytes before "CONTINUE")
//   The game sprintf's this with player number. If we replace the whole
//   string with a pre-formatted name, %d never gets substituted.
//   BUT: this is shared for both players, so we can only show one name at a time.
//
// Approach 3 (current): Continuously patch. Every frame, write names to BOTH locations.
//   We overwrite "PLAYER\0\0" with spaces, then put full names in a custom RAM buffer
//   and patch the string pointers in the draw call list.
//
// For now: use Approach 1 extended — overwrite "PLAYER\0\0" + "1\0\0\0" as one
//   contiguous 12-byte region per player concept. The prefix "PLAYER" is shared
//   but we can blank it and use the number field. Max 3 chars per name.
//
// ACTUALLY: Let's use unused RAM. Write full names to a free area and patch
//   the "1" and "2" single-char strings to point... no, they're read as strings
//   not pointers.
//
// BEST: Overwrite at 0x8CBBC316. The layout is:
//   "PLAYER\0\0" + "1\0\0\0" + "2\0\0\0" + "WIN     %02d\0\0\0\0" + ...
//   Total 16 bytes for PLAYER+padding+1+padding+2+padding
//   If we overwrite all 16 bytes, we break WIN display.
//   Safe: overwrite PLAYER(6) + pad(2) + "1"(1) = 9 bytes for P1 name concept
//
// The real solution: find work RAM and write there.
// DC RAM 0x8C000000-0x8C00FFFF is usually stack/scratch. Let's use 0x8C000100.

static const uint32_t ADDR_PLAYER_PREFIX = 0x8CBBC316;  // "PLAYER\0\0"
static const uint32_t ADDR_P1_NUM = 0x8CBBC31E;         // "1\0\0\0"
static const uint32_t ADDR_P2_NUM = 0x8CBBC322;         // "2\0\0\0"

// Custom name buffer in unused low RAM — 16 bytes per player
static const uint32_t ADDR_P1_NAME_BUF = 0x8C000100;
static const uint32_t ADDR_P2_NAME_BUF = 0x8C000110;

// Save originals
static uint8_t _origData[16] = {};
static bool _origSaved = false;
static char _p1Name[16] = {};
static char _p2Name[16] = {};
static bool _namesActive = false;

static void saveOriginals()
{
	if (_origSaved) return;
	for (int i = 0; i < 16; i++)
		_origData[i] = (uint8_t)addrspace::read8(ADDR_PLAYER_PREFIX + i);
	_origSaved = true;
}

// Write a null-terminated string to DC RAM at addr, up to maxLen bytes
static void writeString(uint32_t addr, const char* str, int maxLen)
{
	int len = strlen(str);
	if (len > maxLen - 1) len = maxLen - 1;
	for (int i = 0; i < len; i++)
		addrspace::write8(addr + i, (uint8_t)str[i]);
	for (int i = len; i < maxLen; i++)
		addrspace::write8(addr + i, 0);
}

void setPlayerName(int slot, const char* name)
{
	saveOriginals();

	char* dest = (slot == 0) ? _p1Name : _p2Name;
	strncpy(dest, name, 15);
	dest[15] = 0;

	// Write to custom RAM buffer for future use
	uint32_t bufAddr = (slot == 0) ? ADDR_P1_NAME_BUF : ADDR_P2_NAME_BUF;
	writeString(bufAddr, dest, 16);

	// SHOTGUN: patch every location that might display player names
	// Let the user tell us which one actually shows on screen

	// Location 1: "PLAYER\0\0" prefix (0x8CBBC316) — 8 bytes
	// Blank it so only the number shows
	writeString(ADDR_PLAYER_PREFIX, "      ", 8);

	// Location 2: "1" / "2" number strings — 4 bytes each
	uint32_t numAddr = (slot == 0) ? ADDR_P1_NUM : ADDR_P2_NUM;
	writeString(numAddr, dest, 4);  // 3 chars max here

	// Location 3: "PLAYER%d" format string (0x8CBBC982) — 12 bytes before CONTINUE
	// Replace with just the name (no %d). Both players share this so only do it once.
	// This will show the SAME name for both players on pause screen.
	// writeString(0x8CBBC982, dest, 12);

	// Location 4: "PLAYER\0TART BUTTON" (0x8CD10145+2) — press start area
	// The \0 separates "PLAYER" from "START BUTTON", overwrite PLAYER part
	writeString(0x8CD10147, dest, 6);

	// Location 5: Second PLAYER at 0x8CD10187
	writeString(0x8CD10187, dest, 6);

	// Location 6: "PLAYER SE %x" at 0x8CBBEF24 — some debug string?
	writeString(0x8CBBEF24, dest, 10);

	_namesActive = true;
	printf("[gamestate] P%d name SHOTGUN patched to '%s' at 6 locations\n", slot + 1, dest);
}

void restorePlayerNames()
{
	if (!_origSaved) return;
	for (int i = 0; i < 16; i++)
		addrspace::write8(ADDR_PLAYER_PREFIX + i, _origData[i]);
	_namesActive = false;
	_p1Name[0] = 0;
	_p2Name[0] = 0;
	printf("[gamestate] Player names restored\n");
}

}  // namespace maplecast_gamestate
