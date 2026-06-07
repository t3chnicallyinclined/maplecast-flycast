/*
	MapleCast Game State — reads MVC2 state from Dreamcast RAM every frame.
	80-240 bytes per frame instead of 29,000 bytes of H.264.
*/
#include "maplecast_gamestate.h"
#include "hw/sh4/sh4_mem.h"

// Gamepad globals — authoritative input state read by the game at vblank
extern u32 kcode[4];
extern u16 lt[4], rt[4];

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
// Runtime pointers into the engine's DECODED structures (runtime-derivation probe).
// These are resolved by the engine when a character loads — following them reads
// the already-decompressed palette / assembly the GPU is using, no ROM codec.
static const uint32_t OFF_COLOR           = 0x025;  // live displayed palette idx (button + hit-flash)
static const uint32_t OFF_GFX00_PTR       = 0x15C;  // -> decoded GFX
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
					out[n].xflip      = (uint8_t)addrspace::read8(node + 0x130);
					out[n].owner_slot = (uint8_t)(slot < 0 ? 0 : slot);
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
	for (int layer = 0; layer < SLOT_LAYERS; layer++) {
		int count = (int)addrspace::read8(SLOT_COUNT_BASE + layer);
		if (count <= 0 || count > SLOT_MAX_ROW) continue;
		uint32_t row = SLOT_PTR_BASE + (uint32_t)layer * SLOT_ROW_STRIDE;
		for (int i = 0; i < count; i++) {
			uint32_t node = addrspace::read32(row + i * 4);
			if (node < 0x8C000000 || node >= 0x8D000000) continue;
			// Skip the 6 fighter bodies — they're shipped via GSTA already; the
			// slot table holds them too (avoid double-draw).
			bool isBody = false;
			for (int s = 0; s < 6; s++) if (node == CHAR_BASE[s]) { isBody = true; break; }
			if (isBody) continue;
			if (addrspace::read8(node + 0x12C) == 0) continue;     // renderer's visibility gate
			uint16_t sid = (uint16_t)addrspace::read16(node + 0x144);
			if (sid == 0) continue;
			float sx = readFloat(node + 0xE0), sy = readFloat(node + 0xE4);
			if (sx < -64.f || sx > 704.f || sy < -64.f || sy > 544.f) continue;
			// Owner is OPTIONAL (global effects have none). Check both conventions.
			int slot = -1;
			uint32_t oA = addrspace::read32(node + 0x18), oB = addrspace::read32(node + 0x80);
			for (int s = 0; s < 6; s++) if (oA == CHAR_BASE[s] || oB == CHAR_BASE[s]) { slot = s; break; }
			nPass++;
			if (n >= maxObjs) continue;
			out[n].owner_cid  = (uint8_t)(slot >= 0 ? addrspace::read8(CHAR_BASE[slot] + OFF_CHAR_ID) : 0);
			out[n].sprite_id  = sid;
			out[n].screen_x   = (int16_t)sx;
			out[n].screen_y   = (int16_t)sy;
			out[n].type       = (uint8_t)layer;                       // z-order: slot-table layer
			out[n].category   = (uint8_t)addrspace::read8(node + 0x03);
			out[n].xflip      = (uint8_t)addrspace::read8(node + 0x130);
			out[n].owner_slot = (uint8_t)(slot < 0 ? 0xFF : slot);
			n++;
		}
	}
	if (nPass > n)
		fprintf(stderr, "[OBJS-SLOT] CAP HIT: %d drawn, cap=%d -> DROPPED %d\n", nPass, maxObjs, nPass - n);
	else { static int _d = 0; if (++_d % 120 == 0) fprintf(stderr, "[OBJS-SLOT] drawn=%d (cap=%d)\n", n, maxObjs); }
	return n;
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
		out[n].xflip     = (uint8_t)addrspace::read8(a + 0x130);
		{ int os = 0; for (int s = 0; s < 6; s++) if (v == CHAR_BASE[s]) { os = s; break; } out[n].owner_slot = (uint8_t)os; }
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

		// sprite_id -> ASSEMBLY RE-KEY (Gap 2, FROM THE LIVE CELL — the ground truth).
		// LIVE LOG correction: `*(player+0x144) = 0x0000005d` is the plain sid 0x5d, NOT
		// a pointer (so the earlier `*(0x144)+0x18` read garbage). But `cell` (player+0x154
		// = current_cell_data) IS a valid pointer to the live 20-byte keyframe (log:
		// cell=0x0c52ebfc). The anim tick (bank03:loc_8c034ed2) copies that keyframe into
		// player+0x140.. via the Duff-copy bank12:loc_8c1294c8, so:
		//   cell      = read32(player+0x154)          ; valid keyframe pointer (live)
		//   live_sid  = read16(cell + 4)              ; == read16(player+0x144) (the client's key)
		//   slot      = read16(cell + 0x12)           ; keyframe -> EXTRAS slot index
		//   records   = EXTRAS + slot*0x400 + 0x08    ; assembly, 8-byte recs, mode==0xFF ends
		// Reading from the LIVE cell guarantees live_sid == read16(player+0x144) — unlike
		// (a) the offline ANIMATION-table scan (different namespace, never matched) and
		// (b) *(0x144) (not a pointer). Across fires the live sid set accumulates.
		//
		// File PL{hex}_sidasm.txt: "<sid> <slot> <nrecs> dx,dy,part,flip;...". If the
		// slot's assembly is empty, we dump the cell's 20 bytes so the real slot-field
		// offset is verifiable from the log.
		bool liveUsePart[256] = {false};   // part_idx referenced by the LIVE assembly (merged into usePart)
		{
			uint32_t animP = addrspace::read32(pbase + 0x168);   // ANIMATION base (dump for ref)
			if (_ramAddr(animP)) {
				char an[96]; snprintf(an, sizeof an, "/dev/shm/PL%02X_anim.bin", cid);
				FILE* af = fopen(an, "wb");
				if (af) { for (int k = 0; k < 0x8000; k++) fputc((uint8_t)addrspace::read8(animP + k), af); fclose(af); }
			}

			uint16_t sid144  = (uint16_t)addrspace::read16(pbase + 0x144);   // client's key (cross-check)
			uint16_t liveSid = sid144;
			uint16_t slot = 0xFFFF;
			uint32_t recs = 0;
			char kfbytes[64] = "-";
			if (_ramAddr(cellP)) {
				liveSid = (uint16_t)addrspace::read16(cellP + 4);            // keyframe[4] == sid144
				slot    = (uint16_t)addrspace::read16(cellP + 0x12);         // keyframe[0x12] = slot
				int n = 0; for (int b = 0; b < 20 && n < 60; b++) n += snprintf(kfbytes + n, sizeof(kfbytes) - n, "%02x", (uint8_t)addrspace::read8(cellP + b));
				if (slot < 64 && _ramAddr(exP))
					recs = exP + (uint32_t)slot * 0x400 + 0x08;
			}

			char km[96]; snprintf(km, sizeof km, "/dev/shm/PL%02X_sidasm.txt", cid);
			FILE* kf = fopen(km, "a");
			if (kf && ftell(kf) == 0)
				fprintf(kf, "# sprite_id slot nrecs dx,dy,part,flip;...  (LIVE cell: sid=kf[4], slot=kf[0x12], asm=EXTRAS+slot*0x400+8)\n");

			int nrec = 0;
			if (kf && recs) {
				char body[1900]; int bn = 0;
				for (int r = 0; r < 128; r++) {
					uint32_t rec = recs + (uint32_t)r * 8;
					int16_t rdx = (int16_t)addrspace::read16(rec);
					int16_t rdy = (int16_t)addrspace::read16(rec + 2);
					uint8_t rpart = (uint8_t)addrspace::read8(rec + 4);
					uint8_t rmode = (uint8_t)addrspace::read8(rec + 6);
					uint8_t rflip = (uint8_t)addrspace::read8(rec + 7);
					if (rmode == 0xFF) break;                       // assembly terminator
					uint32_t lo = addrspace::read32(rec);
					if (lo == 0 && rmode == 0 && rpart == 0 && rflip == 0) continue;   // pad
					if (bn < (int)sizeof(body) - 32)
						bn += snprintf(body + bn, sizeof(body) - bn, "%d,%d,%u,%u;",
						               rdx, rdy, rpart, (rflip & 0x80) ? 1 : 0);
					liveUsePart[rpart] = true;                      // ensure this part is dumped
					nrec++;
				}
				if (nrec > 0) fprintf(kf, "%u %u %d %s\n", liveSid, slot, nrec, body);
			}
			if (kf) fclose(kf);
			if (lg) {
				fprintf(lg, "[CHAR] LIVE re-key: sid144=0x%04x cell=0x%08x kf[4]=0x%04x slot=kf[0x12]=%u recs@0x%08x nrec=%d\n",
				        sid144, cellP, liveSid, slot, recs, nrec);
				fprintf(lg, "[CHAR]   cell kf[0..19] = %s\n", kfbytes);
				if (nrec == 0) fprintf(lg, "[CHAR]   *** empty assembly — verify slot field offset against the kf bytes above ***\n");
			}
		}

		// Collect part_idx referenced by EVERY assembly slot in the EXTRAS table, not
		// just the first — the EXTRAS table is 0x400-byte slots, each a 128-record (8B)
		// assembly ending at mode==0xFF. Walking only slot 0 (the old bug) pinned the
		// dump to one fixed assembly regardless of sid. Scanning all slots accumulates
		// the character's FULL part set; the per-fire sid log lets us correlate which
		// slot is live. (Header is 0x18 bytes before slot 0's records — GFX-NOTES §3.)
		bool usePart[256] = {false};
		int nUse = 0, nSlots = 0;
		if (_ramAddr(exP)) {
			const int SLOT = 0x400, NREC = SLOT / 8;
			for (int slot = 0; slot < 64; slot++) {           // up to 64 slots (0x10000)
				uint32_t sbase = exP + 0x18 + (uint32_t)slot * SLOT;
				bool slotHasPart = false;
				for (int r = 0; r < NREC; r++) {
					uint32_t rec = sbase + (uint32_t)r * 8;
					uint8_t part = (uint8_t)addrspace::read8(rec + 4);
					uint8_t mode = (uint8_t)addrspace::read8(rec + 6);
					if (mode == 0xFF) break;                   // assembly terminator
					// skip all-zero pad records
					uint32_t lo = addrspace::read32(rec);
					if (lo == 0 && mode == 0 && part == 0) continue;
					if (!usePart[part]) { usePart[part] = true; nUse++; }
					slotHasPart = true;
				}
				if (slotHasPart) nSlots++;
			}
		}
		// Merge the LIVE assembly's parts in (guarantees every part the on-screen sid
		// references gets dumped, even if the slot-scan didn't reach it).
		for (int p = 0; p < 256; p++) if (liveUsePart[p] && !usePart[p]) { usePart[p] = true; nUse++; }
		if (lg) fprintf(lg, "[CHAR] EXTRAS scan: %d non-empty slots, %d distinct part_idx (incl live)\n",
		                nSlots, nUse);

		// Per-fire compact log: sid + the part_idx list (so we see it change across fires).
		// Also appended to a sid-trace file that survives across fires (the main log is
		// rewritten each fire because it carries the big directory dump).
		{
			char pl[512]; int pn = snprintf(pl, sizeof pl, "[FIRE %d] cid=%u(PL%02X) sid=%u cell=0x%08x parts=",
			                                fires, cid, cid, sid, cellP);
			for (int p = 0; p < 256 && pn < (int)sizeof(pl) - 6; p++) if (usePart[p]) pn += snprintf(pl + pn, sizeof(pl) - pn, "%d ", p);
			if (lg) fprintf(lg, "%s\n", pl);
			FILE* tf = fopen("/dev/shm/mc_partdump_sidtrace.log", "a");
			if (tf) { fprintf(tf, "%s\n", pl); fclose(tf); }
		}

		// Dump each referenced part via dir_entry(charBase + part_idx): RAW texels (for
		// offline format-locking) + a best-effort PPM preview. Manifest lines carry the
		// resolved key + all fields so the packer can verify; deduped by part_idx.
		char mn[96]; snprintf(mn, sizeof mn, "/dev/shm/PL%02X_parts.manifest", cid);
		FILE* mf = fopen(mn, "a");   // append: full atlas accumulates across frames
		if (mf && ftell(mf) == 0)
			fprintf(mf, "# part_idx key raw ppm w h e4 texptr ec rawbytes tcw fmt twid descU16 desc[0x3Chex]\n");
		for (int part = 0; part < 256; part++) {
			if (!usePart[part]) continue;
			int key = charBase + part;
			uint32_t e  = dirBase + (uint32_t)key * 0x10;
			uint32_t e0 = addrspace::read32(e),     e4 = addrspace::read32(e + 4);
			uint32_t e8 = addrspace::read32(e + 8), ec = addrspace::read32(e + 12);
			int w = e0 & 0xffff, h = (e0 >> 16) & 0xffff;
			// FOLLOW THE DESCRIPTOR (the game's own resolution): e4 is a descriptor
			// index, not the format. partResolveTCW walks key -> u16 -> 0x20-stride
			// descriptor -> TCW@+0xC, and we read fmt/scan from that TCW. Fall back to
			// the e4-byte1 heuristic only if the runtime tables aren't resolvable.
			uint32_t descAddr = 0; uint16_t descU16 = 0xFFFF;
			uint32_t tcw = partResolveTCW(key, &descAddr, &descU16);
			int  fmt; bool linear;
			if (tcw != 0) { fmt = tcwFmt(tcw); linear = tcwLinear(tcw); }
			else          { fmt = partFmtFromE4(e4); linear = false; }   // fallback
			if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || !_ramAddr(e8)) {
				if (mf) fprintf(mf, "%d %d - - %d %d %08x %08x %08x SKIP\n", part, key, w, h, e4, e8, ec);
				continue;
			}
			char rfn[96]; snprintf(rfn, sizeof rfn, "/dev/shm/PL%02X_part_%03d.raw", cid, part);
			char pfn[96]; snprintf(pfn, sizeof pfn, "/dev/shm/PL%02X_part_%03d.ppm", cid, part);
			// RAW byte count tracks the format: 16-bit = w*h*2, PAL8 = w*h, PAL4 = w*h/2.
			int rawBytes = (fmt == 5) ? (w * h / 2) : (fmt == 6) ? (w * h) : (w * h * 2);
			partDumpRawN(e8, rawBytes, rfn);
			partDecodeToPPM(e8, w, h, fmt, linear, palP, pfn, false);   // flycast-canonical
			// For LARGE (>=64px) parts, also emit the transposed (x-first) twiddle so the
			// 256x256 body can be A/B'd offline against the oracle (the small parts decoded
			// under one order; this reveals if large parts need the other).
			if (!linear && w >= 64 && h >= 64) {
				char pfa[96]; snprintf(pfa, sizeof pfa, "/dev/shm/PL%02X_part_%03d.altTw.ppm", cid, part);
				partDecodeToPPM(e8, w, h, fmt, linear, palP, pfa, true);
			}
			// Manifest: "...e4 texptr ec rawbytes tcw fmt twid descU16 desc[0x3C]".
			// Full 0x3C descriptor (the e4-indexed table stride) is dumped so any sub-rect
			// / stride / page-UV field for the 256x256 body is visible OFFLINE (to confirm
			// whether a large part is one twiddled texture or a composite page).
			char dh[140] = "-";
			if (_ramAddr(descAddr)) { int n = 0; for (int b = 0; b < 0x3C && n < 132; b++) n += snprintf(dh + n, sizeof(dh) - n, "%02x", (uint8_t)addrspace::read8(descAddr + b)); }
			if (mf) fprintf(mf, "%d %d PL%02X_part_%03d.raw PL%02X_part_%03d.ppm %d %d %08x %08x %08x %d %08x %d %s %04x %s\n",
			                part, key, cid, part, cid, part, w, h, e4, e8, ec, rawBytes,
			                tcw, fmt, linear ? "linear" : "twid", descU16, dh);
			dumpedTotal++;
		}
		if (mf) fclose(mf);
		if (lg) fprintf(lg, "[CHAR] manifest -> %s (append, raw+ppm)\n", mn);
	}
	if (lg) { fprintf(lg, "\ndumped %d parts this frame\n", dumpedTotal); fclose(lg); }
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
	partDump(state);   // read-only part-atlas capture probe (MAPLECAST_PARTDUMP=1)
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

	return off;  // WIRE_SIZE = 253 + 8 + 1 = 262
}

// Deserialize from network bytes back to GameState — exact reverse of serialize
static uint8_t readBufU8(const uint8_t* buf, int& off) { return buf[off++]; }
static uint16_t readBufU16(const uint8_t* buf, int& off) { uint16_t v; memcpy(&v, buf + off, 2); off += 2; return v; }
static uint32_t readBufU32(const uint8_t* buf, int& off) { uint32_t v; memcpy(&v, buf + off, 4); off += 4; return v; }
static float readBufF32(const uint8_t* buf, int& off) { float v; memcpy(&v, buf + off, 4); off += 4; return v; }

void deserialize(const uint8_t* buf, int len, GameState& state)
{
	// 253 = the legacy char+global block. Raw input (+8) and stage_anim (+1)
	// are optional trailers so older/newer packets both parse.
	static const int LEGACY_SIZE = 5 + 2+2+2+2 + 4+4+4 + 6*38;  // 253
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
