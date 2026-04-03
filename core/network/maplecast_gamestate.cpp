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

}  // namespace maplecast_gamestate
