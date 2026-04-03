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

int serialize(const GameState& state, uint8_t* buf, int maxLen)
{
	// Simple binary serialization — just memcpy the struct
	// The struct is packed and consistent across platforms at these sizes
	int needed = sizeof(GameState);
	if (needed > maxLen) return 0;
	memcpy(buf, &state, needed);
	return needed;
}

}  // namespace maplecast_gamestate
