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
	// Raw input state from the server's kcode[]/lt[]/rt[] — the same
	// values the game reads at vblank. NEVER hardcode button mappings
	// on the client — always read from this authoritative source.
	uint16_t p1_buttons;  // active-low DC button bits
	uint16_t p2_buttons;
	uint8_t  p1_lt, p1_rt;
	uint8_t  p2_lt, p2_rt;
	// Animated-stage timing — injected so the stage background renders from the
	// server's truth under FREEZE (the local SH4 would otherwise drive it).
	uint8_t  stage_anim_timer;   // 0x8C1F9D80
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
// 253 legacy + 8 raw input + 1 stage_anim = 262 bytes total
// NEVER hardcode button mappings on the client — read p1_buttons/p2_buttons.
static constexpr int WIRE_SIZE = 5 + 2+2+2+2 + 4+4+4 + 6*38 + 2+2+1+1+1+1 + 1; // = 262

// Patch the in-game "PLAYER" + "1"/"2" text with custom names
// slot: 0=P1, 1=P2. name: up to 10 chars (null-terminated)
void setPlayerName(int slot, const char* name);

// Restore original "PLAYER" + "1"/"2" text
void restorePlayerNames();

// Live "satellite" objects from the pool (cape, effects, projectiles). Each is a
// separate sprite the owner spawns; render PL{owner_cid}DAT/{sprite_id}.png at
// (screen_x, screen_y). The character is assembled from body (0x144) + these.
// See re-catalog/00-README.md + PL2A-storm.md.
struct ObjectState {
	uint8_t  owner_cid;   // owning character_id -> which rip atlas
	uint16_t sprite_id;   // indexes the owner's atlas (+0x12C)
	int16_t  screen_x;    // +0xC8
	int16_t  screen_y;    // +0xCC
	uint8_t  type;        // +0x0E : 1=lightning 2=aura 3=cape (attach/blend classifier)
	// Fields needed to INJECT the object back into the pool (state-replica
	// FREEZE). category@+0x3 selects the render head-list; xflip@+0x130 is the
	// authoritative facing the game's pool walker reads.
	uint8_t  category;    // +0x03 (marvelous2: render head-list selector, 0..13)
	uint8_t  xflip;       // +0x130
	uint8_t  owner_slot;  // which CHAR_BASE[] slot owns it (0..5); for owner-ptr re-inject
};
// Scan the object pool; fill up to maxObjs, return count. Skips inactive
// (sprite_id==0) and the position-less body object. Cheap RAM scan (~14k reads).
int readObjects(ObjectState* out, int maxObjs);

// PALF packet: per-slot palette-effect flag (char+0x40). Nonzero = body hit-flash
// (palette swapped to hurt bank). Writes 'PALF'+6×u16 into out; returns length (16).
int serializePalEffects(uint8_t* out, int maxLen);

// WTCH live bit-probe: configurable char-struct byte range x6 slots (debug).
int serializeWatch(uint8_t* out, int maxLen);

// Inject the object pool back into RAM (state-replica FREEZE — the INVERSE of
// readObjects). OVERWRITE MODE: for each wire object, find a matching already-
// linked local pool node (same owner + category, in wire order) and overwrite
// its sprite_id / screen_x/y / xflip so the game's pool render walker draws the
// server's truth. Does NOT allocate or relink nodes — objects the local SH4
// never spawned (input-driven projectiles/supers under FREEZE) have no node to
// write into and will be MISSING; that gap is the measured "needs node
// synthesis" region (see maplecast_state_replica.h). Returns nodes written.
int writeObjects(const ObjectState* objs, int n);

// Inject the animated-stage / background timing state so the stage renders from
// the server's truth (not the local SH4's). frame_counter + stage_anim_timer.
void writeStageState(uint32_t frame_counter, uint8_t stage_anim_timer);
uint8_t readStageAnimTimer();

// 'OBJF' packet — the FULL object record for the state-replica inject. Distinct
// from the browser-facing 'OBJS' packet (8/9B, position-only) so neither parser
// disturbs the other. Layout: per object owner_cid(1) sprite_id(2 LE)
// type(1) category(1) xflip(1) owner_slot(1) screen_x(i16 LE) screen_y(i16 LE)
// = 10 bytes. serialize writes count(1) + N*10 into buf (NO magic — caller
// prepends 'OBJF'); returns bytes written. deserialize reads it back.
static constexpr int OBJF_REC_SIZE = 10;
int  serializeObjects(const ObjectState* objs, int n, uint8_t* buf, int maxLen);
int  deserializeObjects(const uint8_t* buf, int len, ObjectState* out, int maxObjs);

}
