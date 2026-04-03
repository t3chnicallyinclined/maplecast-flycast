/*
	MapleCast Game State — MVC2 memory reader.

	Reads game state directly from Flycast's emulated Dreamcast RAM.
	80 bytes per frame. Every character position, health, animation,
	combo counter, meter, timer — everything the client needs to render.

	Addresses verified from 6 independent sources:
	  flycast-dojo-training/mvsc2.lua
	  lord-yoshi/MvC2-CE-Trainer-Script
	  libretro-database .cht files
	  Codebreaker cheat codes
	  MAME NAOMI cheat database
	  SRK community reverse engineering
*/
#pragma once
#include <cstdint>

namespace maplecast_gamestate
{

// MVC2 character state — per character, 6 total (P1C1, P2C1, P1C2, P2C2, P1C3, P2C3)
struct CharacterState {
	uint8_t  active;           // 1 = on point
	uint8_t  character_id;     // 0-58 (see CHARACTER table)
	float    pos_x;            // arena position X
	float    pos_y;            // arena position Y
	float    screen_x;         // screen position X
	float    screen_y;         // screen position Y
	float    vel_x;            // velocity X
	float    vel_y;            // velocity Y
	uint8_t  facing_right;     // 1 = facing right
	uint16_t sprite_id;        // current animation frame/sprite
	uint16_t animation_state;  // current action/state ID
	uint16_t anim_timer;       // animation cycle frame timer
	uint8_t  health;           // current HP (max 0x90 = 144)
	uint8_t  red_health;       // recoverable HP
	uint8_t  special_move_id;  // current special move
	uint8_t  assist_type;      // assist type ID
	uint8_t  palette_id;       // color variant
};  // 36 bytes per character

// MVC2 global game state
struct GameState {
	uint8_t  in_match;         // 1 = playing
	uint8_t  game_timer;       // 0-99
	uint8_t  stage_id;         // 0-16
	float    camera_x;         // camera position
	float    camera_y;
	uint16_t p1_meter_fill;    // hyper gauge fill
	uint16_t p2_meter_fill;
	uint8_t  p1_meter_level;   // 0-5 bars
	uint8_t  p2_meter_level;
	uint16_t p1_combo;         // combo counter
	uint16_t p2_combo;
	uint32_t frame_counter;    // total frames since boot

	CharacterState chars[6];   // P1C1, P2C1, P1C2, P2C2, P1C3, P2C3
};  // ~240 bytes total

// Read current MVC2 game state from Flycast's emulated RAM
void readGameState(GameState& state);

// Serialize to compact binary for network send
// Returns bytes written
int serialize(const GameState& state, uint8_t* buf, int maxLen);

}
