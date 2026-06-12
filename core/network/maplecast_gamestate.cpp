/*
	MapleCast Game State — reads MVC2 state from Dreamcast RAM every frame.
	80-240 bytes per frame instead of 29,000 bytes of H.264.
*/
#include "maplecast_gamestate.h"
#include "hw/sh4/sh4_mem.h"
#include "types.h"          // RAM_SIZE (settings.platform.ram_size)
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <algorithm>

// Gamepad globals — authoritative input state read by the game at vblank
extern u32 kcode[4];
extern u16 lt[4], rt[4];

// Render-phase sprite_id/anim_timer latch — populated at STARTRENDER in oracle_hook.cpp
// (mc_oracle_charPassCapture, the phase loc_8c0344d4 renders). Read below so the wire ships
// the RENDERED-frame sid, not serverPublish's 1-frame-later read (finding:gsta_sprite_id_sampling_phase).
namespace maplecast_oracle_hook {
	extern uint16_t mc_sidLatch[6];
	extern uint16_t mc_timerLatch[6];
	extern uint8_t  mc_sidLatchValid[6];
	// REAL per-object PVR blend captured at the bank12 submit (MAPLECAST_FRAME_ORACLE_HOOK).
	// 0=opaque/PT, 1=alpha, 2=additive, 0xFF = no capture this frame (use computeObjectBlend).
	uint8_t mc_oracle_nodeBlend(uint32_t node);
}

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
// GSTA enrich (reconstruct-from-state, step 1) — quad-emitter / palette-handler inputs
static const uint32_t OFF_SPRITE_SCALE_X  = 0x050;  // float (CONFIRMED spec §2,§6 #1)
static const uint32_t OFF_SPRITE_SCALE_Y  = 0x054;  // float
static const uint32_t OFF_PAL_12D         = 0x12D;  // u8: per-part palette row (CONFIRMED §2)
static const uint32_t OFF_PAL_12E         = 0x12E;  // u8: live hit-flash / palette-effect (CONFIRMED §2,§3)
static const uint32_t OFF_OVERLAY_1A4     = 0x1A4;  // u8: super/aura overlay class (CONFIRMED §3 loc_8c035162)
// GSTA wire extension (append-only +49..+55). OFF_COLOR (0x025) below is pal_color_25.
static const uint32_t OFF_RENDER_EXTRA     = 0x151;  // u8: RenderExtra (super/aura overlay driver; gamestate ramFieldName)
static const uint32_t OFF_FACING_1D2       = 0x1D2;  // u8: authoritative xflip (pl_mem.asm: xflip 0x01d2)
static const uint32_t OFF_HYPER_ARMOR      = 0x202;  // u8: Buff_HyperArmor (pl_mem.asm 0x0202)
static const uint32_t OFF_FLIGHT           = 0x201;  // u8: Flight_Flag (pl_mem.asm 0x0201)
static const uint32_t OFF_STANCE           = 0x1F9;  // u8: stance 0 stand/1 crouch/2 jump/3 otg (pl_mem.asm 0x01f9)
// Hidden state discovered by RAM autopsy (rend_diff v2)
static const uint32_t OFF_SUB_ANIM_PHASE  = 0x502;  // sub-animation phase counter
static const uint32_t OFF_CHAR_LINK_PTR   = 0x00C;  // linked list pointer between chars
// Runtime pointers into the engine's DECODED structures (runtime-derivation probe).
// These are resolved by the engine when a character loads — following them reads
// the already-decompressed palette / assembly the GPU is using, no ROM codec.
static const uint32_t OFF_COLOR           = 0x025;  // live displayed palette idx (button + hit-flash)
static const uint32_t OFF_GFX00_PTR       = 0x15C;  // -> decoded GFX (GFX1 part pixels)
static const uint32_t OFF_GFX01_PTR       = 0x160;  // -> GFX2 cell table (sid -> part list)
static const uint32_t OFF_PAL_PTR         = 0x164;  // -> live ARGB4444 palette
static const uint32_t OFF_HITBOX_PTR      = 0x170;  // -> live hitbox data
static const uint32_t OFF_EXTRAS_PTR      = 0x178;  // -> sprite assembly / extras

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
static const uint32_t ADDR_STAGE_ANIM     = 0x8C1F9D80;  // u8: monotonic stage-anim timer
                                                          // (docs/MVC2-MEMORY-MAP.md page 505)
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

// Helper: write float to DC memory (defined here so the object-pool / stage
// injectors below can use it; readGameState's writers reuse it too)
static void writeFloat(uint32_t addr, float f)
{
	uint32_t raw;
	memcpy(&raw, &f, 4);
	addrspace::write32(addr, raw);
}

// Read-only runtime-derivation probe (MAPLECAST_PTRDUMP=1). Follows the engine's
// resolved pointers and dumps the DECODED palette + assembly the GPU is using —
// proving we can build anchors/colors from the runtime without ROM-codec RE.
static inline bool inRam(uint32_t a) { return a >= 0x0C000000 && a < 0x10000000; }

// PATH A — TRUE ANCHOR. Walk a drawn node's LIVE sprite-assembly (node+0x178,
// Sprite_Extras) and return the hotspot = (min dx, min dy) over the 8-byte
// records. Binding confirmed marvelous2 bank10:loc_8C1075EA; the iterator
// loc_8C108060 starts the walk at extras+0x18; per-record layout (8B):
//   dx:s16@+0, dy:s16@+2, part:u8@+4, b5@+5, mode:u8@+6 (0xFF = end), flip@+7
// The satellite's own origin != the body's, so the baked body-relative sp.dx is
// wrong for it; the assembly hotspot is the object's true origin. Returns true
// and sets hotDx/hotDy (clamped to int8) if valid extras were found; false =>
// degenerate / no extras (client keeps the baked anchor).
static bool readHotspot(uint32_t node, int8_t& hotDx, int8_t& hotDy)
{
	uint32_t e = addrspace::read32(node + OFF_EXTRAS_PTR);   // +0x178
	if (!inRam(e)) return false;
	int minDx = 0x7fffffff, minDy = 0x7fffffff;
	bool any = false;
	uint32_t rec = e + 0x18;                                 // iterator start (loc_8C108060)
	for (int guard = 0; guard < 64; ++guard, rec += 8) {
		uint8_t mode = (uint8_t)addrspace::read8(rec + 6);
		if (mode == 0xFF) break;                             // end sentinel
		int dx = (int16_t)addrspace::read16(rec + 0);
		int dy = (int16_t)addrspace::read16(rec + 2);
		if (dx < minDx) minDx = dx;
		if (dy < minDy) minDy = dy;
		any = true;
	}
	if (!any) return false;
	auto clampS8 = [](int v) -> int8_t {
		if (v < -128) v = -128; else if (v > 127) v = 127; return (int8_t)v;
	};
	hotDx = clampS8(minDx);
	hotDy = clampS8(minDy);
	return true;
}

static void ptrDump(const GameState& state)
{
	static bool en = (getenv("MAPLECAST_PTRDUMP") != nullptr);
	if (!en) return;

	// PER-FRAME object tracker -> /dev/shm file (journal rate-limits at 60Hz).
	// Transient projectiles live only a few frames, so the 2s cadence below misses
	// them. Dump every owner-pointer object's full struct each frame; grep the file
	// for the projectile (e.g. Magneto disruptor sprite 802-814) to find its layout.
	if (state.in_match) {
		static FILE* of = nullptr; static long wrote = 0;
		if (!of) of = fopen("/dev/shm/mc_obj.log", "w");
		if (of && wrote < 8L*1024*1024) {
			for (uint32_t a = 0x8C26A600; a < 0x8C278000; a += 4) {
				uint32_t v = addrspace::read32(a);
				int oslot = -1; for (int s = 0; s < 6; s++) if (v == CHAR_BASE[s]) oslot = s;
				if (oslot < 0) continue;
				uint8_t ocid = (uint8_t)addrspace::read8(v + OFF_CHAR_ID);
				char b[800]; int n = snprintf(b, sizeof b, "f%u obj=%08x cid=%d:", state.frame_counter, a, ocid);
				for (int o = -0x10; o < 0x140; o += 4)
					n += snprintf(b + n, sizeof(b) - n, "%08x", (uint32_t)addrspace::read32(a + o));
				n += snprintf(b + n, sizeof(b) - n, "\n");
				fwrite(b, 1, n, of); wrote += n;
			}
			fflush(of);
		}
	}

	static uint32_t fc = 0;
	if (++fc % 120 != 0) return;                 // ~every 2s (char dump only)
	printf("[PTRDUMP] ===== in_match=%d frame=%u =====\n", state.in_match, state.frame_counter);
	for (int i = 0; i < 6; i++)
	{
		uint32_t base = CHAR_BASE[i];
		if (!(uint8_t)addrspace::read8(base + OFF_ACTIVE)) continue;
		uint8_t  cid   = (uint8_t)addrspace::read8(base + OFF_CHAR_ID);
		uint16_t sid   = (uint16_t)addrspace::read16(base + OFF_SPRITE_ID);
		float    sx    = readFloat(base + OFF_SCREEN_X), sy = readFloat(base + OFF_SCREEN_Y);
		uint8_t  color = (uint8_t)addrspace::read8(base + OFF_COLOR);
		uint8_t  palId = (uint8_t)addrspace::read8(base + OFF_PALETTE);
		uint32_t gfx0  = addrspace::read32(base + OFF_GFX00_PTR);
		uint32_t palP  = addrspace::read32(base + OFF_PAL_PTR);
		uint32_t hbP   = addrspace::read32(base + OFF_HITBOX_PTR);
		uint32_t exP   = addrspace::read32(base + OFF_EXTRAS_PTR);
		// marvelous2 reconcile: facing@0x110 is xflip_copy_2 (the COPY we currently ship);
		// authoritative xflip=0x1D2, walk-dir=0x1D3. If 0x110 != 0x1D2 during a turn-around,
		// 0x110 is the wrong wire source. paleffect@0x40 = super-glow/hit-flash tint.
		uint8_t  f110  = (uint8_t)addrspace::read8(base + 0x110);
		uint8_t  x1d2  = (uint8_t)addrspace::read8(base + 0x1D2);
		uint8_t  w1d3  = (uint8_t)addrspace::read8(base + 0x1D3);
		uint16_t peff  = (uint16_t)addrspace::read16(base + 0x40);
		printf("[PTRDUMP] slot%d cid=%2d sid=0x%04x scr=(%.0f,%.0f) color@0x25=%d palId@0x52D=%d\n",
		       i, cid, sid, sx, sy, color, palId);
		printf("[PTRDUMP]   FACING fac@0x110=%d xflip@0x1D2=%d walk@0x1D3=%d  paleffect@0x40=%u\n",
		       f110, x1d2, w1d3, peff);
		printf("[PTRDUMP]   ptrs GFX00=0x%08x PAL=0x%08x HITBOX=0x%08x EXTRAS=0x%08x\n", gfx0, palP, hbP, exP);
		if (inRam(palP)) {
			char b[300]; int n = snprintf(b, sizeof b, "[PTRDUMP]   PAL16:");
			for (int c = 0; c < 16; c++) n += snprintf(b+n, sizeof(b)-n, " %04x", (uint16_t)addrspace::read16(palP + c*2));
			printf("%s\n", b);
		}
		if (inRam(exP)) {
			char b[300]; int n = snprintf(b, sizeof b, "[PTRDUMP]   EXTRAS[0..31]:");
			for (int k = 0; k < 32; k++) n += snprintf(b+n, sizeof(b)-n, " %02x", (uint8_t)addrspace::read8(exP + k));
			printf("%s\n", b);
		}
	}
	// Object-pool RE v2: find object structs by their OWNER pointer (a value
	// equal to one of the 6 char-struct bases), then dump the full struct around
	// it, decoded — so screen_x/y (float in 0..640/0..480) and a sprite_id (u16)
	// stand out. Also derive owner char_id + the gfx-offset (object gfx ptr minus
	// the owner's GFX00 base) so we can later map it to our atlas. Reads only.
	if (state.in_match) {
		static uint32_t dc = 0;
		if (++dc % 2) return;            // ~every 4s, cut log volume
		int found = 0;
		for (uint32_t a = 0x8C26A000; a < 0x8C278000 && found < 3; a += 4) {
			uint32_t v = addrspace::read32(a);
			int oslot = -1;
			for (int s = 0; s < 6; s++) if (v == CHAR_BASE[s]) oslot = s;
			if (oslot < 0) continue;
			found++;
			uint8_t  ocid    = (uint8_t)addrspace::read8(v + OFF_CHAR_ID);
			uint32_t gfxbase = addrspace::read32(v + OFF_GFX00_PTR);
			uint32_t objgfx  = addrspace::read32(a - 0x8);
			printf("[OBJ] owner@0x%08x slot%d cid=%d gfx=0x%08x gfxbase=0x%08x gfxoff=0x%x\n",
			       a, oslot, ocid, objgfx, gfxbase, objgfx - gfxbase);
			for (int32_t d = -0x20; d < 0x160; d += 4) {
				uint32_t fv = addrspace::read32(a + d);
				char tb[48]; tb[0] = 0;
				float f; memcpy(&f, &fv, 4);
				if (0x8c000000 <= fv && fv < 0x8d000000) snprintf(tb, sizeof tb, "ptr");
				else if (f > 1 && f < 640) snprintf(tb, sizeof tb, "f=%.1f", f);
				else if (fv > 0 && fv < 4000) snprintf(tb, sizeof tb, "u=%u", fv);
				if (tb[0]) printf("[OBJ]   owner%+05x: %08x %s\n", d, fv, tb);
			}
			printf("[OBJ] ----\n");
		}
	}

	// marvelous2 POOL ANCHOR RESOLVER: our readObjects() treats the owner-pointer word
	// address `a` as the record base (H1: sid@a+0x12C, scr@a+0xC8). The disasm says the
	// owner ptr sits at record+0x80 (H2: record base = a-0x80). Dump BOTH so one live
	// capture decides which yields the known Storm cape sid range (731-763) at the right
	// screen pos. `ownerCopies` = every offset near `a` where the owner value repeats;
	// the disasm predicts copies at +0x80/+0x84 of the record (so if `a` is the FIRST
	// copy and the record base, we expect another at +4; if `a` is the +0x80 copy, the
	// record starts 0x80 earlier). The cape sid range is the real tiebreaker.
	{
		printf("[POOLV] === anchor resolver: H1(a=base) vs H2(base=a-0x80); cape sid in 731-763 wins ===\n");
		int shown = 0;
		for (uint32_t a = 0x8C26A600; a < 0x8C278000 && shown < 12; a += 4) {
			uint32_t v = addrspace::read32(a);
			int oslot = -1; for (int c = 0; c < 6; c++) if (v == CHAR_BASE[c]) oslot = c;
			if (oslot < 0) continue;
			char rep[160]; int rn = 0;
			for (int32_t d = -0x100; d <= 0x100 && rn < (int)sizeof(rep)-8; d += 4)
				if ((uint32_t)addrspace::read32(a + d) == v) rn += snprintf(rep+rn, sizeof(rep)-rn, "%+d ", d);
			uint16_t sidH1 = (uint16_t)addrspace::read16(a + 0x12C);
			float    sxH1  = readFloat(a + 0xC8), syH1 = readFloat(a + 0xCC);
			uint8_t  catH1 = (uint8_t)addrspace::read8(a + 0x03);
			uint32_t rb    = a - 0x80;
			uint16_t sidH2 = (uint16_t)addrspace::read16(rb + 0x12C);
			float    sxH2  = readFloat(rb + 0xC8), syH2 = readFloat(rb + 0xCC);
			uint8_t  catH2 = (uint8_t)addrspace::read8(rb + 0x03);
			if (sidH1 == 0 && sidH2 == 0) continue;
			shown++;
			uint8_t  ocid = (uint8_t)addrspace::read8(v + OFF_CHAR_ID);
			uint32_t nxt = addrspace::read32(a + 0x08), prv = addrspace::read32(a + 0x0C);
			float owx = readFloat(v + 0x34), owy = readFloat(v + 0x38);
			printf("[POOLV] a=%08x cid=%d ownW=(%.0f,%.0f) nx=%08x pv=%08x ownerCopies@{%s}\n",
			       a, ocid, owx, owy, nxt, prv, rep);
			printf("[POOLV]   H1(a=base):    sid=%-5u cat=%02x scr=(%.0f,%.0f)\n", sidH1, catH1, sxH1, syH1);
			printf("[POOLV]   H2(base=a-80): sid=%-5u cat=%02x scr=(%.0f,%.0f)\n", sidH2, catH2, sxH2, syH2);
		}
	}
}

// Faithful render-list enumeration — ports MVC2's per-category head-list walk
// (marvelous2 `loc_8c0301ce`, bank03). The game draws satellite objects (cape /
// projectiles / supers) by walking, per category, the linked list whose head is
// heads[cat] = *(0x8C287A5C + cat*4); each list is SINGLY-linked forward via
// node+0x0C. Node base = the old scan anchor `a` minus 0x18, so the canonical
// pl_mem.asm offsets apply directly (sprite_id 0x144, screen 0xE0/0xE4,
// visible 0x12C, xflip 0x130, category 0x03, owner-word 0x18).
//
// Membership in heads[cat] IS the alive signal — there is no doubly-linked
// back-link. Our legacy scan's "alive" test read next@a-0x10 / prev@a-0x0c,
// which are the WRONG fields (the real `next` is node+0x0C == a-0x0c, which the
// old code misnamed "prev"); that test culled live objects (capture: only 2 of
// 39 valid-sid objects passed) and let stale cape frames linger. Walking the
// head-lists yields EXACTLY the drawn objects, in z-order, no ghost frames.
// Gated by MAPLECAST_OBJS_WALK until visually A/B-confirmed against the legacy
// path (prod browser clients keep the legacy reader until then).
static int readObjectsWalk(ObjectState* out, int maxObjs)
{
	static const uint32_t HEAD_ARRAY = 0x8C287A5C;          // heads[cat], stride 4
	// Category draw order back->front (master dispatch loc_8c0305d8): cape/body
	// class {0x0B,0x05,0x06} behind, projectile/effect class {0x07,0x08,0x09},
	// then {0x0C,0x0D,0x01} in front.
	static const uint8_t DRAW_ORDER[] = {0x0B,0x05,0x06,0x07,0x08,0x09,0x0C,0x0D,0x01};
	int n = 0, visited = 0;
	for (uint8_t cat : DRAW_ORDER) {
		uint32_t node = addrspace::read32(HEAD_ARRAY + cat * 4);
		for (int guard = 0; node >= 0x8C000000 && node < 0x8D000000
		                    && guard < 256 && n < maxObjs; ++guard) {
			visited++;
			uint16_t sid = (uint16_t)addrspace::read16(node + 0x144);
			// Only emit fighter-OWNED satellites (cape/projectile/super). The
			// head-lists also carry stage/UI nodes (the bulk of `visited`); the
			// owner-word @node+0x18 == a fighter base is the capture-proven scope
			// filter, and list membership is the alive signal. (Dropped the
			// unverified vis@0x12C / opt@0x84 gates — the capture confirmed only
			// sprite_id 0x144 and screen 0xE0/0xE4.)
			if (sid != 0) {
				uint32_t ownerWord = addrspace::read32(node + 0x18);
				int slot = -1;
				for (int s = 0; s < 6; s++) if (ownerWord == CHAR_BASE[s]) { slot = s; break; }
				float sx = readFloat(node + 0xE0), sy = readFloat(node + 0xE4);
				if (slot >= 0 && !(sx == 0.f && sy == 0.f)
				    && sx >= -64.f && sx <= 704.f && sy >= -64.f && sy <= 544.f) {
					// owner_cid drives the atlas (PL{cid}); use the OWNER's char id so
					// e.g. Storm's cape (sid 735) resolves against PL2A.
					out[n].owner_cid  = (uint8_t)addrspace::read8(ownerWord + OFF_CHAR_ID);
					out[n].sprite_id  = sid;
					out[n].screen_x   = (int16_t)sx;
					out[n].screen_y   = (int16_t)sy;
					out[n].type       = cat;                              // walk-order layer
					out[n].category   = (uint8_t)addrspace::read8(node + 0x03);  // real render layer byte
					out[n].xflip      = (uint8_t)(addrspace::read16(node + 0x130) ? 1 : 0);
					out[n].owner_slot = (uint8_t)(slot < 0 ? 0 : slot);
					{ uint32_t gbRaw = addrspace::read32(node + OFF_GFX00_PTR);
					  uint32_t gb = gbRaw & 0x0FFFFFFF;
					  out[n].is_effect = (gb >= 0x0CED0000 && gb < 0x0CEE0000) ? 1 : 0;
					  out[n].effect_key = (uint16_t)(gbRaw & 0xFFFF); }   // GSTA wire ext
					// GSTA wire ext: per-object PVR blend/list-type. PREFER the engine's REAL
					// TSP blend (captured at the bank12 submit when MAPLECAST_FRAME_ORACLE_HOOK is
					// on) keyed by this node; fall back to the category heuristic if not captured.
					{ uint8_t rb = maplecast_oracle_hook::mc_oracle_nodeBlend(node);
					  out[n].blend = (rb != 0xFF) ? rb
					               : computeObjectBlend(out[n].is_effect, out[n].category); }
					// PATH A: true assembly hotspot from node+0x178 (parity w/ readAllDrawn).
					out[n].hot_dx = 0; out[n].hot_dy = 0;
					readHotspot(node, out[n].hot_dx, out[n].hot_dy);
					n++;
				}
			}
			node = addrspace::read32(node + 0x0C);                    // forward link
		}
	}
	static int _dbg = 0;
	if (++_dbg % 120 == 0) fprintf(stderr, "[OBJS-WALK] visited=%d emitted=%d\n", visited, n);
	return n;
}

// FULL draw-list enumeration — reads MVC2's OWN per-frame slot table, the exact
// structure the renderer walks (marvelous2 loc_8c0308c2 "Render_sprites"). This
// captures EVERYTHING on screen — bodies, capes, projectiles, AND the owner-less
// global super overlays (Hailstorm / Lightning Storm) that the owner-scan can't
// see because it keys on a fighter-base owner word. Slot table:
//   per-layer count = u8 @ 0x8C2895E0[layer]   (16 layers)
//   node-ptr array  = u32 @ 0x8C287DE0 + layer*0x180 + i*4   (<=0x60/layer)
// The slot-table pointer IS the node record base R, so fields are ABSOLUTE
// (no +0x18 skew): sprite_id R+0x144, screen R+0xE0/0xE4, visible R+0x12C,
// category R+0x03, owner R+0x18 OR R+0x80 (two conventions; may be neither =
// global effect). z-order falls out of the layer index. Per the mvc2-sh4-re-expert
// disassembly trace (bank03:1200, bank04 slot insert, bank0e Storm super spawns).
static int readAllDrawn(ObjectState* out, int maxObjs)
{
	static const uint32_t SLOT_COUNT_BASE = 0x8C2895E0;
	static const uint32_t SLOT_PTR_BASE   = 0x8C287DE0;
	static const uint32_t SLOT_ROW_STRIDE = 0x180;
	static const int      SLOT_LAYERS     = 16;
	static const int      SLOT_MAX_ROW    = 0x60;
	int n = 0, nPass = 0;
	int nRaw = 0, nInvalid = 0, nVis0 = 0, nSid0 = 0, nOOB = 0;   // diagnostic: where objects are lost
	uint8_t lcounts[16];
	for (int layer = 0; layer < SLOT_LAYERS; layer++) {
		int count = (int)addrspace::read8(SLOT_COUNT_BASE + layer);
		lcounts[layer] = (uint8_t)(count & 0xFF);
		if (count <= 0 || count > SLOT_MAX_ROW) continue;
		uint32_t row = SLOT_PTR_BASE + (uint32_t)layer * SLOT_ROW_STRIDE;
		for (int i = 0; i < count; i++) {
			nRaw++;
			uint32_t node = addrspace::read32(row + i * 4);
			if (node < 0x8C000000 || node >= 0x8D000000) { nInvalid++; continue; }
			// Skip the 6 fighter bodies — they're shipped via GSTA already; the
			// slot table holds them too (avoid double-draw).
			bool isBody = false;
			for (int s = 0; s < 6; s++) if (node == CHAR_BASE[s]) { isBody = true; break; }
			if (isBody) continue;
			if (addrspace::read8(node + 0x12C) == 0) { nVis0++; continue; }     // renderer's visibility gate
			uint16_t sid = (uint16_t)addrspace::read16(node + 0x144);
			if (sid == 0) { nSid0++; continue; }
			float sx = readFloat(node + 0xE0), sy = readFloat(node + 0xE4);
			if (sx < -64.f || sx > 704.f || sy < -64.f || sy > 544.f) { nOOB++; continue; }
			// Owner is OPTIONAL (global effects have none). Check both conventions.
			int slot = -1;
			uint32_t oA = addrspace::read32(node + 0x18), oB = addrspace::read32(node + 0x80);
			for (int s = 0; s < 6; s++) if (oA == CHAR_BASE[s] || oB == CHAR_BASE[s]) { slot = s; break; }
			// Effect-routing flag (GSTA enrich): the node's GFX base (node+0x15c) points
			// into the shared "Effect Poly" bank [0x0CED0000,0x0CEE0000) => route to the
			// effects atlas, not PL{cid}. Both the 0x0C.. (mem_b) and 0x8C.. aliases match.
			uint32_t gfxBase = addrspace::read32(node + OFF_GFX00_PTR);
			uint32_t gfxLow  = gfxBase & 0x0FFFFFFF;   // strip cached/uncached region nibble
			uint8_t  isEfx   = (gfxLow >= 0x0CED0000 && gfxLow < 0x0CEE0000) ? 1 : 0;
			nPass++;
			if (n >= maxObjs) continue;
			out[n].owner_cid  = (uint8_t)(slot >= 0 ? addrspace::read8(CHAR_BASE[slot] + OFF_CHAR_ID) : 0);
			out[n].sprite_id  = sid;
			out[n].screen_x   = (int16_t)sx;
			out[n].screen_y   = (int16_t)sy;
			out[n].type       = (uint8_t)layer;                       // z-order: slot-table layer
			out[n].category   = (uint8_t)addrspace::read8(node + 0x03);
			out[n].xflip      = (uint8_t)(addrspace::read16(node + 0x130) ? 1 : 0);
			out[n].owner_slot = (uint8_t)(slot < 0 ? 0xFF : slot);
			out[n].is_effect  = isEfx;
			// GSTA wire extension: low 16 bits of the GFX base (node+0x15c) — a stable
			// per-effect content key (same gfxBase already read for is_effect).
			out[n].effect_key = (uint16_t)(gfxBase & 0xFFFF);
			// GSTA wire ext: per-object PVR blend/list-type. PREFER the engine's REAL TSP
			// blend (captured at the bank12 submit when MAPLECAST_FRAME_ORACLE_HOOK is on)
			// keyed by this node; fall back to the category heuristic when not captured.
			// THIS is the fix for the Sentinel rocket-TRAIL blob: its category 0x01 maps to
			// "opaque" via computeObjectBlend, but its real TSP blend is additive.
			{ uint8_t rb = maplecast_oracle_hook::mc_oracle_nodeBlend(node);
			  out[n].blend = (rb != 0xFF) ? rb
			               : computeObjectBlend(out[n].is_effect, out[n].category); }
			// PATH A: true assembly hotspot from node+0x178 (0,0 = no extras => client
			// falls back to the baked anchor).
			out[n].hot_dx = 0; out[n].hot_dy = 0;
			readHotspot(node, out[n].hot_dx, out[n].hot_dy);
			n++;
		}
	}
	{
		static int _d = 0;
		if (++_d % 60 == 0) {
			char lb[96]; int p = 0;
			for (int L = 0; L < 16; L++) p += snprintf(lb + p, sizeof(lb) - p, "%d,", lcounts[L]);
			fprintf(stderr, "[OBJS-SLOT] drawn=%d raw=%d inval=%d vis0=%d sid0=%d oob=%d layers=[%s]\n",
			        n, nRaw, nInvalid, nVis0, nSid0, nOOB, lb);
		}
	}
	return n;
}

// PALF — per-slot palette-effect flag (char+0x40 = char_pal_effect, the hit-flash/
// super-glow selector per the bank03:loc_8c035000 on-hit palette path). Nonzero =>
// the body's palette is swapped to the hurt bank (Dat_Pal+0x300); the browser tints
// the victim's body toward white (electric -> blue-white) while it's nonzero. Shipped
// as its OWN packet (not GSTA) so no existing parser is touched. 16 bytes.
int serializePalEffects(uint8_t* out, int maxLen)
{
	if (maxLen < 4 + 6 * 2) return 0;
	out[0] = 'P'; out[1] = 'A'; out[2] = 'L'; out[3] = 'F';
	int off = 4;
	uint16_t pe[6];
	for (int i = 0; i < 6; i++) {
		pe[i] = (uint16_t)addrspace::read16(CHAR_BASE[i] + 0x40);
		out[off++] = pe[i] & 0xff; out[off++] = (pe[i] >> 8) & 0xff;
	}
	// DIAGNOSTIC: log candidate hit-flash fields ONLY in the window right after a real
	// hit (health@0x420 drops) — captures exactly which field changes during the flash.
	// Logs every frame for 14 frames after each hp-drop. Cross-checked vs marvelous2's
	// damage routine loc_8c056454 (writes the reaction state) + loc_8c035162 (reads the
	// palette mode). Also log anim_state@0x1d0 (the palette-mode source) + flash@0x150.
	// HITDIFF: on a real hit (hp drop) log which bytes in 0x100..0x240 change for
	// the victim each frame -> read journalctl + cross-ref to find the flash field
	// (team says char+0x12e). fputc(10) for newline to dodge format-string escapes.
	static uint8_t _prevHp[6] = {0};
	static int     _hitLog[6] = {0};
	static uint8_t _prevR[6][0x140];
	static bool    _prevRok[6] = {false};
	const int RB = 0x100, RL = 0x140;
	for (int i = 0; i < 6; i++) {
		if (addrspace::read8(CHAR_BASE[i] + 0x000) == 0) { _prevHp[i] = 0; _prevRok[i] = false; continue; }
		uint8_t cur[RL];
		for (int b = 0; b < RL; b++) cur[b] = (uint8_t)addrspace::read8(CHAR_BASE[i] + (uint32_t)(RB + b));
		uint8_t hp = addrspace::read8(CHAR_BASE[i] + 0x420);
		if (_prevHp[i] != 0 && hp < _prevHp[i]) _hitLog[i] = 12;
		_prevHp[i] = hp;
		if (_hitLog[i] > 0 && _prevRok[i]) {
			_hitLog[i]--;
			char buf[700]; int q = 0;
			q += snprintf(buf + q, sizeof buf - q, "[HITDIFF] s%d hp=%u chg:", i, hp);
			for (int b = 0; b < RL && q < (int)sizeof buf - 24; b++)
				if (cur[b] != _prevR[i][b])
					q += snprintf(buf + q, sizeof buf - q, " 0x%x:%u>%u", RB + b, _prevR[i][b], cur[b]);
			fprintf(stderr, "%s", buf); fputc(10, stderr);
		}
		memcpy(_prevR[i], cur, RL); _prevRok[i] = true;
	}
	return off;  // 4 + 12 = 16
}

// WTCH - LIVE BIT-PROBE. Ships a configurable char-struct byte range for all 6
// slots so the browser overlay shows fields changing in real time (correlate a
// RAM byte to an on-screen visual without a rebuild). Gated MAPLECAST_WATCH;
// range from MAPLECAST_WATCH_BASE/_LEN (default 0x100/0x140). Read-only.
int serializeWatch(uint8_t* out, int maxLen)
{
	static const bool on = getenv("MAPLECAST_WATCH") != nullptr;
	if (!on) return 0;
	static int base = getenv("MAPLECAST_WATCH_BASE") ? (int)strtol(getenv("MAPLECAST_WATCH_BASE"), nullptr, 0) : 0x100;
	static int len  = getenv("MAPLECAST_WATCH_LEN")  ? (int)strtol(getenv("MAPLECAST_WATCH_LEN"),  nullptr, 0) : 0x140;
	if (len < 1) len = 1; if (len > 512) len = 512;
	int need = 8 + 6 * (2 + len);
	if (maxLen < need) return 0;
	int o = 0;
	out[o++] = 'W'; out[o++] = 'T'; out[o++] = 'C'; out[o++] = 'H';
	out[o++] = base & 0xff; out[o++] = (base >> 8) & 0xff;
	out[o++] = len  & 0xff; out[o++] = (len  >> 8) & 0xff;
	for (int s = 0; s < 6; s++) {
		out[o++] = (uint8_t)addrspace::read8(CHAR_BASE[s] + 0x000);
		out[o++] = (uint8_t)addrspace::read8(CHAR_BASE[s] + 0x001);
		for (int b = 0; b < len; b++)
			out[o++] = (uint8_t)addrspace::read8(CHAR_BASE[s] + (uint32_t)(base + b));
	}
	return o;
}

// Scan the object pool for active satellite objects (cape, effects, projectiles).
// Each owner-pointer object carries sprite_id@+0x12C + screen_x@+0xC8 + screen_y@+0xCC.
// Skips inactive (sid==0) and the body object (no own position, (0,0)).
// See re-catalog/00-README.md + PL2A-storm.md. Reads only.
int readObjects(ObjectState* out, int maxObjs)
{
	static const bool _slot = getenv("MAPLECAST_OBJS_SLOTTABLE") != nullptr;
	if (_slot) return readAllDrawn(out, maxObjs);
	static const bool _walk = getenv("MAPLECAST_OBJS_WALK") != nullptr;
	if (_walk) return readObjectsWalk(out, maxObjs);
	int n = 0, nOwner = 0, nSid = 0, nPass = 0;
	// Scan the ENTIRE pool region — do NOT stop at maxObjs. The MVC2 object pool
	// is itself a finite, fixed-stride region, so the count of drawable objects
	// has a hard ceiling; scanning all of it lets us COUNT every drawable object
	// (nPass) vs how many we can store (n <= maxObjs) and LOG any shortfall.
	// Truncation is then never silent — that was the bug that dropped the supers
	// when both capes filled a too-small cap and the address-ordered scan never
	// reached the higher-address super objects.
	for (uint32_t a = 0x8C26A600; a < 0x8C278000; a += 4) {
		uint32_t v = addrspace::read32(a);
		bool owned = false;
		for (int s = 0; s < 6; s++) if (v == CHAR_BASE[s]) { owned = true; break; }
		if (!owned) continue;
		nOwner++;
		uint16_t sid = (uint16_t)addrspace::read16(a + 0x12C);
		if (sid == 0) continue;
		nSid++;
		// POOLV: state byte @a+0x0C — 0x00 = freed slot. Cheap first reject.
		if (addrspace::read8(a + 0x0C) == 0) continue;
		// NOTE: a doubly-linked "alive" test used to live here (next@a-0x10,
		// prev@a-0x0c). The PTRDUMP capture proved it WRONG: Storm's live
		// multi-segment cape objects read a-0x0c = 0x2a030005 (packed data —
		// 0x2a is her cid — not a record pointer), so the test culled the real
		// cape/hail/lightning objects (only 2 of 39 passed) while the user saw
		// them on screen. These satellites have no clean forward link at that
		// offset, so list-membership can't be cheaply verified from the scanned
		// record. Rely on the validated signals instead: owner == a fighter
		// base, sprite_id != 0, state byte @a+0x0C != 0, and an on-screen
		// position. (Stale/stuck records are filtered by the state byte +
		// off-screen reject below; if a few linger, that's a later refinement.)
		float sx = readFloat(a + 0xC8), sy = readFloat(a + 0xCC);
		if ((sx == 0.f && sy == 0.f) || sx < -64.f || sx > 704.f || sy < -64.f || sy > 544.f) continue;
		nPass++;                     // a real on-screen drawable object — counted even past the cap
		if (n >= maxObjs) continue;  // no slot to store it; the drop guard below makes this visible
		out[n].owner_cid = (uint8_t)addrspace::read8(v + OFF_CHAR_ID);
		out[n].sprite_id = sid;
		out[n].screen_x  = (int16_t)sx;
		out[n].screen_y  = (int16_t)sy;
		out[n].type      = (uint8_t)addrspace::read8(a + 0x0E);
		// All object offsets are kept relative to the SAME anchor `a` the read
		// loop uses (the owner-ptr word), because those are the walk-test-
		// validated offsets (sprite_id a+0x12C, screen a+0xC8). The marvelous2
		// disasm names xflip at record+0x130, adjacent to sprite_id+0x12C — read
		// it at a+0x130 (same anchor). The disasm record+0x3 category is NOT
		// safely reachable from this anchor (the +0x80 vs absolute-base ambiguity
		// is unresolved in re-catalog), so we carry `type`@a+0x0E (validated) and
		// match on owner alone when injecting.
		out[n].category  = (uint8_t)addrspace::read8(a + 0x0E);   // = type (render layer hint)
		out[n].xflip     = (uint8_t)(addrspace::read16(a + 0x130) ? 1 : 0);
		{ int os = 0; for (int s = 0; s < 6; s++) if (v == CHAR_BASE[s]) { os = s; break; } out[n].owner_slot = (uint8_t)os; }
		{ uint32_t gbRaw = addrspace::read32(a + OFF_GFX00_PTR);
		  uint32_t gb = gbRaw & 0x0FFFFFFF;
		  out[n].is_effect = (gb >= 0x0CED0000 && gb < 0x0CEE0000) ? 1 : 0;
		  out[n].effect_key = (uint16_t)(gbRaw & 0xFFFF); }   // GSTA wire ext: low 16 of GFX base
		// GSTA wire ext: per-object PVR blend/list-type. PREFER the engine's REAL TSP blend
		// (captured at the bank12 submit when MAPLECAST_FRAME_ORACLE_HOOK is on); the node
		// base for this legacy owner-anchored scan is a-0x18 (the record anchor used for the
		// hotspot below). Fall back to the category heuristic when not captured. NOTE: the
		// category=type@a+0x0E here is approximate (the disasm +0x03 category is unreachable
		// from this anchor); the captured real blend is exact when present.
		{ uint8_t rb = maplecast_oracle_hook::mc_oracle_nodeBlend(a - 0x18);
		  out[n].blend = (rb != 0xFF) ? rb
		               : computeObjectBlend(out[n].is_effect, out[n].category); }
		// PATH A: legacy owner-anchored scan reads the record at a-0x18; the extras ptr
		// is then (a-0x18)+0x178. Walk it for the hotspot (0,0 = none).
		out[n].hot_dx = 0; out[n].hot_dy = 0;
		readHotspot(a - 0x18, out[n].hot_dx, out[n].hot_dy);
		n++;
	}
	// Truncation guard — NEVER silent. The pool is finite, so a cap >= the pool's
	// drawable maximum means nPass == n on every frame. If this line ever prints,
	// the cap is genuinely too small (or the 255 wire-count limit was hit) and an
	// effect is being dropped — raise the cap / widen the OBJS count field.
	if (nPass > n) {
		fprintf(stderr, "[OBJS] CAP HIT: %d drawable objects, cap=%d -> DROPPED %d (raise the readObjects cap)\n",
		        nPass, maxObjs, nPass - n);
	} else {
		static int _dbg = 0;
		if (++_dbg % 120 == 0)
			fprintf(stderr, "[OBJS] owners=%d withSid=%d shipped=%d (cap=%d)\n", nOwner, nSid, n, maxObjs);
	}
	return n;
}

// Inject the object pool back into RAM — INVERSE of readObjects (OVERWRITE
// MODE). For each wire object we find an already-linked local pool node owned
// by the same character (matched in wire order) and overwrite its visible
// fields (sprite_id / screen_x/y / xflip) at the SAME `a`-relative offsets the
// read uses. This makes the game's pool render walker draw the server's truth
// for objects the local SH4 already keeps alive (cape, idle aura — animation-
// spawned, present even under neutral input).
//
// LIMITATION (measured, not a bug): nodes the local SH4 never allocated
// (input-driven projectiles / supers under FREEZE) have no slot to write into
// and stay MISSING. Closing that gap needs node SYNTHESIS — allocate a free
// node (marvelous2 free-list head 0x8C287A54, count 0x8C287AE8) and splice it
// into the per-category render head-list (heads[] 0x8C287A5C, chained via +0xC;
// walker loc_8c0301ce). That is the next region to add once this overwrite pass
// is visually confirmed. Returns the number of nodes written.
// Enumerate the local pool's PASSING nodes (exactly the set + order readObjects
// produces) into an address array. Returns the count.
static int enumLiveNodes(uint32_t* outAddr, uint8_t* outCid, int maxN)
{
	int n = 0;
	for (uint32_t a = 0x8C26A600; a < 0x8C278000 && n < maxN; a += 4) {
		uint32_t v = addrspace::read32(a);
		bool owned = false;
		for (int s = 0; s < 6; s++) if (v == CHAR_BASE[s]) { owned = true; break; }
		if (!owned) continue;
		uint16_t sid = (uint16_t)addrspace::read16(a + 0x12C);
		if (sid == 0) continue;
		if (addrspace::read8(a + 0x0C) == 0) continue;
		{
			uint32_t myRec = a - 0x18;
			uint32_t nx = addrspace::read32(a - 0x10), pv = addrspace::read32(a - 0x0c);
			bool linked = (nx >= 0x8C26A000 && nx < 0x8C278000 && addrspace::read32(nx + 0x0c) == myRec)
			           || (pv >= 0x8C26A000 && pv < 0x8C278000 && addrspace::read32(pv + 0x08) == myRec);
			if (!linked) continue;
		}
		float lsx = readFloat(a + 0xC8), lsy = readFloat(a + 0xCC);
		if ((lsx == 0.f && lsy == 0.f) || lsx < -64.f || lsx > 704.f || lsy < -64.f || lsy > 544.f) continue;
		outAddr[n] = a;
		outCid[n]  = (uint8_t)addrspace::read8(v + OFF_CHAR_ID);
		n++;
	}
	return n;
}

int writeObjects(const ObjectState* objs, int n)
{
	if (n <= 0) return 0;

	// Snapshot the SAME passing-node set readObjects sees, in the same order.
	uint32_t nodeAddr[64]; uint8_t nodeCid[64];
	int nNodes = enumLiveNodes(nodeAddr, nodeCid, 64);

	bool used[64] = {false};
	int written = 0;
	for (int oi = 0; oi < n; oi++) {
		const ObjectState& want = objs[oi];
		// First-fit: claim the first unused live node owned by the same char.
		for (int li = 0; li < nNodes; li++) {
			if (used[li] || nodeCid[li] != want.owner_cid) continue;
			uint32_t a = nodeAddr[li];
			addrspace::write16(a + 0x12C, want.sprite_id);
			writeFloat(a + 0xC8, (float)want.screen_x);
			writeFloat(a + 0xCC, (float)want.screen_y);
			addrspace::write8(a + 0x130, want.xflip);
			used[li] = true;
			written++;
			break;
		}
	}
	return written;
}

// Inject the animated-stage / background timing so the stage renders from the
// server's truth rather than the local SH4's. frame_counter drives most stage
// animation timing; stage_anim_timer is the dedicated monotonic counter.
void writeStageState(uint32_t frame_counter, uint8_t stage_anim_timer)
{
	addrspace::write32(ADDR_FRAME_CTR, frame_counter);
	addrspace::write8(ADDR_STAGE_ANIM, stage_anim_timer);
}

uint8_t readStageAnimTimer()
{
	return (uint8_t)addrspace::read8(ADDR_STAGE_ANIM);
}

// 'OBJF' full-object packet (state-replica inject). count(1) + N*10. No magic.
int serializeObjects(const ObjectState* objs, int n, uint8_t* buf, int maxLen)
{
	if (n < 0) n = 0;
	if (n > 255) n = 255;
	if (maxLen < 1 + n * OBJF_REC_SIZE) return 0;
	int o = 0;
	buf[o++] = (uint8_t)n;
	for (int i = 0; i < n; i++) {
		const ObjectState& s = objs[i];
		buf[o++] = s.owner_cid;
		buf[o++] = (uint8_t)(s.sprite_id & 0xff);
		buf[o++] = (uint8_t)((s.sprite_id >> 8) & 0xff);
		buf[o++] = s.type;
		buf[o++] = s.category;
		buf[o++] = s.xflip;
		buf[o++] = s.owner_slot;
		buf[o++] = (uint8_t)(s.screen_x & 0xff);
		buf[o++] = (uint8_t)((s.screen_x >> 8) & 0xff);
		buf[o++] = (uint8_t)(s.screen_y & 0xff);
		buf[o++] = (uint8_t)((s.screen_y >> 8) & 0xff);
		buf[o++] = s.is_effect;
		buf[o++] = (uint8_t)s.hot_dx;
		buf[o++] = (uint8_t)s.hot_dy;
		buf[o++] = (uint8_t)(s.effect_key & 0xff);          // GSTA wire ext: effect_key u16 LE
		buf[o++] = (uint8_t)((s.effect_key >> 8) & 0xff);
		buf[o++] = s.blend;                                 // GSTA wire ext: blend/list-type u8
	}
	return o;
}

int deserializeObjects(const uint8_t* buf, int len, ObjectState* out, int maxObjs)
{
	if (len < 1) return 0;
	int n = buf[0];
	int o = 1;
	int got = 0;
	for (int i = 0; i < n && got < maxObjs && o + OBJF_REC_SIZE <= len; i++) {
		ObjectState& s = out[got];
		s.owner_cid = buf[o++];
		s.sprite_id = (uint16_t)(buf[o] | (buf[o+1] << 8)); o += 2;
		s.type      = buf[o++];
		s.category  = buf[o++];
		s.xflip     = buf[o++];
		s.owner_slot= buf[o++];
		s.screen_x  = (int16_t)(buf[o] | (buf[o+1] << 8)); o += 2;
		s.screen_y  = (int16_t)(buf[o] | (buf[o+1] << 8)); o += 2;
		s.is_effect = buf[o++];
		s.hot_dx    = (int8_t)buf[o++];
		s.hot_dy    = (int8_t)buf[o++];
		s.effect_key = (uint16_t)(buf[o] | (buf[o+1] << 8)); o += 2;   // GSTA wire ext
		s.blend = buf[o++];                                            // GSTA wire ext: blend/list-type u8
		got++;
	}
	return got;
}

// =============================================================================
// PARTDUMP — assembly-driven runtime part-atlas capture (gated MAPLECAST_PARTDUMP).
//
// Captures the CURRENT frame's decoded sprite parts for an active character from
// the DM00 Poly directory in SH4 main RAM, resolving each EXTRAS part_idx to its
// directory entry via the EXACT mapping read from the marvelous2 disassembly (NOT
// the old dimension-match guess, which was structurally wrong — the directory is a
// live working set, not a per-character contiguous array of all a char's parts).
//
// DM00 Poly directory (decoded part textures, PVR-format):
//   dir_base = *(0x0CE80008)            ; header+8 (in mem_b, NOT VRAM)
//   stride   = 0x10 bytes per entry
//   entry+0x0 = (w,h) packed as two u16 PIXELS (w = low half, h = high half)
//   entry+0x4 = u32  (format/key field — DUMPED RAW so we can confirm semantics)
//   entry+0x8 = pointer to twiddled texels (in mem_b)
//   entry+0xc = u32  (key/source field — DUMPED RAW so we can confirm semantics)
//
// THE part_idx -> directory-entry MAPPING (resolved from the disassembly).
//   The per-part fetch wrapper bank03:loc_8c0322c0 computes, for a directory key
//   `k` and the dir struct in r6:
//       entry = *(r6 + 0x8) + (k << 4)        ; (k << 4) == k * 0x10 (the stride)
//       dest  = *(entry + 0x8)                ; the part's decoded texel slot
//   The body-parts loader bank03:loc_8c032ae0 calls it with the key = a SIMPLE
//   INCREMENTING COUNTER `r11` (k = char_base, char_base+1, ... one per part), and
//   char_base is chosen per character at loc_8c032a66 from the player struct byte
//   at +0xad (a slot/side selector): default 9, or 13 (0x0D) when +0xad==1, etc.
//   So:
//       dir_entry(char, part_idx) = dir_base + (char_base + part_idx) * 0x10
//   with char_base a SMALL FIXED CONSTANT (≈8/9/13) from the char's +0xad selector
//   — NOT something to dimension-match. Each loaded entity gets its own contiguous
//   key run; HUD/stage occupy other key ranges of the same directory.
//
//   This probe does NOT guess: it reads char+0xad to pick the candidate char_base,
//   then VALIDATES it by checking that dir_entry(part 0..) dims are non-empty and in
//   range (and logs a small ±window so the exact base is visible if +0xad mapping
//   differs for assists). It then walks the CHARACTER'S CURRENT sprite_id assembly
//   from the live EXTRAS (player+0x178), and for each record's part_idx dumps
//   dir_entry(char_base+part_idx). First test = current frame's parts only; the full
//   atlas accumulates these across frames (FOLLOW-UP — see docs/ASSEMBLY-DRIVEN-DESIGN).
//
//   The probe ALSO dumps the WHOLE directory with ALL FOUR u32 fields per entry, so
//   the +0x4 / +0xc semantics (format vs source-key) are visible in the log and we
//   can lock the mapping. Read-only; ROM-derived pixels -> /dev/shm only.
// =============================================================================
static inline bool _ramAddr(uint32_t a) {
	return (a >= 0x0C000000 && a < 0x0D000000) || (a >= 0x8C000000 && a < 0x8D000000);
}

// Dump the RAW texels of a part straight from the texel pointer — w*h*2 bytes, NO
// decode, NO twiddle. This lets the format/twiddle be locked OFFLINE (iterate
// ARGB1555/RGB565/ARGB4444 x {twiddled,linear,non-square}) without a redeploy per guess.
// Dump `nbytes` raw bytes straight from the texel pointer (no decode, no twiddle).
// Byte count is format-dependent: 16-bit = w*h*2, PAL8 = w*h, PAL4 = w*h/2. The
// offline locker / packer reads the format from the manifest e4 to interpret these.
static void partDumpRawN(uint32_t texPtr, int nbytes, const char* fn) {
	FILE* rf = fopen(fn, "wb");
	if (!rf) return;
	for (int i = 0; i < nbytes; i++)
		fputc((uint8_t)addrspace::read8(texPtr + (uint32_t)i), rf);
	fclose(rf);
}

// EFFECTS DUMP — capture the SHARED "Effect Poly" bank at 0x0CED0000 (gated
// MAPLECAST_DUMP_EFFECTS). These are the global hitspark/electric/super sprites
// that live OUTSIDE any PLxx_DAT (work.asm:39 "0ced0000 - Effect Poly"; loaded
// by bank0e:loc_8C0EEFCC). Mirrors PARTDUMP/DM00: the directory base is assumed
// at *(0x0CED0008) (region+8, exactly like DM00 at 0x0CE80008). This is an
// EXPLORATORY probe — it logs whatever is at that directory so we can confirm
// the layout, and dumps each entry's raw texels (format locked offline like
// parts). Read-only; ROM-derived pixels -> /dev/shm only.
static void effectsDump(const GameState& state) {
	static const char* env = getenv("MAPLECAST_DUMP_EFFECTS");
	static bool on = env != nullptr;
	static int budget = (env && atoi(env) > 0) ? atoi(env) : 1;
	static int fires = 0;
	static uint32_t skip = 0;
	if (!on || fires >= budget || !state.in_match) return;
	if (budget > 1 && (++skip % 8) != 0) return;

	// Probe the candidate directory bases (region+8, like DM00). Log both the
	// 0x0C... (mem_b) and 0x8C... aliases so the real one is visible.
	uint32_t dirBase = addrspace::read32(0x0CED0008);
	if (!_ramAddr(dirBase)) { uint32_t alt = addrspace::read32(0x8CED0008); if (_ramAddr(alt)) dirBase = alt; }
	fires++;
	FILE* lg = fopen("/dev/shm/mc_effects.log", fires == 1 ? "w" : "a");
	if (lg) fprintf(lg, "# MapleCast EFFECTS DUMP — Effect Poly 0x0CED0000 (fire %d/%d) frame=%u\n"
	                    "raw@0x0CED0008=%08x raw@0x8CED0008=%08x  -> dirBase=%08x  ramOK=%d\n",
	                fires, budget, state.frame_counter,
	                addrspace::read32(0x0CED0008), addrspace::read32(0x8CED0008), dirBase, (int)_ramAddr(dirBase));
	if (!_ramAddr(dirBase)) { if (lg) { fprintf(lg, "[EFX] directory not built yet\n"); fclose(lg); } return; }

	const int MAXDIR = 512;
	int dirN = 0, dirBlanks = 0;
	if (lg) fprintf(lg, "\n[EFX] idx  wxh         e0        e4        tex(e8)   ec\n");
	for (int i = 0; i < MAXDIR; i++) {
		uint32_t e  = dirBase + (uint32_t)i * 0x10;
		uint32_t e0 = addrspace::read32(e),     e4 = addrspace::read32(e + 4);
		uint32_t e8 = addrspace::read32(e + 8), ec = addrspace::read32(e + 12);
		uint16_t w = e0 & 0xffff, h = (e0 >> 16) & 0xffff;
		bool blank = (w == 0 && h == 0 && !_ramAddr(e8));
		if (blank) { if (++dirBlanks > 32) break; continue; }
		dirBlanks = 0;
		if (lg) fprintf(lg, "[EFX] %4d %4dx%-4d  %08x  %08x  %08x  %08x\n", i, w, h, e0, e4, e8, ec);
		if (_ramAddr(e8) && w > 0 && h > 0 && w <= 512 && h <= 512) {
			char fn[96]; snprintf(fn, sizeof fn, "/dev/shm/efx_%03d.raw", i);
			partDumpRawN(e8, (int)w * (int)h * 2, fn);   // 16-bit assumed; format locked offline
		}
		dirN = i + 1;
	}
	if (lg) { fprintf(lg, "[EFX] scanned %d entries\n", dirN); fclose(lg); }
}

// FOLLOW THE DESCRIPTOR — resolve a part's real PVR format/TCW the way the game does.
// `e4` (DM00 entry+0x4) is a DESCRIPTOR INDEX, not the format. Traced clean-room from the
// per-entry texture builder bank12:loc_8c123e00 (driven over the directory by
// loc_8c1240a0). For directory key `k` (= charBase + part_idx, the loop counter):
//   t1   = *(0x8C2DAD3C)              ; ptr to a u16 index table
//   u16  = t1[k]                      ; mov.w @(table+k*2)  (k=r10, loc_8c123e56)
//   t2   = *(0x8C2DAD4C)              ; ptr to a 0x20-stride descriptor table
//   desc = t2 + u16*0x20             ; (r13 = r14<<5 + *(0x8C2DAD4C), loc_8c123eee)
//   TCW  = *(desc + 0x0C)            ; mov.l @(0x0C,r13)
//   fmt  = (TCW >> 27) & 7           ; mov 0xE5,r3; shld r3,r0; and 0x07  (PVR PixelFmt)
//   scan = (TCW >> 26) & 1           ; ScanOrder: 1=linear/strided, 0=twiddled
// PVR PixelFmt (ta_structs.h): 0=1555 1=565 2=4444 5=PAL4 6=PAL8. Also dumps the 0x20-byte
// descriptor + the u16 + the resolved TCW so the format is verifiable offline. Returns the
// resolved TCW (0 if any pointer was out of RAM — caller falls back to e4-byte1).
static uint32_t partResolveTCW(int key, uint32_t* outDescAddr, uint16_t* outU16) {
	const uint32_t P_T1 = 0x8C2DAD3C;   // -> u16 index table
	const uint32_t P_T2 = 0x8C2DAD4C;   // -> 0x20-stride descriptor table
	if (outDescAddr) *outDescAddr = 0;
	if (outU16) *outU16 = 0xFFFF;
	uint32_t t1 = addrspace::read32(P_T1);
	uint32_t t2 = addrspace::read32(P_T2);
	if (!_ramAddr(t1) || !_ramAddr(t2)) return 0;
	uint16_t u16 = (uint16_t)addrspace::read16(t1 + (uint32_t)key * 2);
	if (u16 == 0xFFFF) return 0;                      // 0xFF/unused slot
	uint32_t desc = t2 + (uint32_t)u16 * 0x20;
	if (!_ramAddr(desc)) return 0;
	if (outDescAddr) *outDescAddr = desc;
	if (outU16) *outU16 = u16;
	return addrspace::read32(desc + 0x0C);            // the PVR TCW
}
// PVR-TCW -> our part fmt code (0/1/2/5/6) + scan order. Identical bit layout to the
// VRAM path (mcfx) and flycast `union TCW`.
static inline int  tcwFmt(uint32_t tcw)    { return (int)((tcw >> 27) & 7); }
static inline bool tcwLinear(uint32_t tcw) { return ((tcw >> 26) & 1) != 0; }

// The DM00 Poly directory entry (0x10 bytes) carries the part FORMAT at +0x4 (e4),
// NOT +0xC (ec is 0 in the real data). Confirmed from the per-entry texture builder
// bank12:loc_8c123e00 (driven over the 0x10-stride directory by bank12:loc_8c1240a0,
// which terminates on entry+0x4 == 0xFF): it reads `idx = *(entry+0x4)` and uses it as
// `descriptor = global_tex_table + idx*0x3C` (`mov.l @(0x04,r3),r13; mul.l 0x3C,r13`)
// — so e4 is a small format/descriptor code, and the decode loop bank03:loc_8c032734
// derives the same {1,2,3} code from a per-part descriptor byte. The texel pointer is
// entry+0x8. (The earlier ec/TCW read was the VRAM-UPLOAD builder loc_8C122D00, a
// different structure — ec on the DM00 directory is 0.)
//
// e4 ENCODING — the format selector is e4 BYTE 1 ((e4>>8)&0xff); byte 0 (0x01) is a
// constant "present" flag. Empirically locked + consistent with the disasm classifier:
//   0x0101 -> 32x32 parts decode clean as RGB565  (byte1 = 0x01)
//   0x0301 -> 256x256 body is PALETTED, 8bpp PAL8 (byte1 = 0x03)  [operator-confirmed]
// PVR PixelFormat codes (ta_structs.h): 0=ARGB1555 1=RGB565 2=ARGB4444 5=PAL4 6=PAL8.
// The raw texel byte SIZE (dumped) disambiguates PAL4 (w*h/2) vs PAL8 (w*h) offline if
// ever ambiguous. e4 is written to the manifest so the packer derives the same format.
static int partFmtFromE4(uint32_t e4) {
	uint8_t sel = (uint8_t)((e4 >> 8) & 0xff);
	switch (sel) {
		case 0x00: return 0;     // ARGB1555
		case 0x01: return 1;     // RGB565   (32x32 parts — decoded clean)
		case 0x02: return 2;     // ARGB4444
		case 0x03: return 6;     // PAL8  (256x256 body — operator-confirmed paletted)
		case 0x04: return 5;     // PAL4
		default:   return 1;     // unknown -> RGB565 (preview best-effort)
	}
}

// Twiddle (Morton) index — EXACT port of flycast's core/rend/texconv.cpp `detwiddle`+`twop`
// (the same math mcfx uses to decode real VRAM in maplecast_mirror.cpp). The PVR
// interleaves **y-bit first, then x** per pair:
//   detwiddle[0][s][i] = twiddle_slow(i,0, 1024, 1<<s)   (x bits, depth gated by y size)
//   detwiddle[1][s][i] = twiddle_slow(0,i, 1<<s, 1024)   (y bits, depth gated by x size)
//   twop(x,y,bcx,bcy)  = detwiddle[0][bcy][x] + detwiddle[1][bcx][y]   (bc = bitscanrev)
// `swapXY=true` selects the transposed (x-first) order — the previous hand-rolled
// behaviour, which is a transpose of the canonical order for square textures. The small
// parts decoded under the old order; this lets the probe emit BOTH so the 256x256 body
// can be A/B'd offline against the oracle without a redeploy.
static uint32_t s_detwiddle[2][11][1024];
static bool s_detwInit = false;
static uint32_t twiddle_slow(uint32_t x, uint32_t y, uint32_t x_sz, uint32_t y_sz) {
	uint32_t rv = 0, sh = 0; x_sz >>= 1; y_sz >>= 1;
	while (x_sz != 0 || y_sz != 0) {
		if (y_sz != 0) { rv |= (y & 1) << sh; y_sz >>= 1; y >>= 1; sh++; }
		if (x_sz != 0) { rv |= (x & 1) << sh; x_sz >>= 1; x >>= 1; sh++; }
	}
	return rv;
}
static void initDetwiddle() {
	for (uint32_t s = 0; s < 11; s++) {
		uint32_t y_sz = 1u << s;
		for (uint32_t i = 0; i < 1024; i++) {
			s_detwiddle[0][s][i] = twiddle_slow(i, 0, 1024, y_sz);
			s_detwiddle[1][s][i] = twiddle_slow(0, i, y_sz, 1024);
		}
	}
	s_detwInit = true;
}
static inline int bitscanrev(int v) { int r = 0; while ((1 << (r + 1)) <= v) r++; return r; }
static inline uint32_t partTwiddleIdx2(int x, int y, int w, int h, bool swapXY) {
	if (!s_detwInit) initDetwiddle();
	int bcx = bitscanrev(w), bcy = bitscanrev(h);
	if (swapXY) return s_detwiddle[0][bcx][y] + s_detwiddle[1][bcy][x];  // transposed (x-first)
	return s_detwiddle[0][bcy][x] + s_detwiddle[1][bcx][y];              // flycast-canonical (y-first)
}
static inline uint32_t partTwiddleIdx(int x, int y, int w, int h, int /*sq*/, int /*sqBits*/) {
	return partTwiddleIdx2(x, y, w, h, false);
}

// Decode one DM00 Poly part -> PPM preview, dispatching on the e4-derived PVR format.
// fmt: 0/1/2 = 16-bit direct (texels at texPtr); 5/6 = paletted (4/8bpp index ->
// palBase). palBase is the live Dat_Pal (player+0x164) ARGB4444 palette in mem_b for
// the paletted path; index 0 = transparent. Transparent texels emit magenta (PPM has
// no alpha; the packer keys magenta -> alpha 0). This is a best-effort PREVIEW — the
// authoritative pixels come from the .raw dump + the offline packer's per-part decode.
static void partDecodeToPPM(uint32_t texPtr, int w, int h, int fmt, bool linear, uint32_t palBase, const char* fn, bool swapXY = false) {
	FILE* pf = fopen(fn, "wb");
	if (!pf) return;
	fprintf(pf, "P6\n%d %d\n255\n", w, h);
	bool paletted = (fmt == 5 || fmt == 6);
	for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
		// ScanOrder bit (TCW bit 26) -> linear (row-major) vs PVR twiddled (Morton).
		uint32_t idx = linear ? (uint32_t)(y * w + x) : partTwiddleIdx2(x, y, w, h, swapXY);
		uint8_t rr = 0, gg = 0, bb = 0, aa = 0;
		if (!paletted) {
			uint16_t px = (uint16_t)addrspace::read16(texPtr + idx * 2);
			if (fmt == 1) {            // RGB565
				rr = ((px >> 11) & 0x1f) << 3; gg = ((px >> 5) & 0x3f) << 2; bb = (px & 0x1f) << 3; aa = 255;
			} else if (fmt == 2) {     // ARGB4444
				aa = ((px >> 12) & 0xf) * 17; rr = ((px >> 8) & 0xf) * 17; gg = ((px >> 4) & 0xf) * 17; bb = (px & 0xf) * 17;
			} else {                   // ARGB1555 (fmt 0)
				aa = (px & 0x8000) ? 255 : 0;
				rr = ((px >> 10) & 0x1f) << 3; gg = ((px >> 5) & 0x1f) << 3; bb = (px & 0x1f) << 3;
			}
		} else {
			// Paletted: fetch the texel index, look it up in the live ARGB4444 palette.
			uint32_t pidx;
			if (fmt == 5) {            // PAL4: two indices/byte
				uint8_t bytev = (uint8_t)addrspace::read8(texPtr + (idx >> 1));
				pidx = (idx & 1) ? (bytev >> 4) : (bytev & 0xf);
			} else {                   // PAL8
				pidx = (uint8_t)addrspace::read8(texPtr + idx);
			}
			if (pidx == 0 || !_ramAddr(palBase)) { aa = 0; }    // index 0 = transparent
			else {
				uint16_t pe = (uint16_t)addrspace::read16(palBase + pidx * 2);  // ARGB4444 LE
				aa = ((pe >> 12) & 0xf) * 17; rr = ((pe >> 8) & 0xf) * 17; gg = ((pe >> 4) & 0xf) * 17; bb = (pe & 0xf) * 17;
			}
		}
		// PPM has no alpha; emit magenta for fully-transparent so the atlas tool can key it.
		if (aa == 0) { rr = 0xff; gg = 0x00; bb = 0xff; }
		uint8_t rgb[3] = {rr, gg, bb}; fwrite(rgb, 1, 3, pf);
	}
	fclose(pf);
}

static void partDump(const GameState& state) {
	// MAPLECAST_PARTDUMP=1 -> single-frame capture; MAPLECAST_PARTDUMP=N -> capture
	// every ~8th in-match frame for N fires (accumulates the full atlas across frames
	// as the assembly changes). The manifest is APPENDED to, cleared once per process.
	static const char* env = getenv("MAPLECAST_PARTDUMP");
	static bool on = env != nullptr;
	static int budget = (env && atoi(env) > 0) ? atoi(env) : 1;
	static int fires = 0;
	static uint32_t skip = 0;
	static bool cleared = false;
	if (!on || fires >= budget || !state.in_match) return;
	if (budget > 1 && (++skip % 8) != 0) return;   // ~every 8 frames for multi-capture

	uint32_t dirBase = addrspace::read32(0x0CE80008);
	if (!_ramAddr(dirBase)) { uint32_t alt = addrspace::read32(0x8CE80008); if (_ramAddr(alt)) dirBase = alt; }
	if (!_ramAddr(dirBase)) return;       // directory not built yet — wait for char load

	// On first fire of this process, clear stale manifests + sid-trace so append starts fresh.
	if (!cleared) {
		for (int c = 0; c < 0x40; c++) {
			char mn[96];
			snprintf(mn, sizeof mn, "/dev/shm/PL%02X_parts.manifest", c); remove(mn);
			snprintf(mn, sizeof mn, "/dev/shm/PL%02X_sidasm.txt", c);      remove(mn);
		}
		remove("/dev/shm/mc_partdump_sidtrace.log");
		cleared = true;
	}
	fires++;

	FILE* lg = fopen("/dev/shm/mc_partdump.log", "w");
	if (lg) fprintf(lg, "# MapleCast PARTDUMP — disasm-derived part capture (fire %d/%d)\n"
	                    "dirBase=0x%08x frame=%u\n", fires, budget, dirBase, state.frame_counter);

	// ---- 1. Dump the WHOLE directory with ALL FOUR u32 fields per entry so the
	//         +0x4 / +0xc semantics (format vs source-key) are visible. ----
	const int MAXDIR = 2048;
	if (lg) fprintf(lg, "\n[DIR] idx  wxh        e0:dims    e4         tex(e8)    ec\n");
	int dirN = 0, dirBlanks = 0;
	for (int i = 0; i < MAXDIR; i++) {
		uint32_t e  = dirBase + (uint32_t)i * 0x10;
		uint32_t e0 = addrspace::read32(e),     e4 = addrspace::read32(e + 4);
		uint32_t e8 = addrspace::read32(e + 8), ec = addrspace::read32(e + 12);
		uint16_t w = e0 & 0xffff, h = (e0 >> 16) & 0xffff;
		bool blank = (w == 0 && h == 0 && !_ramAddr(e8));
		if (blank) { if (++dirBlanks > 64) break; }   // stop after a long blank run
		else dirBlanks = 0;
		if (lg) fprintf(lg, "[DIR] %4d %4dx%-4d  %08x  %08x  %08x  %08x\n", i, w, h, e0, e4, e8, ec);
		dirN = i + 1;
	}
	if (lg) fprintf(lg, "[DIR] scanned %d entries\n", dirN);

	// ---- 2. Per active character: resolve char_base from the DISASSEMBLY mapping
	//         (char struct byte +0xad selects a small fixed base; default 9, 13 when
	//         +0xad==1). Validate by checking dir_entry(base+0) has non-empty dims;
	//         log a ±8 base window so the exact base is visible if assists differ.
	//         Then walk the CURRENT sprite_id assembly from the live EXTRAS and dump
	//         each referenced part via dir_entry(char_base + part_idx). ----
	const uint32_t OFF_SLOT_SEL = 0x0ad;     // disasm loc_8c032a66/loc_8c032ba2 selector
	int dumpedTotal = 0;
	for (int s = 0; s < 6; s++) {
		uint32_t pbase = CHAR_BASE[s];
		if (!(uint8_t)addrspace::read8(pbase + OFF_ACTIVE)) continue;
		uint8_t  cid  = (uint8_t)addrspace::read8(pbase + OFF_CHAR_ID);
		uint32_t gfx1 = addrspace::read32(pbase + OFF_GFX00_PTR);   // 0x15c
		uint32_t exP  = addrspace::read32(pbase + OFF_EXTRAS_PTR);  // 0x178
		uint32_t palP = addrspace::read32(pbase + OFF_PAL_PTR);     // 0x164 Dat_Pal (paletted parts)
		uint16_t sid  = (uint16_t)addrspace::read16(pbase + OFF_SPRITE_ID);
		uint8_t  sel  = (uint8_t)addrspace::read8(pbase + OFF_SLOT_SEL);
		// disasm: base = (sel==1)?13 : 9 (loc_8c032a66). Assists/other selectors may
		// shift it; we log a window so the true base is identifiable in the dump.
		int charBase = (sel == 1) ? 13 : 9;
		if (lg) fprintf(lg, "\n[CHAR] slot%d cid=%u(PL%02X) sid=%u sel@0xad=%u => charBase=%d "
		                    "GFX1=0x%08x EXTRAS=0x%08x\n",
		                s, cid, cid, sid, sel, charBase, gfx1, exP);

		// Validation window: show dir entries around the candidate base (±8) with dims.
		if (lg) {
			fprintf(lg, "[CHAR] base window (key: dims tex):\n");
			for (int b = charBase - 4; b <= charBase + 24; b++) {
				if (b < 0) continue;
				uint32_t e = dirBase + (uint32_t)b * 0x10;
				uint32_t e0 = addrspace::read32(e), e8 = addrspace::read32(e + 8);
				fprintf(lg, "[CHAR]   key %3d: %3dx%-3d tex=%08x%s\n",
				        b, e0 & 0xffff, (e0 >> 16) & 0xffff, e8, b == charBase ? "  <= base" : "");
			}
		}

		// Follow the live animation/assembly pointers FRESH this fire and log them with
		// the current sprite_id so we can see the assembly track sid across frames.
		//   0x144 sprite_id (already have `sid`), 0x154 current_cell_data (anim cursor),
		//   0x178 EXTRAS base (whole assembly table; per-cell sub-assembly selected by sid).
		uint32_t cellP = addrspace::read32(pbase + 0x154);
		if (lg) fprintf(lg, "[CHAR] live sid=%u cell(0x154)=0x%08x extras(0x178)=0x%08x\n",
		                sid, cellP, exP);

		// Dump the live EXTRAS region (16KB) for offline assembly grouping.
		if (_ramAddr(exP)) {
			char en[96]; snprintf(en, sizeof en, "/dev/shm/PL%02X_extras.bin", cid);
			FILE* ef = fopen(en, "wb");
			if (ef) { for (int k = 0; k < 0x4000; k++) fputc((uint8_t)addrspace::read8(exP + k), ef); fclose(ef); }
			if (lg) fprintf(lg, "[CHAR] EXTRAS -> %s (16KB)\n", en);
		}

		// Dump the live Dat_Pal palette (player+0x164) so the offline packer can decode
		// PALETTED parts (fmt 5/6). ARGB4444 LE, 16 colors/bank; we dump 128 banks
		// (0x1000 bytes) to cover every skin/sub-palette variant. Index 0 = transparent.
		if (_ramAddr(palP)) {
			char pn[96]; snprintf(pn, sizeof pn, "/dev/shm/PL%02X_palette.bin", cid);
			FILE* pf2 = fopen(pn, "wb");
			if (pf2) { for (int k = 0; k < 0x1000; k++) fputc((uint8_t)addrspace::read8(palP + k), pf2); fclose(pf2); }
			if (lg) fprintf(lg, "[CHAR] PALETTE(0x164=%08x) -> %s (4KB, ARGB4444)\n", palP, pn);
		}

		// sprite_id -> ASSEMBLY RE-KEY (Gap 2) — THE CRACKED GFX2 CELL-TABLE WALK.
		// FIXED 2026-06-09: the per-pose part list is GFX2[sid] (node+0x160), NOT the old
		// degenerate EXTRAS slot table (exP + slot*0x400 + 8). The old `slot*0x400` stride
		// was UNCONFIRMED and returned only the body's 1-2 EXTRAS records, so every fire
		// logged `parts=0 1` and the offline tool got 6-19. The GFX2 read (matching
		// tools/rip_gfx2_assembly.py read_cells() and bank03 loc_8c0344d4, §3a) is:
		//   gfx2  = read32(player+0x160)                       ; GFX2 base (Dat_GFX2)
		//   cell  = gfx2 + read32(gfx2 + (sid & 0x7FFF)*4)     ; per-sid cell record
		//   count = read16(cell)                               ; record COUNT (6-19 for a body pose)
		//   recs  = cell + 2                                   ; COUNT * 8-byte records
		// `cell` (player+0x154) is still read for the live keyframe trace (kf[4]==sid), but
		// it is NO LONGER the source of the assembly records.
		//
		// File PL{hex}_sidasm.txt: "<sid> <cnt> <nrecs> penX,penY,part,flip,palRow;...".
		// dx/dy are accumulated into a running pen (§3a). If the count is 0 the GFX2 cell
		// was unresolved (logged so the gfx2 pointer is verifiable).
		bool liveUsePart[512] = {false};   // part_idx referenced by the LIVE assembly (merged into usePart); u16 indices reach ~277
		{
			uint32_t animP = addrspace::read32(pbase + 0x168);   // ANIMATION base (dump for ref)
			if (_ramAddr(animP)) {
				char an[96]; snprintf(an, sizeof an, "/dev/shm/PL%02X_anim.bin", cid);
				FILE* af = fopen(an, "wb");
				if (af) { for (int k = 0; k < 0x8000; k++) fputc((uint8_t)addrspace::read8(animP + k), af); fclose(af); }
			}

			uint16_t sid144  = (uint16_t)addrspace::read16(pbase + 0x144);   // client's key (cross-check)
			uint16_t liveSid = sid144;
			char kfbytes[64] = "-";
			if (_ramAddr(cellP)) {
				liveSid = (uint16_t)addrspace::read16(cellP + 4);            // keyframe[4] == sid144
				int n = 0; for (int b = 0; b < 20 && n < 60; b++) n += snprintf(kfbytes + n, sizeof(kfbytes) - n, "%02x", (uint8_t)addrspace::read8(cellP + b));
			}

			// THE PER-POSE PART LIST = the CRACKED GFX2 CELL-TABLE walk (node+0x160),
			// indexed by sid144. Supersedes the degenerate EXTRAS slot table
			// (exP + slot*0x400 + 8) which returned only the body's 1-2 records.
			//   gfx2  = *(node+0x160) ; cell = gfx2 + *(u32)(gfx2 + (sid&0x7FFF)*4)
			//   count = u16 @ cell    ; COUNT * 8-byte records follow at cell+2
			// Matches tools/rip_gfx2_assembly.py read_cells() and bank03 loc_8c0344d4
			// (MARVELOUS2-GFX-NOTES §3a).
			uint32_t kfGfx2  = addrspace::read32(pbase + OFF_GFX01_PTR) & 0x0FFFFFFFu;
			uint32_t recs    = 0;
			int      recCnt  = 0;
			if (_ramAddr(kfGfx2 | 0x0C000000u)) {
				uint32_t off  = addrspace::read32(kfGfx2 + (uint32_t)(sid144 & 0x7FFF) * 4) & 0x0FFFFFFFu;
				uint32_t cell = (kfGfx2 + off) & 0x0FFFFFFFu;
				if (_ramAddr(cell | 0x0C000000u)) {
					int cnt = (int)(uint16_t)addrspace::read16(cell);
					if (cnt > 0 && cnt <= 64) { recCnt = cnt; recs = cell + 2; }
				}
			}

			char km[96]; snprintf(km, sizeof km, "/dev/shm/PL%02X_sidasm.txt", cid);
			FILE* kf = fopen(km, "a");
			if (kf && ftell(kf) == 0)
				fprintf(kf, "# sprite_id cnt nrecs dx,dy,part,flip;...  (GFX2 cell: cell=gfx2+*(gfx2+sid*4), cnt=u16@cell, recs@cell+2)\n");

			// 8-byte GFX2 cell record (CONFIRMED bank03 loc_8c0344d4, rip_gfx2_assembly.py):
			//   [dx:s16 @+0][dy:s16 @+2][palette:u16 @+4][GFX_SELECTOR:u16 @+6]
			//   GFX_SELECTOR = the GFX1 offset-table index (the +6 field, range ~29-65)
			//   palette row  = (rec[+0x4] & 0x3ff) >> 4 ; flip = rec[+4] bit 0x10
			// dx/dy are accumulated into a RUNNING PEN (facing-neutral) per §3a so the
			// emitted assembly stores absolute-in-cell-space placement.
			int nrec = 0;
			if (kf && recs) {
				char body[1900]; int bn = 0;
				int px = 0, py = 0;   // cumulative pen (running accumulator)
				for (int r = 0; r < recCnt; r++) {
					uint32_t rec = recs + (uint32_t)r * 8;
					int16_t  rdx   = (int16_t) addrspace::read16(rec);
					int16_t  rdy   = (int16_t) addrspace::read16(rec + 2);
					uint16_t rpalw = (uint16_t)addrspace::read16(rec + 4);   // palette word
					uint16_t rsel  = (uint16_t)addrspace::read16(rec + 6);   // GFX_SELECTOR (the key)
					if (rsel == 0x00FF) break;                      // assembly terminator (sentinel)
					px += rdx; py += rdy;                           // advance the pen BEFORE emit
					unsigned palRow = (unsigned)((rpalw & 0x03ff) >> 4);   // static palette row
					unsigned flip   = (rpalw & 0x10) ? 1u : 0u;            // +4 bit 0x10 = X-mirror
					if (bn < (int)sizeof(body) - 40)
						// "<penX>,<penY>,<selector>,<flip>,<palRow>;"  — `part` field == +6 selector
						bn += snprintf(body + bn, sizeof(body) - bn, "%d,%d,%u,%u,%u;",
						               px, py, rsel, flip, palRow);
					if (rsel < 512) liveUsePart[rsel] = true;       // dump this selector's pixels
					nrec++;
				}
				if (nrec > 0) fprintf(kf, "%u %u %d %s\n", liveSid, recCnt, nrec, body);
			}
			if (kf) fclose(kf);
			if (lg) {
				fprintf(lg, "[CHAR] LIVE re-key (GFX2): sid144=0x%04x cell=0x%08x kf[4]=0x%04x gfx2=0x%08x cnt=%d recs@0x%08x nrec=%d\n",
				        sid144, cellP, liveSid, kfGfx2, recCnt, recs, nrec);
				fprintf(lg, "[CHAR]   cell kf[0..19] = %s\n", kfbytes);
				if (nrec == 0) fprintf(lg, "[CHAR]   *** empty assembly — GFX2 cell unresolved or count==0 (gfx2=0x%08x) ***\n", kfGfx2);
			}
		}

		// Collect every GFX_SELECTOR referenced anywhere in the EXTRAS region so the
		// pixel dump below covers the character's FULL part set. The region is a flat
		// stream of assemblies, each a run of 8-byte records terminated by selector(@+6)
		// ==0x00FF (or an all-zero separator). CONFIRMED layout: palette=u16@+4,
		// GFX_SELECTOR=u16@+6 (the key). (The old part-u8@+4 model was wrong.)
		bool usePart[512] = {false};
		int nUse = 0, nSlots = 0;
		if (_ramAddr(exP)) {
			const int NREC = 0x10000 / 8;                     // scan up to 64KB region
			bool inAsm = false;
			for (int r = 0; r < NREC; r++) {
				uint32_t rec  = exP + (uint32_t)r * 8;
				uint16_t palw = (uint16_t)addrspace::read16(rec + 4);   // palette word
				uint16_t sel  = (uint16_t)addrspace::read16(rec + 6);   // GFX_SELECTOR (key)
				uint32_t lo   = addrspace::read32(rec);
				if (sel == 0x00FF || (lo == 0 && sel == 0 && palw == 0)) {
					if (inAsm) { nSlots++; inAsm = false; }   // assembly boundary
					continue;
				}
				if (sel < 512 && !usePart[sel]) { usePart[sel] = true; nUse++; }
				inAsm = true;
			}
			if (inAsm) nSlots++;
		}
		// Merge the LIVE assembly's parts in (guarantees every part the on-screen sid
		// references gets dumped, even if the slot-scan didn't reach it).
		for (int p = 0; p < 512; p++) if (liveUsePart[p] && !usePart[p]) { usePart[p] = true; nUse++; }
		if (lg) fprintf(lg, "[CHAR] EXTRAS scan: %d non-empty slots, %d distinct part_idx (incl live)\n",
		                nSlots, nUse);

		// Per-fire compact log: sid + the CURRENT-POSE part list (the GFX2 cell-walk
		// selectors = liveUsePart), so we see it change across fires. A correct body pose
		// shows 6-19 selectors here (NOT `0 1`, which was the degenerate EXTRAS path).
		// The EXTRAS-region superset (usePart) is reported separately for reference.
		{
			int poseN = 0; for (int p = 0; p < 512; p++) if (liveUsePart[p]) poseN++;
			char pl[512]; int pn = snprintf(pl, sizeof pl, "[FIRE %d] cid=%u(PL%02X) sid=%u cell=0x%08x poseParts=%d parts=",
			                                fires, cid, cid, sid, cellP, poseN);
			for (int p = 0; p < 512 && pn < (int)sizeof(pl) - 6; p++) if (liveUsePart[p]) pn += snprintf(pl + pn, sizeof(pl) - pn, "%d ", p);
			if (pn < (int)sizeof(pl) - 24) pn += snprintf(pl + pn, sizeof(pl) - pn, " (extrasScan=%d)", nUse);
			if (lg) fprintf(lg, "%s\n", pl);
			FILE* tf = fopen("/dev/shm/mc_partdump_sidtrace.log", "a");
			if (tf) { fprintf(tf, "%s\n", pl); fclose(tf); }
		}

		// =====================================================================
		// PIXEL RESOLUTION — the QUAD EMITTER path (bank03 loc_8c033d78/e90), CONFIRMED
		// against marvelous2 build/bank03.asm:9092-9305:
		//
		//   gfx1     = *(node + 0x15c)                     ; Dat_GFX1 (loc_8c033f72=0x15c)
		//   ; the running texel pointer is seeded ONCE per node at routine entry:
		//   texPtr   = 0x0CE60000                          ; (loc_8c033e28 literal)
		//   ; then per GFX2-cell record, IN ORDER (records from GFX2[sid], see above):
		//   GFX_SELECTOR = u16 @(rec + 0x06)               ; THE FIX: +6 (NOT +4=palette)
		//   off          = u32 @(gfx1 + GFX_SELECTOR*4)    ; offset-table lookup
		//   blob         = gfx1 + off ; blob += 0x02       ; skip 2-byte piece header
		//   w            = (u8 @ blob++) << 3              ; ×8  (shll2;shll)
		//   h            = (u8 @ blob++) << 3              ; ×8
		//   ; texels are 4bpp paletted in main RAM starting at texPtr, then:
		//   texPtr      += (w*h) >> 1                      ; 4bpp byte advance per part
		//
		// THE FIX: the index source is the +6 GFX_SELECTOR (range 29-65), NOT +4 (which
		// is the palette word, a near-constant ~16). NO FAC (+0x184), NO per-char base,
		// NO shift beyond sel*4. The texels are NOT inside the GFX1 blob (that holds the
		// part header/dims); they are a CONTIGUOUS run consumed in cell-walk order from
		// 0x0CE60000, so the per-part texptr is only defined within the live walk — hence
		// we re-walk the GFX2 cell here (same records as the sidasm block) and dump each
		// distinct selector the first time it's encountered with its running texptr.
		//
		// KEY CONSISTENCY (gate c): the manifest is keyed by the +6 selector — the SAME
		// key the sidasm assembly records carry (their `part` field == +6 selector). So
		// assembly part-keys are a subset of manifest/parts keys; no dangling lookups.
		// =====================================================================
		char mn[96]; snprintf(mn, sizeof mn, "/dev/shm/PL%02X_parts.manifest", cid);
		FILE* mf = fopen(mn, "a");   // append: full atlas accumulates across frames
		if (mf && ftell(mf) == 0)
			// Columns read by tools/pack_part_atlas.py read_manifest():
			//   <selector> <key> <raw> <ppm> <w> <h> <e4> <texptr> <ec> <rawbytes> <tcw>
			fprintf(mf, "# selector key raw ppm w h e4 texptr ec rawbytes tcw\n");

		const uint32_t TEXEL_BASE = 0x0CE60000;       // bank03.asm:9118 loc_8c033e28
		uint32_t gfxBase = gfx1 & 0x0FFFFFFFu;        // *(node+0x15c), 0x0C alias
		// 4bpp PAL4, LINEAR (0x0CE60000 is the LZSS-expanded planar working buffer, NOT a
		// twiddled VRAM upload). Synthesise a TCW so the offline packer's tcw_format()
		// resolves PAL4/linear directly: fmt(bits27-29)=5, scan-order(bit26)=1=linear.
		const uint32_t TCW_PAL4_LINEAR = (5u << 27) | (1u << 26);   // 0x2C000000

		// THE FIX (2026-06-09): resolve the live per-pose part list via the CRACKED GFX2
		// CELL-TABLE walk (node+0x160), NOT the degenerate EXTRAS slot table
		// (exP + slot*0x400 + 8 — an UNCONFIRMED stride that returned only the body's
		// 1-2 EXTRAS records). This is the exact read tools/rip_gfx2_assembly.py uses and
		// matches bank03 loc_8c0344d4 (MARVELOUS2-GFX-NOTES §3a):
		//   gfx2  = *(node+0x160)                              ; GFX2 base
		//   cell  = gfx2 + *(u32)(gfx2 + (sid & 0x7FFF)*4)     ; per-sid cell record
		//   count = u16 @ cell                                 ; record COUNT (6-19 for a body pose)
		//   recs  = cell + 2                                   ; COUNT * 8-byte records follow
		// Each 8-byte record: [dx s16 @+0][dy s16 @+2][pal u16 @+4][SEL u16 @+6].
		// The pixel walk below (rec+4=palette, rec+6=GFX1 selector, texPtr advance) is
		// unchanged — only the RECORD SOURCE changed from the EXTRAS slot table to GFX2.
		uint32_t gfx2     = addrspace::read32(pbase + OFF_GFX01_PTR);   // *(node+0x160)
		uint32_t gfx2Base = gfx2 & 0x0FFFFFFFu;
		uint32_t pdRecs   = 0;
		int      pdCount  = 0;
		if (_ramAddr(gfx2Base | 0x0C000000u)) {
			uint32_t sidIdx  = (uint32_t)(sid & 0x7FFF);
			uint32_t cellOff = addrspace::read32(gfx2Base + sidIdx * 4) & 0x0FFFFFFFu;
			uint32_t cell    = (gfx2Base + cellOff) & 0x0FFFFFFFu;
			if (_ramAddr(cell | 0x0C000000u)) {
				int cnt = (int)(uint16_t)addrspace::read16(cell);
				if (cnt > 0 && cnt <= 64) { pdCount = cnt; pdRecs = cell + 2; }
			}
		}

		bool dumped[512] = {false};
		uint32_t texPtr = TEXEL_BASE;    // running pointer, seeded once per node (disasm)
		if (_ramAddr(gfxBase | 0x0C000000u) && pdRecs) {
			for (int r = 0; r < pdCount; r++) {       // bounded by the GFX2 cell COUNT
				uint32_t rec   = pdRecs + (uint32_t)r * 8;
				uint16_t rpalw = (uint16_t)addrspace::read16(rec + 4);   // palette word
				uint16_t rsel  = (uint16_t)addrspace::read16(rec + 6);   // GFX_SELECTOR (key)
				if (rsel == 0x00FF) break;                        // assembly terminator (sentinel)
				uint32_t lo = addrspace::read32(rec);
				if (lo == 0 && rsel == 0 && rpalw == 0) continue; // pad/separator
				unsigned palRow = (unsigned)((rpalw & 0x03ff) >> 4);   // static palette row

				// Resolve dims via the GFX1 offset table at gfx1 + selector*4 (per disasm).
				uint32_t off  = addrspace::read32(gfxBase + (uint32_t)rsel * 4);
				uint32_t blob = (gfxBase + off) & 0x0FFFFFFFu;
				uint32_t bAlias = blob | 0x0C000000u;
				int w = 0, h = 0;
				if (_ramAddr(bAlias)) {
					// blob += 2 (skip header), then read w,h (each <<3).
					w = (int)((uint8_t)addrspace::read8(bAlias + 0x02)) << 3;
					h = (int)((uint8_t)addrspace::read8(bAlias + 0x03)) << 3;
				}
				int rawBytes = (w * h) >> 1;                      // 4bpp byte size

				if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
					if (mf && rsel < 512 && !dumped[rsel]) {
						fprintf(mf, "%u %u - - %d %d 0401 %08x 0 %d %08x SKIP\n",
						        rsel, palRow, w, h, texPtr, rawBytes, TCW_PAL4_LINEAR);
						dumped[rsel] = true;
					}
					texPtr += (rawBytes > 0) ? (uint32_t)rawBytes : 0;   // still advance the run
					continue;
				}

				if (rsel < 512 && !dumped[rsel]) {
					dumped[rsel] = true;
					char rfn[96]; snprintf(rfn, sizeof rfn, "PL%02X_part_%03u.raw", cid, rsel);
					char pfn[96]; snprintf(pfn, sizeof pfn, "PL%02X_part_%03u.ppm", cid, rsel);
					char rfp[112], pfp[112];
					snprintf(rfp, sizeof rfp, "/dev/shm/%s", rfn);
					snprintf(pfp, sizeof pfp, "/dev/shm/%s", pfn);
					// 4bpp paletted (fmt 5), LINEAR. Palette = live Dat_Pal (player+0x164)
					// at the part's static palette row. Raw dump lets the offline packer
					// relock if needed; PPM is the proven correct preview.
					partDumpRawN(texPtr, rawBytes, rfp);
					partDecodeToPPM(texPtr, w, h, /*fmt=*/5, /*linear=*/true,
					                palP + (uint32_t)palRow * 32, pfp, false);
					// Columns: selector key raw ppm w h e4 texptr ec rawbytes tcw
					if (mf) fprintf(mf, "%u %u %s %s %d %d 0401 %08x 0 %d %08x\n",
					                rsel, palRow, rfn, pfn, w, h, texPtr, rawBytes, TCW_PAL4_LINEAR);
					dumpedTotal++;
				}
				texPtr += (uint32_t)rawBytes;                     // advance the running pointer
			}
		}
		if (mf) fclose(mf);
		if (lg) fprintf(lg, "[CHAR] GFX1-emitter dump: gfxBase=0x%08x gfx2Base=0x%08x cellRecs=0x%08x cnt=%d texBase=0x%08x -> %s\n",
		                gfxBase, gfx2Base, pdRecs, pdCount, TEXEL_BASE, mn);

		// =====================================================================
		// CLEAN PIXEL SOURCE — the DM00 directory (FIX 2026-06-09).
		//
		// ROOT CAUSE of the magenta/stripe NOISE: the GFX1-emitter dump above reads
		// each part from the TRANSIENT scratch 0x0CE60000 with a CONTIGUOUS
		// `texPtr += (w*h)>>1` walk. That layout is only valid DURING the one-shot
		// load-decode (loc_8c03281c/loc_8c032696 decode parts into the tiny
		// 0x0CE60000/0x0CE61000 ping-pong scratch and upload each to its persistent
		// slot). The per-frame emitter (loc_8c033e90) ALSO reads 0x0CE60000, but
		// nothing re-decodes the FULL character there each frame — with two chars
		// loaded the scratch holds whichever decoded LAST, so contiguous offsets read
		// stale residue (garbage / the other char's bytes). Confirmed by montage.
		//
		// THE CLEAN SOURCE: MVC2 decodes EVERY part ONCE at character load into a
		// PERSISTENT per-part slot and records it in the DM00 directory at
		// *(0x0CE80008) (this `dirBase`), stride 0x10:
		//   entry+0x0 = (w,h) packed as two u16 PIXELS
		//   entry+0x4 = format word (e4); byte1 ((e4>>8)&0xff) = PVR PixelFmt code:
		//               0x01->RGB565(1)  0x02->ARGB4444(2)  0x03->PAL8(6)  0x00->1555
		//   entry+0x8 = pointer to the DECODED TWIDDLED texels (persistent, in mem_b)
		// These slots survive the whole match (loc_8c0322c0: dir_entry = *(r6+8)+(k<<4),
		// dest = *(entry+8)). The body lands at key `charBase` (256x256 PAL8), the
		// sub-parts at charBase+1.. (per loc_8c032ae0's incrementing counter `r11`).
		//
		// We dump the active char's directory run (charBase..charBase+run) as CLEAN
		// PPMs keyed by directory key. The PPM format (P6, magenta=transparent) and
		// the GFX2 selector walk above are UNCHANGED — this only adds the correct
		// pixel source. Each part is twiddled (ScanOrder=0 for these slots) → use the
		// existing detwiddle path. Palette = live Dat_Pal (player+0x164) for PAL8.
		// =====================================================================
		{
			char dmn[96]; snprintf(dmn, sizeof dmn, "/dev/shm/PL%02X_dm00.manifest", cid);
			FILE* dmf = fopen(dmn, "a");
			if (dmf && ftell(dmf) == 0)
				fprintf(dmf, "# key w h fmt texptr e4 ppm  (DM00 directory — clean persistent decoded parts)\n");
			int dmDumped = 0;
			// Walk the directory from charBase; the char's parts form a contiguous run.
			// Stop after a short blank run (the run for one entity is small, ~16-24).
			int blanks = 0;
			for (int k = charBase; k < charBase + 64; k++) {
				if (k < 0) continue;
				uint32_t e   = dirBase + (uint32_t)k * 0x10;
				uint32_t e0  = addrspace::read32(e);
				uint32_t e4d = addrspace::read32(e + 4);
				uint32_t e8d = addrspace::read32(e + 8);
				int dw = (int)(e0 & 0xffff), dh = (int)((e0 >> 16) & 0xffff);
				if (dw <= 0 || dh <= 0 || dw > 512 || dh > 512 || !_ramAddr(e8d)) {
					if (++blanks > 2) break;     // end of this entity's contiguous run
					continue;
				}
				blanks = 0;
				int dfmt = partFmtFromE4(e4d);                 // e4 byte1 -> PVR PixelFmt
				bool dlinear = false;                          // DM00 slots are twiddled
				// PAL8/PAL4 use the live Dat_Pal; 16-bit formats ignore palBase.
				char dpfn[96]; snprintf(dpfn, sizeof dpfn, "PL%02X_dm00_%03d.ppm", cid, k);
				char dpfp[112]; snprintf(dpfp, sizeof dpfp, "/dev/shm/%s", dpfn);
				partDecodeToPPM(e8d, dw, dh, dfmt, dlinear, palP, dpfp, false);
				if (dmf) fprintf(dmf, "%d %d %d %d %08x %08x %s\n", k, dw, dh, dfmt, e8d, e4d, dpfn);
				dmDumped++;
			}
			if (dmf) fclose(dmf);
			if (lg) fprintf(lg, "[CHAR] DM00 CLEAN dump: charBase=%d dumped %d entries (key range %d..) -> %s\n",
			                charBase, dmDumped, charBase, dmn);
		}
	}
	if (lg) { fprintf(lg, "\ndumped %d parts this frame\n", dumpedTotal); fclose(lg); }
}

// =============================================================================
// GFX1DUMP — CLEAN gameplay-part pixels from the LOAD-TIME GFX1 decode buffer
// (gated MAPLECAST_GFX1DUMP). READ-ONLY.
//
// THE PROBLEM (why partDump's GFX1 path was noise, why DM00 was the wrong set):
//   * The per-frame partDump's contiguous GFX1 walk (texPtr=0x0CE60000; texPtr +=
//     (w*h)>>1 per part) is the STRUCTURALLY CORRECT algorithm — confirmed against
//     the load decoder bank03:loc_8c032ae0/loc_8c0327d4, where the decode DEST
//     pointer (r12) ADVANCES by each part's byte size (`add r4,r12`), i.e. the
//     character's parts are laid out CONTIGUOUSLY in 0x0CE60000 as they decode.
//   * But that buffer is a TRANSIENT scratch. With two chars loaded and a match in
//     flight, nothing re-decodes the full character there each frame; mid-match the
//     contiguous offsets read stale residue -> the magenta/stripe NOISE we saw.
//   * The DM00 directory (*(0x0CE80008)) IS persistent + clean. The EARLIER attempt
//     keyed it by `char_base + part_ordinal` (a sequential counter, charBase=9/13 from
//     player+0xad) — the WRONG offset; that run is the portrait/UI set + the 256x256
//     body, not the per-pose +6-selector gameplay parts.
//
// THE FIX (2026-06-09, DISASM-CONFIRMED): the persistent per-part texels store is
// SELECTOR-INDEXED at +0xA0, read EXACTLY as the gameplay-part decoder loc_8c032696's
// copy-out (the only LOOPING LZSS caller; bank03:5807-5853) writes it:
//   DM00base = *(0x0CE80008)                              ; prologue r8=*(0x0ce80008)
//   entry    = DM00base + (sel<<4) + 0xA0                 ; sel=*r9 (+6 selector), +0xA0=loc_8c03283c
//   e0 = dims (w=lo16, h=hi16)   e4 byte1 = PVR fmt   e8 = persistent texels ptr
// This is the SAME store the per-frame render loc_8c0344d4 reads via its texptr — it
// persists the WHOLE match, so we can read it ANY in-match frame (palette loaded). We
// walk the live per-pose GFX2 cell to recover the +6 SELECTOR per record, then read the
// DM00 entry at dirBase + sel*0x10 + 0xA0 for that part's persistent texels + dims/fmt.
// Output is keyed by the +6 selector so tools/rip_gfx2_assembly.py --realparts
// (PL{HEX}_part_NNN.ppm) consumes it. Reuses the PROVEN partDecodeToPPM (twiddled,
// format from partFmtFromE4). READ-ONLY.
//
// Output (operator-local, ROM-derived -> /dev/shm only):
//   /dev/shm/PL{HEX}_gfx1_NNNN.ppm   one clean part per +6 selector (P6, magenta=transp)
//   /dev/shm/PL{HEX}_gfx1.manifest   "<selector> <palRow> <w> <h> <fmt> <texptr> <ppm>"
//   /dev/shm/mc_gfx1dump.log         per-fire trace
// The --realparts contract wants PL{HEX}_part_NNN.ppm; we ALSO write that filename
// (symlink-free copy) so the tool consumes it directly. Manifest selector column ==
// the +6 GFX selector == the `part` field in the sidasm assembly (no dangling keys).
// =============================================================================
static void gfx1Dump(const GameState& state) {
	static const char* env = getenv("MAPLECAST_GFX1DUMP");
	static bool on = env != nullptr;
	if (!on) return;
	// Capture the first WINDOW in-match frames after EACH fresh match start. The
	// one-shot load decode runs at match load, so the buffer is freshest in the
	// opening frames; we re-scan each of these frames and keep first-seen-per-selector
	// (a later frame only fills selectors a poses-this-frame missed).
	static const int WINDOW = (env && atoi(env) > 1) ? atoi(env) : 20;
	static bool prevInMatch = false;
	static int  framesIn = 0;
	static bool cleared = false;
	static bool seen[0x40][512] = {{false}};   // [char_id][selector] first-seen gate

	if (!state.in_match) { prevInMatch = false; framesIn = 0; return; }
	if (!prevInMatch) {                         // fresh match start -> reset window + gates
		prevInMatch = true; framesIn = 0;
		for (int c = 0; c < 0x40; c++) for (int s = 0; s < 512; s++) seen[c][s] = false;
		cleared = false;
	}
	if (framesIn++ >= WINDOW) return;           // only the opening (fresh-buffer) window

	// On first fire of this window, clear stale dumps + manifests (as the maplecast
	// user; /dev/shm is maplecast-owned).
	if (!cleared) {
		for (int c = 0; c < 0x40; c++) {
			char mn[96];
			snprintf(mn, sizeof mn, "/dev/shm/PL%02X_gfx1.manifest", c); remove(mn);
		}
		cleared = true;
	}

	FILE* lg = fopen("/dev/shm/mc_gfx1dump.log", framesIn == 1 ? "w" : "a");
	if (lg) fprintf(lg, "# GFX1DUMP fire (framesIn=%d/%d) frame=%u — clean LOAD-decode parts by +6 selector\n",
	                framesIn, WINDOW, state.frame_counter);

	// ROOT CAUSE of dumpedThisFire=0 (fixed 2026-06-09): the old gfx1Dump read pixels
	// from the TRANSIENT scratch 0x0CE60000 with the contiguous `texPtr += (w*h)>>1`
	// walk AND took its dims from the GFX1 offset table (gfxBase + sel*4 -> blob+2/+3).
	// Mid-match that offset table / scratch is stale (the load decode ran on the VS /
	// char-load screen, BEFORE in_match flipped true). Worse than noise: the dims read
	// came back 0, so `w<=0||h<=0` skipped EVERY part -> zero PPMs. The clean, PERSISTENT
	// source is the DM00 directory (*(0x0CE80008)): MVC2 decodes every part ONCE at load
	// into a per-part slot recorded there (loc_8c0322c0: entry=*(r6+8)+(k<<4), texels at
	// *(entry+8); loc_8c032ae0 fills entries in an incrementing r11 order). It SURVIVES
	// the match — it's the same store that gave the clean PAL4/portrait dumps. We map the
	// per-pose GFX2 cell records to the DM00 run (record r -> dir key charBase+r, same
	// decode order) and key the output by the +6 selector so rip_gfx2_assembly.py
	// --realparts consumes it directly. Reuses the proven partDecodeToPPM (DM00 fmt/dims,
	// twiddled).  OPTION (a): persistent per-part address, NOT the transient 0x0CE60000.
	const uint32_t OFF_SLOT_SEL = 0x0ad;   // disasm loc_8c032a66/loc_8c032ba2 (base selector)
	uint32_t dirBase = addrspace::read32(0x0CE80008);
	if (!_ramAddr(dirBase)) { uint32_t alt = addrspace::read32(0x8CE80008); if (_ramAddr(alt)) dirBase = alt; }
	if (!_ramAddr(dirBase)) {              // directory not built yet — wait for char load
		if (lg) { fprintf(lg, "[GFX1] DM00 directory not built yet (dirBase=%08x) — skip fire\n", dirBase); fclose(lg); }
		return;
	}

	for (int s = 0; s < 6; s++) {
		uint32_t pbase = CHAR_BASE[s];
		if (!(uint8_t)addrspace::read8(pbase + OFF_ACTIVE)) continue;
		uint8_t  cid  = (uint8_t)addrspace::read8(pbase + OFF_CHAR_ID);
		uint32_t gfx2 = addrspace::read32(pbase + OFF_GFX01_PTR);   // 0x160 Dat_GFX2 (cell tbl)
		uint32_t palP = addrspace::read32(pbase + OFF_PAL_PTR);     // 0x164 Dat_Pal
		uint16_t sid  = (uint16_t)addrspace::read16(pbase + OFF_SPRITE_ID);
		uint8_t  bsel = (uint8_t)addrspace::read8(pbase + OFF_SLOT_SEL);
		uint32_t gfx2Base = gfx2 & 0x0FFFFFFFu;
		if (!_ramAddr(gfx2Base | 0x0C000000u)) continue;
		// disasm loc_8c032a66: base = (sel==1)?13 : 9. (Kept only for the diagnostic
		// log — the CORRECT DM00 entry is selector-indexed at +0xA0, see below; the old
		// charBase+ordinal cursor was the WRONG offset.)
		int charBase = (bsel == 1) ? 13 : 9;

		// Resolve THIS pose's GFX2 cell (record ORDER == DM00 fill order == load decode).
		uint32_t sidIdx  = (uint32_t)(sid & 0x7FFF);
		uint32_t cellOff = addrspace::read32(gfx2Base + sidIdx * 4) & 0x0FFFFFFFu;
		uint32_t cell    = (gfx2Base + cellOff) & 0x0FFFFFFFu;
		if (!_ramAddr(cell | 0x0C000000u)) continue;
		int cnt = (int)(uint16_t)addrspace::read16(cell);
		if (cnt <= 0 || cnt > 64) continue;
		uint32_t recs = cell + 2;

		char mn[96]; snprintf(mn, sizeof mn, "/dev/shm/PL%02X_gfx1.manifest", cid);
		FILE* mf = fopen(mn, "a");
		if (mf && ftell(mf) == 0)
			fprintf(mf, "# selector palRow w h fmt texptr ppm  (clean DM00 persistent parts, keyed by +6 selector)\n");

		// === CORRECT DM00 entry addressing (DISASM-CONFIRMED 2026-06-09) ===========
		// The persistent per-part texels store is SELECTOR-INDEXED at +0xA0, NOT keyed
		// by charBase+ordinal (that was the wrong offset, giving the portrait/UI run).
		// Confirmed from the gameplay-part decoder loc_8c032696's copy-out (the only
		// LOOPING LZSS caller; bank03 lines 5807-5853):
		//   r8 = *(0x0CE80008)                 ; DM00base   (prologue @5683: mov.l @(0x8,r4),r8, r4=0x0ce80000)
		//   r7 = *r9 (u8 +6 selector)          ; @5811 mov.b @r9,r7
		//   r7 <<= 4  (= sel * 0x10 stride)    ; @5813/5817 two shll2 after extu.b
		//   r7 += r8                           ; @5819 add r8,r7   -> DM00base + sel*0x10
		//   r7 += 0xA0  (const loc_8c03283c)   ; @5821 add r4,r7   (r4 = mov.w @(loc_8c03283c) = 0x00a0)
		//   texels = *(r7 + 0x8)               ; @5825 mov.l @(0x8,r7),r7 = copy-out DEST (e8)
		// So entry = DM00base + sel*0x10 + 0xA0; e0=dims(w=lo16,h=hi16), e4 byte1=fmt,
		// e8=persistent texels. This is the SAME store the per-frame render loc_8c0344d4
		// reads via its texptr — it persists the whole match. (The +0x100/r10 sibling
		// table @5841-5853 is the paired high-half; the +6 gameplay selector is r9/+0xA0.)
		static const uint32_t DM00_BIAS   = 0xA0;   // bank03:5853 const loc_8c03283c
		static const uint32_t DM00_STRIDE = 0x10;
		int dumpedThis = 0;
		for (int r = 0; r < cnt; r++) {
			uint32_t rec   = recs + (uint32_t)r * 8;
			uint16_t rpalw = (uint16_t)addrspace::read16(rec + 4);   // palette word
			uint16_t rsel  = (uint16_t)addrspace::read16(rec + 6);   // +6 GFX SELECTOR (the key)
			if (rsel == 0x00FF) break;                               // assembly terminator
			uint32_t lo = addrspace::read32(rec);
			if (lo == 0 && rsel == 0 && rpalw == 0) continue;        // pad/separator
			unsigned palRow = (unsigned)((rpalw & 0x03ff) >> 4);

			// CLEAN PERSISTENT pixels: the DM00 entry is SELECTOR-INDEXED at +0xA0
			// (disasm trace above). The directory carries this part's OWN dims (e0),
			// format (e4) + the decoded persistent texel ptr (e8) the game's renderer reads.
			uint32_t e   = dirBase + (uint32_t)rsel * DM00_STRIDE + DM00_BIAS;
			uint32_t e0  = addrspace::read32(e);
			uint32_t e4d = addrspace::read32(e + 4);
			uint32_t e8d = addrspace::read32(e + 8);
			int w = (int)(e0 & 0xffff), h = (int)((e0 >> 16) & 0xffff);
			if (w <= 0 || h <= 0 || w > 512 || h > 512 || !_ramAddr(e8d)) {
				// Directory entry empty/invalid for this selector (part not loaded /
				// out of this char's set). The per-part addr in the log exposes a bad
				// +0xA0/stride immediately if the layout were off.
				if (lg) fprintf(lg, "[GFX1] cid=%u sel=%u entry=%08x EMPTY (e0=%08x e8=%08x) skip\n",
				                cid, rsel, e, e0, e8d);
				continue;
			}
			int dfmt = partFmtFromE4(e4d);           // e4 byte1 -> PVR PixelFmt (proven map)
			bool dlinear = false;                    // DM00 slots are twiddled (proven path)

			if (rsel < 512 && cid < 0x40 && !seen[cid][rsel]) {
				// FIRST-SEEN gate: dump this selector's clean persistent part. Write BOTH
				// the --realparts contract name (PLxx_part_NNN.ppm) AND the brief's
				// PLxx_gfx1_NNNN.ppm alias, keyed by the +6 selector.
				char pfn[96]; snprintf(pfn, sizeof pfn, "PL%02X_part_%03u.ppm", cid, rsel);
				char pfp[112]; snprintf(pfp, sizeof pfp, "/dev/shm/%s", pfn);
				partDecodeToPPM(e8d, w, h, dfmt, dlinear,
				                palP + (uint32_t)palRow * 32, pfp, /*swapXY=*/false);
				char gfn[96]; snprintf(gfn, sizeof gfn, "PL%02X_gfx1_%04u.ppm", cid, rsel);
				char gfp[112]; snprintf(gfp, sizeof gfp, "/dev/shm/%s", gfn);
				partDecodeToPPM(e8d, w, h, dfmt, dlinear,
				                palP + (uint32_t)palRow * 32, gfp, /*swapXY=*/false);
				seen[cid][rsel] = true;
				if (mf) fprintf(mf, "%u %u %d %d %d %08x %s\n", rsel, palRow, w, h, dfmt, e8d, pfn);
				if (lg)  fprintf(lg, "[GFX1] cid=%u(PL%02X) sel=%u entry=%08x %dx%d fmt=%d palRow=%u tex=%08x -> %s\n",
				                 cid, cid, rsel, e, w, h, dfmt, palRow, e8d, pfn);
				dumpedThis++;
			}
		}
		if (mf) fclose(mf);
		if (lg) fprintf(lg, "[GFX1] slot%d cid=%u(PL%02X) sid=%u cnt=%d charBase=%d gfx2=%08x dirBase=%08x dumpedThisFire=%d -> %s\n",
		                s, cid, cid, sid, cnt, charBase, gfx2Base, dirBase, dumpedThis, mn);
	}
	if (lg) fclose(lg);
}

// =============================================================================
// CHURN INSTRUMENT (gated MAPLECAST_CHURN). Answers: after the SH4 computes a
// frame, how many bytes / which fields of SH4 main RAM actually change per
// frame in-match? This is the information-content floor for a reconstruct-from-
// state renderer (vs shipping pixels). Read-only; prod-safe (default OFF).
//
// Approach: keep a shadow copy of the full 16 MB SH4 main RAM (mem_b, the
// 0x0C000000 / 0x8C000000 region; direct pointer via GetMemPtr). Each frame
// byte-diff live vs shadow. Aggregate over 60 frames (1 s) and report to
// /dev/shm/mc_churn.log + stdout:
//   1. total changed bytes/frame (avg/min/max) across all 16 MB
//   2. histogram by 64 KB region — top changed regions, LABELED via the memory
//      map (char structs @0x8C268000 pg616, globals @0x8C289000 pg649, object
//      pool @0x8C26A000, frame_ctr @0x8C3496B0, TA/render scratch, audio, …)
//   3. per-OFFSET change-frequency map within the 6 char structs (base 0x..340,
//      stride 0x5A4): which struct offsets the SH4 updates, how often; flag
//      HIGH-churn offsets NOT already on the 262-byte GSTA wire (the gaps)
//   4. split "logical game-state" churn (char structs + globals — ship-worthy)
//      from "render-list / TA-build / scratch" churn (the rest — reconstructed)
// =============================================================================
namespace {

// DC main-RAM offset (from 0x0C000000) of a 0x8C... address.
static inline uint32_t ramOff(uint32_t dcAddr) { return dcAddr & 0x00FFFFFF; }

// Label a 64 KB region index (region = byteOffset >> 16) per the MVC2 memory map.
// dcBase of region r = 0x8C000000 + (r << 16).
static const char* regionLabel(int region)
{
	uint32_t base = 0x8C000000u + ((uint32_t)region << 16);
	uint32_t end  = base + 0x10000u;
	// Char structs span 0x8C268340..~0x8C26A518 (6 * 0x5A4 interleaved) -> page 0x26
	if (base <= 0x8C268000u && 0x8C268000u < end) return "char-structs(pg616 0x..268000)";
	if (base <= 0x8C26A000u && 0x8C26A000u < end) return "object-pool(0x..26A000)";
	if (region == 0x28)                           return "globals+pool(pg649 0x..289000 in_match/meter/combo)";
	if (region == 0x34)                           return "frame_ctr-region(0x..3496B0)";
	if (region == 0x1F)                           return "camera/stage-anim(0x..1F9xxx)";
	// Heuristic bands (labels are best-effort; the numbers are authoritative).
	if (region <= 0x01)                           return "boot/IP.BIN/sys-lowram";
	if (region >= 0x02 && region <= 0x0F)         return "code(EntryPoint 0x..010000+)";
	if (region >= 0x10 && region <= 0x25)         return "engine-data/heaps";
	if (region >= 0x26 && region <= 0x2A)         return "game-state(structs+globals+pool)";
	if (region >= 0x2B && region <= 0x5F)         return "decoded-assets/work";
	if (region >= 0xC0 && region <= 0xCF)         return "render-scratch/poly-build";
	if (region >= 0xD0 && region <= 0xDF)         return "audio/AICA-work";
	return "other/scratch";
}

struct ChurnAgg {
	bool     en = false;
	bool     init = false;
	uint8_t* live = nullptr;          // direct mem_b pointer (0x0C000000)
	uint32_t ramSize = 0;
	std::vector<uint8_t> shadow;
	// per-frame totals over the window
	int      frames = 0;
	uint64_t totalChanged = 0;        // sum of changed bytes across window
	uint32_t minFrame = 0xFFFFFFFFu, maxFrame = 0;
	// per-64KB-region: changed bytes accumulated over the window
	std::vector<uint64_t> regionBytes; // 256 entries
	// per-char-struct OFFSET change frequency: union across the 6 slots, counted
	// per-frame (a byte that changed in ANY active slot at that offset counts once).
	// stride 0x5A4 = 1444 offsets.
	std::vector<uint32_t> offChanged;  // [0x5A4] frame-count where offset changed
	uint64_t logicalChanged = 0;       // bytes in char-structs+globals regions
	uint64_t restChanged = 0;          // everything else
};

static ChurnAgg g_churn;

// The 262-byte GSTA wire offsets within a char struct (for gap-flagging).
// Source: readGameState() above. byte = true if that struct offset is shipped.
static void buildGstaMask(bool* m /*[0x5A4]*/)
{
	memset(m, 0, 0x5A4);
	auto mark = [&](uint32_t off, int len){ for (int i=0;i<len && off+i<0x5A4;i++) m[off+i]=true; };
	mark(0x000,1); mark(0x001,1);              // active, char_id
	mark(0x034,4); mark(0x038,4);              // pos_x, pos_y
	mark(0x0E0,4); mark(0x0E4,4);              // screen_x, screen_y
	mark(0x05C,4); mark(0x060,4);              // vel_x, vel_y
	mark(0x110,1);                             // facing
	mark(0x144,2);                             // sprite_id
	mark(0x1D0,2);                             // animation_state
	mark(0x142,2);                             // anim_timer
	mark(0x420,1); mark(0x424,1);              // health, red_health
	mark(0x1E9,1);                             // special_move_id
	mark(0x4C9,1);                             // assist_type
	mark(0x52D,1);                             // palette_id
	mark(0x168,4);                             // anim_pointer
}

// Name a char-struct offset for the report (best-effort; from gamestate.h +
// pl_mem.asm / mvc2-sh4-re-expert KB). Only the notable ones are named.
static const char* offName(uint32_t off)
{
	switch (off) {
		case 0x000: return "active";          case 0x001: return "character_id";
		case 0x00C: return "char_link_ptr";
		case 0x025: return "color/pl_palid_match";
		case 0x034: return "pos_x";           case 0x038: return "pos_y";
		case 0x040: return "paleffect";
		case 0x050: return "x_sprite_scale";  case 0x054: return "y_sprite_scale";
		case 0x05C: return "vel_x";           case 0x060: return "vel_y";
		case 0x0E0: return "screen_x";        case 0x0E4: return "screen_y";
		case 0x110: return "facing";
		case 0x12C: return "visible_gate";
		case 0x12E: return "palette-effect-sel(hit-flash)";
		case 0x130: return "xflip_copy";      case 0x134: return "xflip_copy2";
		case 0x142: return "anim_timer";      case 0x144: return "sprite_id";
		case 0x151: return "RenderExtra";
		case 0x154: return "current_cell_data(ptr)";
		case 0x158: return "anim_group";
		case 0x15C: return "Dat_GFX1(ptr)";   case 0x160: return "Dat_GFX2(ptr)";
		case 0x164: return "Dat_Pal(ptr)";    case 0x168: return "animations(ptr)";
		case 0x1D0: return "animation_state";
		case 0x1D2: return "xflip";           case 0x1D3: return "walk_dir";
		case 0x1E9: return "special_move_id";
		case 0x1F9: return "stance";
		case 0x275: return "hitstun_flash(0xff in hitstun)";
		case 0x420: return "health";          case 0x424: return "red_health";
		case 0x4C9: return "assist_type";
		case 0x502: return "sub_anim_phase";
		case 0x52D: return "palette";
		default:    return "";
	}
}

static void churnReport(const GameState& state)
{
	ChurnAgg& C = g_churn;
	FILE* lg = fopen("/dev/shm/mc_churn.log", "a");
	auto out = [&](const char* fmt, ...) {
		va_list ap; va_start(ap, fmt);
		va_list ap2; va_copy(ap2, ap);
		vprintf(fmt, ap);
		if (lg) vfprintf(lg, fmt, ap2);
		va_end(ap2); va_end(ap);
	};
	double favg = C.frames ? (double)C.totalChanged / C.frames : 0.0;
	out("\n==================== MAPLECAST_CHURN  (in_match=%d frame=%u, window=%d frames) ====================\n",
	    state.in_match, state.frame_counter, C.frames);
	out("[1] TOTAL changed bytes/frame over 16MB:  avg=%.0f  min=%u  max=%u\n",
	    favg, (C.minFrame==0xFFFFFFFFu?0:C.minFrame), C.maxFrame);
	out("    -> reconstruct-from-state FLOOR (if we shipped EVERY changed byte raw): %.0f B/frame * 60 = %.1f KB/s = %.2f Mbps\n",
	    favg, favg*60.0/1024.0, favg*60.0*8.0/1e6);

	// [4] logical vs rest split
	double logAvg  = C.frames ? (double)C.logicalChanged / C.frames : 0.0;
	double restAvg = C.frames ? (double)C.restChanged    / C.frames : 0.0;
	out("[4] LOGICAL game-state churn (char-structs pg616 + globals/pool pg649): avg=%.0f B/frame  (%.2f KB/s, %.3f Mbps)\n",
	    logAvg, logAvg*60.0/1024.0, logAvg*60.0*8.0/1e6);
	out("    RENDER-LIST/SCRATCH churn (everything else — reconstructed, NOT shipped): avg=%.0f B/frame  (%.2f KB/s)\n",
	    restAvg, restAvg*60.0/1024.0);

	// [2] region histogram — top 12 by changed bytes
	out("[2] TOP CHANGED 64KB REGIONS (avg bytes/frame):\n");
	std::vector<int> idx(C.regionBytes.size());
	for (size_t i = 0; i < idx.size(); i++) idx[i] = (int)i;
	std::sort(idx.begin(), idx.end(), [&](int a, int b){ return C.regionBytes[a] > C.regionBytes[b]; });
	for (int k = 0; k < 12 && k < (int)idx.size(); k++) {
		int r = idx[k];
		if (C.regionBytes[r] == 0) break;
		double avg = (double)C.regionBytes[r] / (C.frames ? C.frames : 1);
		out("    region 0x%02X  base=0x%08X  avg=%8.1f B/frame  %s\n",
		    r, 0x8C000000u + ((uint32_t)r << 16), avg, regionLabel(r));
	}

	// [3] char-struct per-offset change-frequency map + GSTA gap flags
	bool gsta[0x5A4]; buildGstaMask(gsta);
	out("[3] CHAR-STRUCT changed offsets (union over 6 slots; %% = frames-changed/%d). "
	    "[*]=on GSTA wire, [GAP]=NOT shipped:\n", C.frames);
	// collect offsets that changed at least once
	std::vector<int> chOff;
	for (uint32_t o = 0; o < 0x5A4; o++) if (C.offChanged[o]) chOff.push_back((int)o);
	std::sort(chOff.begin(), chOff.end(), [&](int a, int b){ return C.offChanged[a] > C.offChanged[b]; });
	int gapHi = 0;
	for (int o : chOff) {
		double pct = C.frames ? 100.0 * C.offChanged[o] / C.frames : 0.0;
		bool shipped = gsta[o];
		const char* nm = offName((uint32_t)o);
		bool gap = !shipped && pct >= 25.0;   // HIGH-churn & not shipped
		if (gap) gapHi++;
		out("    +0x%03X  %5.1f%%  %s  %s%s\n", o, pct,
		    shipped ? "[*]  " : "[GAP]",
		    nm[0] ? nm : "(unknown)",
		    gap ? "   <== HIGH-CHURN GAP" : "");
	}
	out("    -> %d HIGH-churn (>=25%%) char-struct offsets are NOT on the 262B GSTA wire.\n", gapHi);
	out("================================================================================================\n");
	if (lg) fclose(lg);
	fflush(stdout);
}

} // anonymous namespace

// Per-frame churn hook. Cheap when disabled. Diffs full 16MB mem_b vs a shadow.
static void churnDump(const GameState& state)
{
	ChurnAgg& C = g_churn;
	if (!C.init) {
		C.en = (getenv("MAPLECAST_CHURN") != nullptr);
		C.init = true;
		if (C.en) {
			C.ramSize = RAM_SIZE;                      // 16 MB on Dreamcast
			C.live = GetMemPtr(0x0C000000, C.ramSize); // contiguous mem_b base
			if (!C.live || C.ramSize == 0) { C.en = false; }
			else {
				C.shadow.assign(C.live, C.live + C.ramSize);
				C.regionBytes.assign((C.ramSize >> 16) + 1, 0);
				C.offChanged.assign(0x5A4, 0);
				// truncate the log on first init
				FILE* lg = fopen("/dev/shm/mc_churn.log", "w");
				if (lg) { fprintf(lg, "# MapleCast churn instrument — full 16MB mem_b per-frame diff\n"); fclose(lg); }
				printf("[CHURN] enabled: ramSize=%u live=%p — diffing full main RAM each frame\n",
				       C.ramSize, (void*)C.live);
				fflush(stdout);
			}
		}
	}
	if (!C.en) return;

	// Only measure in-match (the question is "in-match per-frame churn"). When
	// out of match we still resync the shadow so the first in-match frame isn't
	// a giant spurious delta.
	uint8_t* live = C.live;
	const uint32_t N = C.ramSize;

	if (!state.in_match) {
		memcpy(C.shadow.data(), live, N);
		return;
	}

	// --- byte diff: full RAM, region histogram, logical/rest split ---
	uint32_t frameChanged = 0;
	uint8_t* shadow = C.shadow.data();
	for (uint32_t r = 0; r < N; r += 0x10000) {
		uint32_t end = std::min(r + 0x10000u, N);
		uint32_t regChanged = 0;
		for (uint32_t i = r; i < end; i++) {
			if (live[i] != shadow[i]) { regChanged++; shadow[i] = live[i]; }
		}
		if (regChanged) {
			int region = (int)(r >> 16);
			C.regionBytes[region] += regChanged;
			frameChanged += regChanged;
			// logical = char-structs (region 0x26) + globals/pool (region 0x28).
			// 0x27 is the object-pool tail (0x..27xxxx) — engine-owned, treat as rest.
			if (region == 0x26 || region == 0x28) C.logicalChanged += regChanged;
			else                                  C.restChanged    += regChanged;
		}
	}

	// --- char-struct per-offset change map (union over the 6 active slots) ---
	// We already advanced the shadow above, so recompute the per-offset signal
	// from the GSTA-read live bytes vs a small dedicated per-slot shadow.
	{
		static uint8_t slotShadow[6][0x5A4];
		static bool    slotOk = false;
		for (int s = 0; s < 6; s++) {
			uint32_t baseOff = ramOff(CHAR_BASE[s]);
			if (baseOff + 0x5A4 > N) continue;
			uint8_t active = live[baseOff + 0x000];
			for (uint32_t o = 0; o < 0x5A4; o++) {
				uint8_t cur = live[baseOff + o];
				if (slotOk && active && cur != slotShadow[s][o])
					C.offChanged[o]++;   // counted once per frame per offset per slot
				slotShadow[s][o] = cur;
			}
		}
		slotOk = true;
		// NOTE: offChanged counts per-slot frame-changes; to keep %<=100 in the
		// report we normalize by (frames) which over-counts if >1 slot changes the
		// same offset in a frame. Acceptable: it ranks WHICH offsets are hot.
	}

	uint32_t fc = frameChanged;
	C.totalChanged += fc;
	if (fc < C.minFrame) C.minFrame = fc;
	if (fc > C.maxFrame) C.maxFrame = fc;
	C.frames++;

	if (C.frames >= 60) {
		churnReport(state);
		// reset the window accumulators (keep the shadow warm)
		C.frames = 0; C.totalChanged = 0; C.minFrame = 0xFFFFFFFFu; C.maxFrame = 0;
		C.logicalChanged = 0; C.restChanged = 0;
		std::fill(C.regionBytes.begin(), C.regionBytes.end(), 0);
		std::fill(C.offChanged.begin(), C.offChanged.end(), 0);
	}
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
	state.stage_anim_timer = (uint8_t)addrspace::read8(ADDR_STAGE_ANIM);

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
		// Prefer the STARTRENDER-phase latch (the sid actually rendered this frame); fall back
		// to the live read if no in-match STARTRENDER latched yet (finding:gsta_sprite_id_sampling_phase).
		c.sprite_id       = maplecast_oracle_hook::mc_sidLatchValid[i] ? maplecast_oracle_hook::mc_sidLatch[i]
		                                                              : (uint16_t)addrspace::read16(base + OFF_SPRITE_ID);
		c.animation_state = (uint16_t)addrspace::read16(base + OFF_ANIM_STATE);
		c.anim_timer      = maplecast_oracle_hook::mc_sidLatchValid[i] ? maplecast_oracle_hook::mc_timerLatch[i]
		                                                              : (uint16_t)addrspace::read16(base + OFF_ANIM_TIMER);
		c.health          = (uint8_t)addrspace::read8(base + OFF_HEALTH);
		c.red_health      = (uint8_t)addrspace::read8(base + OFF_RED_HEALTH);
		c.special_move_id = (uint8_t)addrspace::read8(base + OFF_SPECIAL_MOVE);
		c.assist_type     = (uint8_t)addrspace::read8(base + OFF_ASSIST_TYPE);
		c.palette_id      = (uint8_t)addrspace::read8(base + OFF_PALETTE);
		c.anim_pointer    = addrspace::read32(base + OFF_ANIM_POINTER);
		// GSTA enrich (step 1): quad-emitter / palette-handler inputs
		c.sprite_scale_x  = readFloat(base + OFF_SPRITE_SCALE_X);
		c.sprite_scale_y  = readFloat(base + OFF_SPRITE_SCALE_Y);
		c.pal_12d         = (uint8_t)addrspace::read8(base + OFF_PAL_12D);
		c.pal_12e         = (uint8_t)addrspace::read8(base + OFF_PAL_12E);
		c.overlay_1a4     = (uint8_t)addrspace::read8(base + OFF_OVERLAY_1A4);
		// GSTA wire extension (append-only +49..+56) — engine render/state bytes.
		c.render_extra    = (uint8_t)addrspace::read8(base + OFF_RENDER_EXTRA);
		c.facing_1d2      = (uint8_t)addrspace::read8(base + OFF_FACING_1D2);
		c.pal_color_25    = (uint8_t)addrspace::read8(base + OFF_COLOR);
		c.hyper_armor     = (uint8_t)addrspace::read8(base + OFF_HYPER_ARMOR);
		c.flight_flag     = (uint8_t)addrspace::read8(base + OFF_FLIGHT);
		c.stance          = (uint8_t)addrspace::read8(base + OFF_STANCE);
		c._pad            = 0;
		// draw_layer: find which slot-table layer holds this character's struct
		// (the slot table IS the draw list; mirrors readAllDrawn's walk). 0xFF = not
		// in any layer this frame. CONFIRMED reference_mvc2_slot_table_drawlist.
		{
			static const uint32_t SLOT_COUNT_BASE = 0x8C2895E0;
			static const uint32_t SLOT_PTR_BASE   = 0x8C287DE0;
			static const uint32_t SLOT_ROW_STRIDE = 0x180;
			uint8_t layer = 0xFF;
			for (int L = 0; L < 16 && layer == 0xFF; L++) {
				int cnt = (int)addrspace::read8(SLOT_COUNT_BASE + L);
				if (cnt <= 0 || cnt > 0x60) continue;
				uint32_t row = SLOT_PTR_BASE + (uint32_t)L * SLOT_ROW_STRIDE;
				for (int k = 0; k < cnt; k++)
					if (addrspace::read32(row + k * 4) == base) { layer = (uint8_t)L; break; }
			}
			c.draw_layer = layer;
		}
	}

	// Raw input state — read from the SAME kcode[]/lt[]/rt[] globals the
	// game reads at vblank. This is the authoritative input source.
	// NEVER hardcode button-to-action mappings on the client — always
	// read these values from the server's game state broadcast.
	state.p1_buttons = (uint16_t)(kcode[0] & 0xFFFF);
	state.p2_buttons = (uint16_t)(kcode[1] & 0xFFFF);
	state.p1_lt = (uint8_t)(lt[0] >> 8);
	state.p1_rt = (uint8_t)(rt[0] >> 8);
	state.p2_lt = (uint8_t)(lt[1] >> 8);
	state.p2_rt = (uint8_t)(rt[1] >> 8);

	ptrDump(state);
	partDump(state);     // read-only part-atlas capture probe (MAPLECAST_PARTDUMP=1)
	gfx1Dump(state);     // read-only CLEAN load-decode GFX1 parts by +6 selector (MAPLECAST_GFX1DUMP=1)
	effectsDump(state);  // read-only Effect Poly capture probe (MAPLECAST_DUMP_EFFECTS=1)
	churnDump(state);    // read-only full-RAM per-frame churn instrument (MAPLECAST_CHURN=1)
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
	addrspace::write8(ADDR_STAGE_ANIM, state.stage_anim_timer);

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
		// GSTA enrich (step 1): write back the same RAM offsets readGameState reads.
		writeFloat(base + OFF_SPRITE_SCALE_X, c.sprite_scale_x);
		writeFloat(base + OFF_SPRITE_SCALE_Y, c.sprite_scale_y);
		addrspace::write8(base + OFF_PAL_12D, c.pal_12d);
		addrspace::write8(base + OFF_PAL_12E, c.pal_12e);
		addrspace::write8(base + OFF_OVERLAY_1A4, c.overlay_1a4);
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

	// 6 characters × 49 bytes each (294 bytes) starting at offset 25
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
		// GSTA enrich (step 1) — appended so existing offsets +0..+37 are unchanged
		writeF32(buf, off, c.sprite_scale_x);  // +38
		writeF32(buf, off, c.sprite_scale_y);  // +42
		writeU8(buf, off, c.pal_12d);          // +46
		writeU8(buf, off, c.pal_12e);          // +47
		writeU8(buf, off, c.overlay_1a4);      // +48
		// GSTA wire extension (append-only) — struct order, +49..+56
		writeU8(buf, off, c.draw_layer);       // +49
		writeU8(buf, off, c.render_extra);     // +50
		writeU8(buf, off, c.facing_1d2);       // +51
		writeU8(buf, off, c.pal_color_25);     // +52
		writeU8(buf, off, c.hyper_armor);      // +53
		writeU8(buf, off, c.flight_flag);      // +54
		writeU8(buf, off, c.stance);           // +55
		writeU8(buf, off, c._pad);             // +56
		// total: 57 bytes per character
	}

	// Raw input state (8 bytes) — appended AFTER the 253-byte legacy block
	writeU16(buf, off, state.p1_buttons);
	writeU16(buf, off, state.p2_buttons);
	writeU8(buf, off, state.p1_lt);
	writeU8(buf, off, state.p1_rt);
	writeU8(buf, off, state.p2_lt);
	writeU8(buf, off, state.p2_rt);

	// Stage animation timer (1 byte) — appended last so older parsers that read
	// only 261 bytes are unaffected.
	writeU8(buf, off, state.stage_anim_timer);

	return off;  // WIRE_SIZE = 25 + 6*57 + 8 + 1 = 376
}

// Deserialize from network bytes back to GameState — exact reverse of serialize
static uint8_t readBufU8(const uint8_t* buf, int& off) { return buf[off++]; }
static uint16_t readBufU16(const uint8_t* buf, int& off) { uint16_t v; memcpy(&v, buf + off, 2); off += 2; return v; }
static uint32_t readBufU32(const uint8_t* buf, int& off) { uint32_t v; memcpy(&v, buf + off, 4); off += 4; return v; }
static float readBufF32(const uint8_t* buf, int& off) { float v; memcpy(&v, buf + off, 4); off += 4; return v; }

void deserialize(const uint8_t* buf, int len, GameState& state)
{
	// 367 = the char+global block (per-char stride 57 after the GSTA wire extension).
	// Raw input (+8) and stage_anim (+1) are optional trailers so the trailer parse
	// stays robust. (25 header + 6*57 = 367; full WIRE_SIZE = 376.)
	static const int LEGACY_SIZE = 5 + 2+2+2+2 + 4+4+4 + 6*57;  // 367
	if (len < LEGACY_SIZE) return;
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
		// GSTA enrich (step 1) — exact reverse of serialize's appended block
		c.sprite_scale_x  = readBufF32(buf, off);
		c.sprite_scale_y  = readBufF32(buf, off);
		c.pal_12d         = readBufU8(buf, off);
		c.pal_12e         = readBufU8(buf, off);
		c.overlay_1a4     = readBufU8(buf, off);
		// GSTA wire extension (append-only) — exact reverse of serialize, +49..+56
		c.draw_layer      = readBufU8(buf, off);
		c.render_extra    = readBufU8(buf, off);
		c.facing_1d2      = readBufU8(buf, off);
		c.pal_color_25    = readBufU8(buf, off);
		c.hyper_armor     = readBufU8(buf, off);
		c.flight_flag     = readBufU8(buf, off);
		c.stance          = readBufU8(buf, off);
		c._pad            = readBufU8(buf, off);
	}

	// Raw input state (8 bytes) — read if present (new format)
	if (len >= off + 8) {
		state.p1_buttons = readBufU16(buf, off);
		state.p2_buttons = readBufU16(buf, off);
		state.p1_lt      = readBufU8(buf, off);
		state.p1_rt      = readBufU8(buf, off);
		state.p2_lt      = readBufU8(buf, off);
		state.p2_rt      = readBufU8(buf, off);
	} else {
		state.p1_buttons = 0xFFFF;
		state.p2_buttons = 0xFFFF;
		state.p1_lt = state.p1_rt = state.p2_lt = state.p2_rt = 0;
	}

	// Stage animation timer (1 byte) — optional trailer.
	if (len >= off + 1)
		state.stage_anim_timer = readBufU8(buf, off);
	else
		state.stage_anim_timer = 0;
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
