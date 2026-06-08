/*
	MapleCast Frame Oracle — LIVE block-entry hook (EXACT per-quad attribution).
	See maplecast_oracle_hook.h for the mechanism + provenance.
*/
#include "maplecast_oracle_hook.h"
#include "hw/sh4/sh4_if.h"      // Sh4cntx (p_sh4rcb->cntx.r[16])
#include "hw/sh4/sh4_mem.h"     // addrspace::read*
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace maplecast_oracle_hook
{

// Initialized at static-init time (before main / before any block compiles) so
// the recompiler's compile-time gate is correct from the very first block. The
// MVC2 draw blocks at 0x8C033E90 only compile once the game reaches that code,
// well after static init, so there is no ordering hazard.
bool mc_oracleHookEnabled = (getenv("MAPLECAST_FRAME_ORACLE_HOOK") != nullptr);

// The two hooked guest PCs (CONFIRMED, marvelous2 bank03; FRAME-ORACLE-SPEC §Draw chain):
//   0x8C03093C loc_8c03093c "Render Main Sprite" — node=r4 (object begin)
//   0x8C033E90 loc_8c033e90 "reading sprite data" — quad emit (r8=texptr, r12=palptr, r14=cursor)
static const u32 PC_OBJ_BEGIN = 0x8C03093C;
static const u32 PC_QUAD_EMIT  = 0x8C033E90;
// Slot-walk restart (loc_8c0308c2 Render_sprites) — an alternate frame boundary.
// We flush on serverPublish() instead (simplest robust), but keep the PC here for
// reference; not hooked.

// --- Per-character struct field offsets (CONFIRMED, pl_mem.asm / CLAUDE.md / spec §2) ---
static const u32 OFF_CATEGORY   = 0x003;   // u8 render-layer/category
static const u32 OFF_SCALE_X    = 0x050;   // f32
static const u32 OFF_SCALE_Y    = 0x054;   // f32
static const u32 OFF_SCREEN_X   = 0x0E0;   // f32 (post-transform; written by this routine)
static const u32 OFF_SCREEN_Y   = 0x0E4;   // f32
static const u32 OFF_SPRITE_ID  = 0x144;   // u16
static const u32 OFF_GFX1       = 0x15C;   // ptr -> decoded GFX
static const u32 OFF_PAL_PTR    = 0x164;   // ptr -> live ARGB4444 palette
static const u32 OFF_EXTRAS     = 0x178;   // ptr -> sprite assembly / extras
static const u32 OFF_FILE_PTR   = 0x17C;   // ptr -> Dat_FilePointer
static const u32 OFF_FAC_PTR    = 0x184;   // ptr -> FAC
static const u32 OFF_FACING     = 0x1D2;   // u8 authoritative xflip

static inline bool inRam(u32 a) { return a >= 0x0C000000 && a < 0x10000000; }
static inline float rdF(u32 a) { u32 r = addrspace::read32(a); float f; memcpy(&f, &r, 4); return f; }

// GFX1-region classify (pattern at maplecast_gamestate.cpp:342-343 / spec §Source-address):
//   0x0CED0000–0x0CEE0000 = EFFECTS_BANK, 0x0CE60000 = DECOMP_BUF, else CHAR_GFX.
static const char* classifyRegion(u32 gfx1)
{
	u32 g = gfx1 & 0x0FFFFFFF;
	if (g >= 0x0CED0000 && g < 0x0CEE0000) return "EFFECTS_BANK";
	if (g >= 0x0CE60000 && g < 0x0CE70000) return "DECOMP_BUF";
	return "CHAR_GFX";
}

// --- Per-frame attribution buffer ---
struct Quad {
	u32 texptr;   // r8  at 0x8C033E90
	u32 palptr;   // r12 at 0x8C033E90
	u32 cursor;   // r14 at 0x8C033E90 (display-buffer cursor; quad geometry header)
	// The 16-byte quad the routine is about to emit lives at the cursor:
	//   {+0 w(u16), +2 h(u16), +4 attr(u32), +8 texptr(u32), +C palptr(u32)}
	// We read it back from r14 so the JSONL carries the real w/h/attr (it is
	// already coherent for THIS quad: r8/r12 are the inputs the routine writes).
	u16 w, h;
	u32 attr;
};

struct Obj {
	u32 node;
	int sprite_id;
	float sx, sy;
	float scaleX, scaleY;
	int category;
	int facing;
	u32 gfx1, pal, extras, file, fac;
	int nquads;
};

static const int MAX_OBJS  = 256;
static const int MAX_QUADS = 4096;
static Obj  s_objs[MAX_OBJS];
static int  s_nobj = 0;
static Quad s_quads[MAX_QUADS];
static int  s_nquad = 0;
static int  s_curObj = -1;   // index into s_objs of the object opened by the last 0x8C03093C

void mc_oracleInit()
{
	static bool logged = false;
	if (logged) return;
	logged = true;
	if (mc_oracleHookEnabled)
		fprintf(stderr, "[ORACLE-HOOK] ENABLED — live block-entry attribution "
		                "(0x%08X begin, 0x%08X quad) -> /dev/shm/mc_oracle_hook.jsonl\n",
		        PC_OBJ_BEGIN, PC_QUAD_EMIT);
}

bool mc_isHookedPC(u32 pc)
{
	return pc == PC_OBJ_BEGIN || pc == PC_QUAD_EMIT;
}

void DYNACALL mc_oracle_blockEntry(u32 pc)
{
	// Read-only. All guest regs coherent in Sh4cntx.r[] at this injection point.
	const u32* r = Sh4cntx.r;

	if (pc == PC_OBJ_BEGIN) {
		// node = r4 (the object/character struct being rendered).
		u32 node = r[4];
		if (!inRam(node)) { s_curObj = -1; return; }
		if (s_nobj >= MAX_OBJS) { s_curObj = -1; return; }
		Obj& o = s_objs[s_nobj];
		o.node     = node;
		o.sprite_id= (int)(u16)addrspace::read16(node + OFF_SPRITE_ID);
		o.sx       = rdF(node + OFF_SCREEN_X);
		o.sy       = rdF(node + OFF_SCREEN_Y);
		o.scaleX   = rdF(node + OFF_SCALE_X);
		o.scaleY   = rdF(node + OFF_SCALE_Y);
		o.category = (int)(u8)addrspace::read8(node + OFF_CATEGORY);
		o.facing   = (int)(u8)addrspace::read8(node + OFF_FACING);
		o.gfx1     = addrspace::read32(node + OFF_GFX1);
		o.pal      = addrspace::read32(node + OFF_PAL_PTR);
		o.extras   = addrspace::read32(node + OFF_EXTRAS);
		o.file     = addrspace::read32(node + OFF_FILE_PTR);
		o.fac      = addrspace::read32(node + OFF_FAC_PTR);
		o.nquads   = 0;
		s_curObj   = s_nobj;
		s_nobj++;
		return;
	}

	// pc == PC_QUAD_EMIT — one 16-byte quad attributed to the current object.
	if (s_nquad >= MAX_QUADS) return;
	u32 texptr = r[8];
	u32 palptr = r[12];
	u32 cursor = r[14];
	Quad& q = s_quads[s_nquad];
	q.texptr = texptr;
	q.palptr = palptr;
	q.cursor = cursor;
	// The routine reads dim bytes (w/h = dim<<3) and writes the quad header at r14.
	// At block ENTRY the header for THIS quad isn't written yet, so derive w/h from
	// the part blob is non-trivial here; record the inputs (texptr/palptr/cursor)
	// which ARE coherent, and leave w/h/attr as the prior-frame header at the
	// cursor (0 on first write). The OFFLINE attributor / differ keys on
	// texptr+palptr+cursor; w/h are advisory.
	q.w    = (u16)addrspace::read16(cursor + 0x0);
	q.h    = (u16)addrspace::read16(cursor + 0x2);
	q.attr = addrspace::read32(cursor + 0x4);
	s_nquad++;
	if (s_curObj >= 0 && s_curObj < s_nobj)
		s_objs[s_curObj].nquads++;
}

void mc_oracle_frameFlush(u32 frame)
{
	if (!mc_oracleHookEnabled) { s_nobj = 0; s_nquad = 0; s_curObj = -1; return; }

	// Capacity guard so a long session can't fill /dev/shm.
	static const long ORACLE_CAP = 64L * 1024 * 1024;
	static FILE* of = nullptr;
	static long  ow = 0;
	static bool  full = false;

	// Only emit a line for frames that actually drew something via the hook.
	if (s_nobj == 0) { s_nquad = 0; s_curObj = -1; return; }

	if (!full) {
		if (!of) of = fopen("/dev/shm/mc_oracle_hook.jsonl", "a");
		if (of && ow < ORACLE_CAP) {
			char b[2048]; int n = 0;
			n  = snprintf(b, sizeof b, "{\"frame\":%u,\"objects\":[", frame);
			ow += fwrite(b, 1, n, of);
			// quads are emitted in stream order; reconstruct per-object slices by
			// walking objects and consuming their nquads in order.
			int qbase = 0;
			for (int i = 0; i < s_nobj; i++) {
				const Obj& o = s_objs[i];
				n = snprintf(b, sizeof b,
					"%s{\"node\":\"0x%08X\",\"sprite_id\":%d,"
					"\"screen_xy\":[%.1f,%.1f],\"scale\":[%.3f,%.3f],"
					"\"category\":%d,\"facing\":%d,"
					"\"tex_src\":{\"gfx1_ptr\":\"0x%08X\",\"pal_ptr\":\"0x%08X\","
					"\"region\":\"%s\"},"
					"\"asm_src\":{\"extras_ptr\":\"0x%08X\",\"file_ptr\":\"0x%08X\","
					"\"fac_ptr\":\"0x%08X\"},\"quads\":[",
					i ? "," : "", o.node, o.sprite_id, o.sx, o.sy,
					o.scaleX, o.scaleY, o.category, o.facing,
					o.gfx1, o.pal, classifyRegion(o.gfx1),
					o.extras, o.file, o.fac);
				ow += fwrite(b, 1, n, of);
				for (int k = 0; k < o.nquads && qbase + k < s_nquad; k++) {
					const Quad& q = s_quads[qbase + k];
					n = snprintf(b, sizeof b,
						"%s{\"w\":%u,\"h\":%u,\"attr\":\"0x%08X\","
						"\"texptr\":\"0x%08X\",\"palptr\":\"0x%08X\","
						"\"cursor\":\"0x%08X\"}",
						k ? "," : "", q.w, q.h, q.attr, q.texptr, q.palptr, q.cursor);
					ow += fwrite(b, 1, n, of);
				}
				qbase += o.nquads;
				n = snprintf(b, sizeof b, "]}");
				ow += fwrite(b, 1, n, of);
			}
			n = snprintf(b, sizeof b, "]}\n");
			ow += fwrite(b, 1, n, of);
			fflush(of);
		} else if (of && ow >= ORACLE_CAP) {
			full = true;
			fprintf(stderr, "[ORACLE-HOOK] /dev/shm cap reached (%ld bytes) — stopping capture\n", ow);
		}
	}

	s_nobj = 0; s_nquad = 0; s_curObj = -1;
}

}
