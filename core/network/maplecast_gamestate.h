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
	// --- GSTA enrich (reconstruct-from-state, step 1; docs/MVC2-RECONSTRUCTION-SPEC.md §6) ---
	// The fields the ported quad emitter loc_8c033e90 + palette handler loc_8c035162 fold in.
	float    sprite_scale_x;    // char+0x50 (f32) — per-char/super dynamic zoom (CONFIRMED §6 #1)
	float    sprite_scale_y;    // char+0x54 (f32)
	uint8_t  pal_12d;           // char+0x12d (u8) — per-part palette row select (CONFIRMED §2)
	uint8_t  pal_12e;           // char+0x12e (u8) — live hit-flash / palette-effect (CONFIRMED §2,§3)
	uint8_t  overlay_1a4;       // char+0x1a4 (u8) — super/aura overlay class (CONFIRMED §3 loc_8c035162)
	// --- GSTA wire extension (append-only +49..+56; existing offsets unchanged) ---
	// All read by readGameState; parsed by every consumer. draw_layer is the slot-table
	// layer (the TRUE draw order, mirrors readAllDrawn); the rest are ENGINE fields the
	// emitter/overlay/palette work will fold in. 0xFF draw_layer = not in any layer.
	uint8_t  draw_layer;        // slot-table walk (0x8C2895E0/0x8C287DE0); 0xFF=not drawn (CONFIRMED reference_mvc2_slot_table_drawlist)
	uint8_t  render_extra;      // char+0x151 (u8) — RenderExtra (super/aura overlay driver)
	uint8_t  facing_1d2;        // char+0x1d2 (u8) — authoritative xflip (pl_mem.asm: xflip 0x01d2)
	uint8_t  pal_color_25;      // char+0x025 (u8) — live displayed palette idx (pl_mem.asm: pl_palid_match 0x25)
	uint8_t  hyper_armor;       // char+0x202 (u8) — Buff_HyperArmor (pl_mem.asm 0x0202)
	uint8_t  flight_flag;       // char+0x201 (u8) — Flight_Flag (pl_mem.asm 0x0201)
	uint8_t  stance;            // char+0x1f9 (u8) — stance: 0 stand/1 crouch/2 jump/3 otg (pl_mem.asm 0x01f9)
	uint8_t  _pad;              // +56: 0 — alignment/reserve for a future per-char byte
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

// Wire format size: 5 + 20 + 6*49 = 319 bytes (per-char stride bumped 38 -> 49 by the
// GSTA enrich: +0x50/0x54 scale (8) + 0x12d/0x12e palette (2) + 0x1a4 overlay (1) = 11).
// RAM autopsy (rend_diff v2) found all correlated hidden addresses are
// frame-deterministic (counters/pointers that increment every frame) —
// they sync naturally between server+client instances running the same ROM.
// 253 bytes achieves 99.7%+ visual match rate. Remaining 0.3% is stage
// background animation and sub-frame interpolation jitter.
// 319 legacy + 8 raw input + 1 stage_anim = 328 bytes total (pre-extension).
// GSTA wire EXTENSION (append-only): per-char block 49 -> 57 (+8: draw_layer,
// render_extra, facing_1d2, pal_color_25, hyper_armor, flight_flag, stance, _pad).
// New total: 25 header + 6*57 (342) + 9 tail (8 input + 1 stage) = 376.
// NEVER hardcode button mappings on the client — read p1_buttons/p2_buttons.
static constexpr int WIRE_SIZE = 5 + 2+2+2+2 + 4+4+4 + 6*57 + 2+2+1+1+1+1 + 1; // = 376

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
	// Effect-routing flag (GSTA enrich step 1): 1 iff the node's GFX base (node+0x15c)
	// points into the shared "Effect Poly" bank 0x0CED0000 (loc_8c032be0). The client
	// routes is_effect==1 objects to the effects atlas, the rest to the PL{cid} char
	// atlas. Derived in readAllDrawn where node+0x15c is already read.
	uint8_t  is_effect;   // node+0x15c in [0x0CED0000, 0x0CEE0000)
	// PATH A — TRUE ANCHOR (sprite-assembly hotspot). Each drawn node carries its
	// LIVE assembly at node+0x178 (Sprite_Extras); the 8B records {dx:s16@+0,
	// dy:s16@+2,...} define the part placement, and the true hotspot = (min dx,
	// min dy) over the records. We ship this so the client anchors satellites
	// (projectiles/capes/effects) at their OWN origin instead of the body-relative
	// baked sp.dx (which is wrong for satellites). 0,0 => no valid extras (client
	// falls back to the baked anchor). See re-catalog/00-README.md.
	int8_t   hot_dx;      // clamped min dx over node+0x178 extras records
	int8_t   hot_dy;      // clamped min dy over node+0x178 extras records
	// GSTA wire extension (append-only): low 16 bits of the node's GFX base pointer
	// (node+0x15c) — a stable per-effect content key for routing/dedup. Same source as
	// is_effect's gfxLow in readAllDrawn. LE on the wire.
	uint16_t effect_key;  // (node+0x15c) & 0xFFFF
	// GSTA wire extension (append-only, 2026-06-11): per-object PVR BLEND / list-type so
	// the lean off-SH4 client renders sparks/supers/auras with correct transparency
	// (additive vs alpha vs opaque) instead of flat opaque. The engine selects blend deep
	// in the bank12 PVR submit from the cell's TSP word (src=(tsp>>29)&7 dst=(tsp>>26)&7,
	// CHARQ-captured @ recBase+0x14); that submit does NOT run on the RAM-walk OBJS path,
	// and there is NO per-object blend byte in the pool node (the readAllDrawn fields
	// +0x03/0x130/0x15C/0x178 hold no list-type). So derive it RAM-side from the engine's
	// own render-path selector: the per-category draw dispatch loc_8c0301f6 (bank03:191-226)
	// splits category {0x05,0x06,0x0B,0x0C,0x0D,0x01}=body/cape (opaque/punch-through) vs
	// {0x07,0x08,0x09}=projectile/effect; and is_effect (GFX in Effect Poly 0x0CED0000) is
	// the proven additive-blend signal (reference_mvc2_effects_bank: effects = additive).
	// Encoding: 0=punch-through/opaque, 1=alpha/translucent, 2=additive.
	uint8_t  blend;       // derived: 0=PT/opaque 1=alpha 2=additive (computeBlend, this file)
};

// Derive the PVR blend/list-type for a pool object from its RAM-walkable fields.
// is_effect (GFX in Effect Poly 0x0CED0000) => 2=additive (sparks/supers/auras render
// additively, reference_mvc2_effects_bank). Body/cape categories {0x05,0x06,0x0B,0x0C,
// 0x0D,0x01} => 0=opaque/punch-through (audit: bodies are uniformly Dat_Pal + PT). Other
// (non-effect projectile-class {0x07,0x08,0x09}) => 1=alpha as the conservative middle.
// Per the engine's per-category render dispatch loc_8c0301f6 (bank03:191-226) + the master
// back-to-front order loc_8c0305d8 (bank03:733). RAM-derived; the TSP submit isn't run here.
static inline uint8_t computeObjectBlend(uint8_t is_effect, uint8_t category)
{
	if (is_effect) return 2;                 // additive
	switch (category) {
		case 0x05: case 0x06: case 0x0B:     // body/cape behind-group
		case 0x0C: case 0x0D: case 0x01:     // body/cape front-group
			return 0;                        // opaque / punch-through
		default:
			return 1;                        // alpha (projectile-class 0x07/0x08/0x09 etc.)
	}
}
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
// is_effect(1) hot_dx(s8) hot_dy(s8) effect_key(u16 LE) blend(u8) = 16 bytes. serialize writes
// count(1) + N*16 into buf (NO magic — caller prepends 'OBJF'); returns bytes written.
// deserialize reads it back. (effect_key appended 2026-06-11, append-only.)
// blend(u8) appended 2026-06-11 (append-only) after effect_key => 16 bytes.
static constexpr int OBJF_REC_SIZE = 16;
int  serializeObjects(const ObjectState* objs, int n, uint8_t* buf, int maxLen);
int  deserializeObjects(const uint8_t* buf, int len, ObjectState* out, int maxObjs);

}
