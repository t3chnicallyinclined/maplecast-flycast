/*
	MapleCast Frame Oracle — per-frame per-object SCREEN quads.

	GOAL: every in-match frame, for every drawn object, its sprite_id + live
	screen_xy + the SCREEN quads (real on-screen x,y/w,h, UV, VRAM tex src) it drew.

	MECHANISM (two cooperating parts):
	  1. SH4 block-entry hook on 0x8C03093C "Render Main Sprite" (loc_8c03093c).
	     Fires PER-OBJECT PER-FRAME during gameplay (confirmed live: 2 objects/frame
	     with real post-transform screen_xy). It reads each object node (r4): node
	     base, sprite_id@+0x144, screen_xy@+0xE0/E4 (the value THIS routine just
	     wrote = exactly what the GPU placed), scale, facing, the asm-ptr cluster.
	     This is the AUTHORITATIVE per-object anchor list.
	  2. mc_oracle_frameFlush(ctx) runs once/frame in serverPublish AFTER the SH4
	     draw walk. It ta_parse()'s the completed TA list to recover the per-frame
	     SCREEN quads (rc.verts x,y), classifies sprite quads (reusing the proven
	     serverPublish de-index + filter), and attributes each to the nearest
	     OBJ_BEGIN object by on-screen position. Unmatched quads -> "unassigned".

	WHY NOT the loc_8c033e90 quad emitter: that routine is the LOAD-TIME part-atlas
	decode — it fires ONCE at match start (proven: 1 of 22197 frames), dumping ~1190
	parts into the 0x0CE60000 decode buffer with NO screen coords. It's the wrong
	routine for placement. Its 16-byte decode records are still capturable under the
	MAPLECAST_FRAME_ORACLE_DECODE sub-flag (off by default).

	Read-only: reads Sh4cntx.r[] + guest RAM; ta_parse builds ctx->rend (re-parsed
	later for the wire) -> determinism-safe, perf-trivial, gated MAPLECAST_FRAME_ORACLE_HOOK.
	See maplecast_oracle_hook.h for the recompiler injection point.
*/
#include "maplecast_oracle_hook.h"
#include "hw/sh4/sh4_if.h"      // Sh4cntx (p_sh4rcb->cntx.r[16])
#include "hw/sh4/sh4_mem.h"     // addrspace::read*
#include "hw/pvr/ta_ctx.h"      // TA_context, rend_context, PolyParam, Vertex
#include "hw/pvr/ta.h"          // ta_parse
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace maplecast_oracle_hook
{

// Initialized at static-init time (before main / before any block compiles) so
// the recompiler's compile-time gate is correct from the very first block. The
// MVC2 draw blocks at 0x8C033E90 only compile once the game reaches that code,
// well after static init, so there is no ordering hazard.
bool mc_oracleHookEnabled = (getenv("MAPLECAST_FRAME_ORACLE_HOOK") != nullptr);

// Sub-flag: also capture the LOAD-TIME part-atlas decode quads at loc_8c033e90
// (0x8C033EC0 post-write). PROVEN (live prod capture 2026-06-08): that routine
// only fires at MATCH LOAD (~frame 2568) and dumps all ~1190 parts into the
// 0x0CE60000 decode buffer with NO screen coords — it is the part-ATLAS decode,
// NOT the per-frame render. So the quad hook is OFF by default now; the PRIMARY
// per-frame SCREEN quads come from ta_parse in mc_oracle_frameFlush(). Set
// MAPLECAST_FRAME_ORACLE_DECODE=1 to additionally log the one-shot atlas decode.
static bool mc_decodeQuadsEnabled = (getenv("MAPLECAST_FRAME_ORACLE_DECODE") != nullptr);

// The two hooked guest PCs (CONFIRMED, marvelous2 bank03; FRAME-ORACLE-SPEC §Draw chain):
//   0x8C03093C loc_8c03093c "Render Main Sprite" — node=r4 (object begin)
//   0x8C033E90 loc_8c033e90 "reading sprite data" — quad emit (r8=texptr, r12=palptr, r14=cursor)
// NOTE: the disassembly labels are P1 (cached, 0x8C..) addresses, but MVC2 actually
// EXECUTES this code from the P0 region — the recompiler's block->vaddr for these
// routines is 0x0C03093C / 0x0C033E90 (same low 28 bits, high nibble differs). So we
// compare on the SH4 area-masked PC (& 0x1FFFFFFF) which normalizes every cached/
// uncached alias (P0/P1/P2) of the same RAM line to 0x0C.. — see mc_isHookedPC.
// (Root cause of the "hook never fires" bug fixed 2026-06-08.)
static const u32 PC_OBJ_BEGIN = 0x8C03093C;
// PC_QUAD_DONE is the POST-WRITE capture point. loc_8c033e90 (block entry) is
// BEFORE the routine writes the 16-byte quad, so reading the quad there returns
// not-yet-written garbage (the prod symptom: w=58572 h=60718). Tracing the emit
// loop in marvelous2/build/bank03.asm (loc_8c033e90, lines 9258-9301) the quad
// header is FULLY written by the time PC reaches 0x8C033EC0 (right after
// `mov.l r12,@(0xC,r14)` @9284) and BEFORE the cursor advances `add 0x10,r14`
// @9294 (PC 0x8C033ED2). So at 0x8C033EC0:
//   - the 16 bytes at r14 are the real quad {w@+0,h@+2,attr@+4,texptr@+8,palptr@+C}
//   - r14 still points AT this quad (not yet advanced)
//   - r8=texptr, r12=palptr are still live
//   - r10 = the node/object base (set `mov r4,r10` @9103 in the loc_8c033d78
//     prologue, never clobbered through the loop) -> attribute the quad to its
//     REAL object directly (no dependence on OBJ_BEGIN ordering -> no orphans).
static const u32 PC_QUAD_DONE  = 0x8C033EC0;
// SH4 external-area mask: drops the P0/P1/P2/U0 cache/region bits so any alias of a
// RAM line compares equal. 0x8C03093C & MASK == 0x0C03093C == 0x0C03093C & MASK.
static const u32 SH4_AREA_MASK = 0x1FFFFFFF;
static const u32 PC_OBJ_BEGIN_M = PC_OBJ_BEGIN & SH4_AREA_MASK;  // 0x0C03093C
static const u32 PC_QUAD_DONE_M = PC_QUAD_DONE & SH4_AREA_MASK;  // 0x0C033EC0
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

// Normalize any P0/P1/P2 alias of a guest pointer to the external-area form so
// the same RAM line keys identically regardless of which alias a register holds
// (r4 at OBJ_BEGIN tends to be P0 0x0C..; r10 in the emitter may be P1 0x8C..).
static inline u32 norm(u32 a) { return a & 0x1FFFFFFF; }
static inline bool inRam(u32 a) { u32 m = norm(a); return m >= 0x0C000000 && m < 0x10000000; }
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
	u32 texptr;   // r8  at 0x8C033EC0 (post-write) — quad+0x8
	u32 palptr;   // r12 at 0x8C033EC0 (post-write) — quad+0xC
	u32 cursor;   // r14 at 0x8C033EC0 (display-buffer cursor; STILL points at THIS quad)
	// At the post-write PC the 16-byte quad at the cursor is FULLY written:
	//   {+0 w(u16), +2 h(u16), +4 attr(u32), +8 texptr(u32), +C palptr(u32)}
	// so w/h/attr are now REAL (sane ~8-256px), not the prior-frame garbage.
	u16 w, h;
	u32 attr;
	int obj;      // index into s_objs this quad is attributed to (by node addr)
};

struct Obj {
	u32 node;
	int sprite_id;
	float sx, sy;
	float scaleX, scaleY;
	int category;
	int facing;
	u32 gfx1, pal, extras, file, fac;
	int nquads;       // LOAD-decode quads (sub-flag) attributed to this object
	int nscreen;      // SCREEN quads (ta_parse) attributed to this object this frame
	bool enriched;    // true once OBJ_BEGIN (0x8C03093C) read the node fields
	bool fromBegin;   // captured by OBJ_BEGIN this frame (vs. quad-only)
};

// A per-frame SCREEN quad recovered from ta_parse(ctx) in mc_oracle_frameFlush:
// real on-screen x/y/w/h, UV sub-rect, depth range, and the VRAM texture source.
// These are the quads the GPU actually drew — the per-object placement anchor.
struct ScreenQuad {
	float x, y, w, h;       // screen-space bbox
	float cx, cy;           // bbox center (attribution anchor)
	float uMn, uMx, vMn, vMx;
	float zMn, zMx;
	u32 tcw, tsp, pcw, vramAddr;
	int  fmt, tw, th, vq, srcBlend, dstBlend;
	int  obj;               // attributed object index (-1 = unassigned)
};

static const int MAX_OBJS   = 256;
static const int MAX_QUADS  = 4096;
static const int MAX_SCREEN = 4096;
static Obj  s_objs[MAX_OBJS];
static int  s_nobj = 0;
static Quad s_quads[MAX_QUADS];     // load-decode quads (sub-flag)
static int  s_nquad = 0;
static ScreenQuad s_screen[MAX_SCREEN]; // per-frame SCREEN quads (ta_parse)
static int  s_nscreen = 0;
// (s_curObj removed: quad attribution is now by node addr (r10), not by the last
//  OBJ_BEGIN — see findOrCreateObj. No "current object" state to track.)

// Read the char-struct-shaped node fields into an Obj (the node base is r4 at
// OBJ_BEGIN / r10 at the quad-done PC; both point at a 0x5A4-stride render record).
static void enrichObj(Obj& o, u32 node)
{
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
	o.enriched = true;
}

// GFX1-region tag a screen quad would correlate to, for the attribution refine.
// (Not used as the primary key — position is — but disambiguates two objects at
//  nearly the same screen point by their decode-region class.)

// Find the object for this node, or create one. Attribution is by NODE ADDRESS
// (r10 at the quad-done PC), NOT by "the last OBJ_BEGIN" — the quad emitter
// (loc_8c033d78) is dispatched from the cell-processor jump table, decoupled from
// loc_8c03093c (Render Main Sprite), so a quad can fire with no immediately
// preceding OBJ_BEGIN for the same object. Keying on the node base lands every
// quad on its real object and eliminates the orphan bucket.
static int findOrCreateObj(u32 node)
{
	for (int i = s_nobj - 1; i >= 0; i--)
		if (s_objs[i].node == node) return i;
	if (s_nobj >= MAX_OBJS) return -1;
	Obj& o = s_objs[s_nobj];
	memset(&o, 0, sizeof o);
	o.sprite_id = -1; o.category = -1; o.facing = -1;
	enrichObj(o, node);
	return s_nobj++;
}

void mc_oracleInit()
{
	static bool logged = false;
	if (logged) return;
	logged = true;
	if (mc_oracleHookEnabled)
		fprintf(stderr, "[ORACLE-HOOK] ENABLED — per-frame per-object SCREEN quads: "
		                "OBJ_BEGIN 0x%08X (live screen_xy) + ta_parse screen quads attributed "
		                "by position%s -> /dev/shm/mc_oracle_hook.jsonl\n",
		        PC_OBJ_BEGIN,
		        mc_decodeQuadsEnabled ? " (+DECODE quad sub-flag at 0x8C033EC0)" : "");
}

bool mc_isHookedPC(u32 pc)
{
	// Compare on the area-masked PC so the cached (0x8C..) disasm label matches the
	// physical/P0 (0x0C..) address the recompiler actually compiles from (and any
	// other alias). This is the FIX for the never-fires bug: block->vaddr is 0x0C..,
	// the literals are 0x8C.., and an exact == never matched.
	u32 m = pc & SH4_AREA_MASK;
	if (m == PC_OBJ_BEGIN_M) return true;
	// The quad-done PC is the LOAD-TIME atlas decode (fires once at match start, no
	// screen coords) — only hook it when the decode sub-flag is set so normal runs
	// don't inject a call into a block that never carries useful per-frame data.
	if (m == PC_QUAD_DONE_M) return mc_decodeQuadsEnabled;
	return false;
}

// Per-PC fire counters (DIAGNOSTIC, gated). The previous proof-of-life used a
// SINGLE shared one-shot flag, so once QUAD_EMIT fired first it consumed the log
// and we could NOT tell whether OBJ_BEGIN ever fires. These split counters answer
// task item #1 directly: OBJ_BEGIN fire count vs QUAD_EMIT fire count.
static unsigned long s_fireObjBegin = 0;
static unsigned long s_fireQuad     = 0;

void DYNACALL mc_oracle_blockEntry(u32 pc)
{
	// Read-only. All guest regs coherent in Sh4cntx.r[] at this injection point.
	const u32* r = Sh4cntx.r;

	// Mask to the SH4 external area so we route correctly whether the recompiler
	// passed the cached (0x8C..) or physical (0x0C..) alias of the PC.
	u32 mpc = pc & SH4_AREA_MASK;

	if (mpc == PC_OBJ_BEGIN_M) {
		if (s_fireObjBegin++ == 0)
			fprintf(stderr, "[ORACLE-HOOK] OBJ_BEGIN first fired (pc=0x%08X masked 0x%08X)\n",
			        pc, mpc);
		// node = r4 (the object/character struct being rendered). OBJ_BEGIN now only
		// PRE-ENRICHES the object record (screen_xy/scale/sprite_id from the node);
		// quad attribution at the quad-done PC keys on the node addr independently.
		u32 node = norm(r[4]);
		if (!inRam(node)) return;
		int oi = findOrCreateObj(node);
		if (oi >= 0) { enrichObj(s_objs[oi], node); s_objs[oi].fromBegin = true; }  // refresh post-transform screen_xy
		return;
	}

	// Below this point only the optional LOAD-decode quad capture runs. When the
	// decode sub-flag is OFF, the quad PC isn't hooked at all (mc_isHookedPC) so we
	// never reach here for it — but guard anyway.
	if (!mc_decodeQuadsEnabled) return;

	// mpc == PC_QUAD_DONE_M — the POST-WRITE capture point (0x8C033EC0). The 16-byte
	// quad at r14 is now FULLY written and r14 has NOT yet advanced. r10 = node base.
	if (mpc != PC_QUAD_DONE_M) return;
	if (s_fireQuad++ == 0)
		fprintf(stderr, "[ORACLE-HOOK] QUAD_DONE first fired (pc=0x%08X masked 0x%08X)\n",
		        pc, mpc);
	if (s_nquad >= MAX_QUADS) return;

	// Attribute to the REAL object by node base (r10), not by OBJ_BEGIN ordering.
	u32 node = norm(r[10]);
	int oi;
	if (inRam(node)) {
		oi = findOrCreateObj(node);
		if (oi < 0) return;  // object table full
	} else {
		// node unreadable — bucket as orphan (node 0) rather than drop the quad.
		oi = -1;
		for (int i = s_nobj - 1; i >= 0; i--)
			if (s_objs[i].node == 0) { oi = i; break; }
		if (oi < 0) {
			if (s_nobj >= MAX_OBJS) return;
			Obj& po = s_objs[s_nobj];
			memset(&po, 0, sizeof po);
			po.sprite_id = -1; po.category = -1; po.facing = -1;
			oi = s_nobj++;
		}
	}

	u32 cursor = r[14];          // r14 STILL points AT this fully-written quad
	Quad& q = s_quads[s_nquad];
	q.obj    = oi;
	q.cursor = cursor;
	// Read the fully-written 16-byte quad header straight from the display buffer:
	//   {w@+0, h@+2, attr@+4, texptr@+8, palptr@+C}. These are now the REAL values.
	q.w      = (u16)addrspace::read16(cursor + 0x0);
	q.h      = (u16)addrspace::read16(cursor + 0x2);
	q.attr   = addrspace::read32(cursor + 0x4);
	q.texptr = addrspace::read32(cursor + 0x8);   // == r8  (cross-check available)
	q.palptr = addrspace::read32(cursor + 0xC);   // == r12
	s_nquad++;
	s_objs[oi].nquads++;
}

// ---- Per-frame SCREEN-quad recovery from ta_parse(ctx) ---------------------
// loc_8c033e90 (the LOAD-decode quad emitter) does NOT fire per frame during
// gameplay (proven: it ran once at match load, frame 2568). The per-frame SCREEN
// quads the GPU actually draws are in the TA list — recovered by ta_parse here.
// This reuses the EXACT de-index + sprite-classifier proven in serverPublish's
// MAPLECAST_FRAME_ORACLE block (maplecast_mirror.cpp ~2494-2588): try rc.idx
// de-index, fall back to direct rc.verts (autosort-tr sprites), filter out
// clears / page-tiled bg / oversized stage layers / opaque fills.
static void collectScreenQuads(rend_context& rc)
{
	s_nscreen = 0;
	const u32 nverts = (u32)rc.verts.size();
	auto collect = [&](std::vector<PolyParam>& lst) {
		for (PolyParam& pp : lst) {
			if (s_nscreen >= MAX_SCREEN) return;
			if (pp.count < 3) continue;
			u32 pcw = pp.pcw.full, tcw = pp.tcw.full, tsp = pp.tsp.full;
			bool textured = ((pcw >> 3) & 1) != 0;
			float mnX=1e9f,mxX=-1e9f,mnY=1e9f,mxY=-1e9f;
			float uMn=1e9f,uMx=-1e9f,vMn=1e9f,vMx=-1e9f;
			float zMn=1e30f,zMx=-1e30f;
			int seen = 0;
			{   // primary: pp.first/.count index rc.idx (op/pt + non-autosort tr)
				u32 iend = pp.first + pp.count; if (iend > rc.idx.size()) iend = (u32)rc.idx.size();
				for (u32 k = pp.first; k < iend; k++) {
					u32 vi = rc.idx[k]; if (vi >= nverts) continue;
					const Vertex& vt = rc.verts[vi];
					if (vt.x<mnX)mnX=vt.x; if (vt.x>mxX)mxX=vt.x;
					if (vt.y<mnY)mnY=vt.y; if (vt.y>mxY)mxY=vt.y;
					if (vt.u<uMn)uMn=vt.u; if (vt.u>uMx)uMx=vt.u;
					if (vt.v<vMn)vMn=vt.v; if (vt.v>vMx)vMx=vt.v;
					if (vt.z<zMn)zMn=vt.z; if (vt.z>zMx)zMx=vt.z;
					seen++;
				}
			}
			if (seen == 0) {   // autosort tr: pp.first/.count index rc.verts directly
				u32 vend = pp.first + pp.count; if (vend > nverts) vend = nverts;
				for (u32 v = pp.first; v < vend; v++) {
					const Vertex& vt = rc.verts[v];
					if (std::isnan(vt.x) || fabsf(vt.x) > 1e25f || std::isnan(vt.y) || fabsf(vt.y) > 1e25f) continue;
					if (vt.x<mnX)mnX=vt.x; if (vt.x>mxX)mxX=vt.x;
					if (vt.y<mnY)mnY=vt.y; if (vt.y>mxY)mxY=vt.y;
					if (vt.u<uMn)uMn=vt.u; if (vt.u>uMx)uMx=vt.u;
					if (vt.v<vMn)vMn=vt.v; if (vt.v>vMx)vMx=vt.v;
					if (vt.z<zMn)zMn=vt.z; if (vt.z>zMx)zMx=vt.z;
					seen++;
				}
			}
			if (seen == 0) continue;
			float w = mxX-mnX, h = mxY-mnY;
			if (w < 2.f || h < 2.f) continue;
			float cy = (mnY+mxY)*0.5f; if (cy <= 20.f) continue;   // strip top HUD row
			int srcB = (int)((tsp>>29)&7), dstB = (int)((tsp>>26)&7);
			bool tiled = (uMn < -0.05f || uMx > 1.05f || vMn < -0.05f || vMx > 1.05f);
			bool opaque = (srcB == 1 && dstB == 0);
			bool oversized = (w > 200.f || h > 200.f);
			bool isSprite = textured && !tiled && !opaque && !oversized && tcw != 0;
			if (!isSprite) continue;
			ScreenQuad& q = s_screen[s_nscreen++];
			q.x=mnX; q.y=mnY; q.w=w; q.h=h; q.cx=(mnX+mxX)*0.5f; q.cy=cy;
			q.uMn=uMn; q.uMx=uMx; q.vMn=vMn; q.vMx=vMx;
			q.zMn=(zMn> 1e29f)?0.f:zMn; q.zMx=(zMx<-1e29f)?0.f:zMx;
			q.tcw=tcw; q.tsp=tsp; q.pcw=pcw; q.srcBlend=srcB; q.dstBlend=dstB;
			q.fmt = (int)((tcw>>27)&7); q.vq = (int)((tcw>>30)&1);
			q.tw = 8 << ((tsp>>3)&7); q.th = 8 << (tsp&7);
			q.vramAddr = (tcw & 0x1FFFFF) << 3;
			q.obj = -1;
		}
	};
	collect(rc.global_param_op);
	collect(rc.global_param_pt);
	collect(rc.global_param_tr);
}

// Attribute each SCREEN quad to the nearest OBJ_BEGIN object by on-screen
// distance (the OBJ_BEGIN screen_xy is the AUTHORITATIVE post-transform position
// the renderer wrote — exactly what the GPU placed the object at). A quad with no
// object within ATTR_RADIUS stays obj=-1 (emitted in the frame "unassigned"
// bucket so nothing is lost / the offline differ can still see it). Position is
// the only per-frame per-object signal available (loc_8c033e90 doesn't fire per
// frame), so this is a clean nearest-anchor over the LIVE screen_xy list — much
// tighter than the slot-table re-read because anchors are the routine's own output.
static void attributeScreenQuads()
{
	for (int i = 0; i < s_nobj; i++) s_objs[i].nscreen = 0;
	const float ATTR_RADIUS = 160.f;   // px; tunable. Parts cluster <120px from screen_xy.
	for (int k = 0; k < s_nscreen; k++) {
		ScreenQuad& q = s_screen[k];
		int best = -1; float bestD2 = ATTR_RADIUS * ATTR_RADIUS;
		for (int i = 0; i < s_nobj; i++) {
			const Obj& o = s_objs[i];
			if (!o.fromBegin) continue;          // only anchor on routine-confirmed objects
			if (o.sx == 0.f && o.sy == 0.f) continue;
			float dx = q.cx - o.sx, dy = q.cy - o.sy;
			float d2 = dx*dx + dy*dy;
			if (d2 < bestD2) { bestD2 = d2; best = i; }
		}
		q.obj = best;
		if (best >= 0) s_objs[best].nscreen++;
	}
}

void mc_oracle_frameFlush(void* ctxv, u32 frame)
{
	if (!mc_oracleHookEnabled) { s_nobj = 0; s_nquad = 0; s_nscreen = 0; return; }

	// Recover this frame's SCREEN quads from the completed TA list and attribute
	// them to the OBJ_BEGIN objects. ta_parse here is read-only w.r.t. guest state
	// (it builds ctx->rend; the mirror re-parses later for the wire) -> no
	// determinism risk, same call the serverPublish oracle/EFCT paths already make.
	s_nscreen = 0;
	TA_context* ctx = (TA_context*)ctxv;
	if (ctx) {
		ta_parse(ctx, true);
		collectScreenQuads(ctx->rend);
		attributeScreenQuads();
	}

	// Capacity guard so a long session can't fill /dev/shm.
	static const long ORACLE_CAP = 64L * 1024 * 1024;
	static FILE* of = nullptr;
	static long  ow = 0;
	static bool  full = false;

	// DIAGNOSTIC: prove the flush is called + show fire totals AND the new
	// per-frame screen-quad recovery (objs / screenQuads / attributed).
	static unsigned long s_flushCalls = 0;
	long owBefore = ow;
	int attributed = 0; for (int k=0;k<s_nscreen;k++) if (s_screen[k].obj>=0) attributed++;
	if ((s_flushCalls++ % 120) == 0)
		fprintf(stderr, "[ORACLE-HOOK] flush #%lu frame=%u objs=%d screenQuads=%d attributed=%d "
		                "decodeQuads=%d fired{OBJ_BEGIN=%lu QUAD=%lu} totalWritten=%ld\n",
		        s_flushCalls, frame, s_nobj, s_nscreen, attributed, s_nquad,
		        s_fireObjBegin, s_fireQuad, ow);

	// Emit a line for any frame that captured an object, a screen quad, or a decode quad.
	if (s_nobj == 0 && s_nquad == 0 && s_nscreen == 0) { s_nobj = 0; s_nquad = 0; s_nscreen = 0; return; }

	if (!full) {
		if (!of) {
			of = fopen("/dev/shm/mc_oracle_hook.jsonl", "a");
			if (of)
				fprintf(stderr, "[ORACLE-HOOK] first jsonl flush — frame=%u objs=%d quads=%d "
				                "-> /dev/shm/mc_oracle_hook.jsonl\n", frame, s_nobj, s_nquad);
			else
				fprintf(stderr, "[ORACLE-HOOK] FAILED to open /dev/shm/mc_oracle_hook.jsonl "
				                "(errno path) — captured objs=%d but cannot write\n", s_nobj);
		}
		if (of && ow < ORACLE_CAP) {
			char b[2048]; int n = 0;
			n  = snprintf(b, sizeof b, "{\"frame\":%u,\"objects\":[", frame);
			ow += fwrite(b, 1, n, of);
			for (int i = 0; i < s_nobj; i++) {
				const Obj& o = s_objs[i];
				n = snprintf(b, sizeof b,
					"%s{\"node\":\"0x%08X\",\"sprite_id\":%d,"
					"\"screen_xy\":[%.1f,%.1f],\"scale\":[%.3f,%.3f],"
					"\"category\":%d,\"facing\":%d,"
					"\"tex_src\":{\"gfx1_ptr\":\"0x%08X\",\"pal_ptr\":\"0x%08X\","
					"\"region\":\"%s\"},"
					"\"asm_src\":{\"extras_ptr\":\"0x%08X\",\"file_ptr\":\"0x%08X\","
					"\"fac_ptr\":\"0x%08X\"},\"screen_quads\":[",
					i ? "," : "", o.node, o.sprite_id, o.sx, o.sy,
					o.scaleX, o.scaleY, o.category, o.facing,
					o.gfx1, o.pal, classifyRegion(o.gfx1),
					o.extras, o.file, o.fac);
				ow += fwrite(b, 1, n, of);
				// PRIMARY: the per-frame SCREEN quads attributed to this object.
				// Each carries the real on-screen x,y (near screen_xy), w/h, UV
				// sub-rect, depth range, VRAM texture source + blend.
				bool firstQ = true;
				for (int k = 0; k < s_nscreen; k++) {
					const ScreenQuad& q = s_screen[k];
					if (q.obj != i) continue;
					n = snprintf(b, sizeof b,
						"%s{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
						"\"u\":[%.4f,%.4f],\"v\":[%.4f,%.4f],\"z\":[%.6g,%.6g],"
						"\"vram_addr\":\"0x%08X\",\"tcw\":\"0x%08X\",\"fmt\":%d,"
						"\"tex_wh\":[%d,%d],\"vq\":%d,\"blend\":[%d,%d]}",
						firstQ ? "" : ",",
						(int)q.x,(int)q.y,(int)q.w,(int)q.h,
						q.uMn,q.uMx,q.vMn,q.vMx, q.zMn,q.zMx,
						q.vramAddr, q.tcw, q.fmt, q.tw, q.th, q.vq,
						q.srcBlend, q.dstBlend);
					ow += fwrite(b, 1, n, of);
					firstQ = false;
				}
				n = snprintf(b, sizeof b, "],\"decode_quads\":[");
				ow += fwrite(b, 1, n, of);
				// OPTIONAL (sub-flag): the LOAD-time part-atlas decode 16-byte
				// records attributed to this object by node (mostly empty per-frame).
				bool firstD = true;
				for (int k = 0; k < s_nquad; k++) {
					const Quad& q = s_quads[k];
					if (q.obj != i) continue;
					n = snprintf(b, sizeof b,
						"%s{\"w\":%u,\"h\":%u,\"attr\":\"0x%08X\","
						"\"texptr\":\"0x%08X\",\"palptr\":\"0x%08X\"}",
						firstD ? "" : ",", q.w, q.h, q.attr, q.texptr, q.palptr);
					ow += fwrite(b, 1, n, of);
					firstD = false;
				}
				n = snprintf(b, sizeof b, "]}");
				ow += fwrite(b, 1, n, of);
			}
			// Frame-level "unassigned" bucket: screen quads with no object within
			// the attribution radius (overlap-ambiguous / owner-less global supers
			// off the OBJ_BEGIN list). Emitted so nothing is lost and the offline
			// differ can reason about coverage.
			n = snprintf(b, sizeof b, "],\"unassigned\":[");
			ow += fwrite(b, 1, n, of);
			bool firstU = true;
			for (int k = 0; k < s_nscreen; k++) {
				const ScreenQuad& q = s_screen[k];
				if (q.obj != -1) continue;
				n = snprintf(b, sizeof b,
					"%s{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
					"\"vram_addr\":\"0x%08X\",\"tcw\":\"0x%08X\",\"blend\":[%d,%d]}",
					firstU ? "" : ",", (int)q.x,(int)q.y,(int)q.w,(int)q.h,
					q.vramAddr, q.tcw, q.srcBlend, q.dstBlend);
				ow += fwrite(b, 1, n, of);
				firstU = false;
			}
			n = snprintf(b, sizeof b, "]}\n");
			ow += fwrite(b, 1, n, of);
			fflush(of);
			// Per-flush wrote-bytes (sampled with the periodic flush log above).
			if ((s_flushCalls % 120) == 1)
				fprintf(stderr, "[ORACLE-HOOK] flush wrote=%ld bytes (frame=%u objs=%d screenQuads=%d)\n",
				        ow - owBefore, frame, s_nobj, s_nscreen);
		} else if (of && ow >= ORACLE_CAP) {
			full = true;
			fprintf(stderr, "[ORACLE-HOOK] /dev/shm cap reached (%ld bytes) — stopping capture\n", ow);
		}
	}

	s_nobj = 0; s_nquad = 0; s_nscreen = 0;
}

}
