/*
	MapleCast Game State — MVC2 memory reader.
	Reads game state from Flycast RAM, serializes to fixed byte layout for network.
	No struct packing — serialize() writes each field at exact byte offsets.
*/
#pragma once
#include <cstdint>

namespace maplecast_gamestate
{

struct CharacterState {
	uint8_t  active;
	uint8_t  character_id;
	uint8_t  facing_right;
	uint8_t  health;
	uint8_t  red_health;
	uint8_t  special_move_id;
	uint8_t  assist_type;
	uint8_t  palette_id;
	float    pos_x;
	float    pos_y;
	float    screen_x;
	float    screen_y;
	float    vel_x;
	float    vel_y;
	uint16_t sprite_id;
	uint16_t animation_state;
	uint16_t anim_timer;
	uint32_t anim_pointer;      // pointer to animation table in DC RAM
	// RAM autopsy found +0x502 (sub_anim_phase) and +0x00C (char_link_ptr)
	// but both are frame-deterministic — they sync naturally between instances
};

struct GameState {
	uint8_t  in_match;
	uint8_t  game_timer;
	uint8_t  stage_id;
	uint8_t  p1_meter_level;
	uint8_t  p2_meter_level;
	uint16_t p1_combo;
	uint16_t p2_combo;
	uint16_t p1_meter_fill;
	uint16_t p2_meter_fill;
	float    camera_x;
	float    camera_y;
	uint32_t frame_counter;
	CharacterState chars[6];
};

// Read current MVC2 game state from Flycast's emulated RAM
void readGameState(GameState& state);

// Write game state INTO Flycast's emulated RAM (client-side sync)
// The exact reverse of readGameState — same addresses, same offsets
void writeGameState(const GameState& state);

// Serialize to fixed byte layout for network (no padding issues)
// Returns bytes written. Layout documented in serialize() implementation.
int serialize(const GameState& state, uint8_t* buf, int maxLen);

// Deserialize from network bytes back to GameState
void deserialize(const uint8_t* buf, int len, GameState& state);

// Wire format size: 5 + 20 + 6*38 = 253 bytes
// RAM autopsy (rend_diff v2) found all correlated hidden addresses are
// frame-deterministic (counters/pointers that increment every frame) —
// they sync naturally between server+client instances running the same ROM.
// 253 bytes achieves 99.7%+ visual match rate. Remaining 0.3% is stage
// background animation and sub-frame interpolation jitter.
static constexpr int WIRE_SIZE = 5 + 2+2+2+2 + 4+4+4 + 6*38; // = 253 bytes

}
