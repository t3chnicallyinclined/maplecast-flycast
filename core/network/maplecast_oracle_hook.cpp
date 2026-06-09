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

// PHASE-0 PROBE (per-object-quad-capture spec §7 Phase 0 + R1/R6). READ-ONLY.
// Confirms the game's per-object quad-COUNT table is populated PER-FRAME (R1) and
// helps LOCATE the satellite/pool count table (R6). Pure instrumentation: reads the
// game's two result tables (count + display-list ptr) plus a window of the
// surrounding count region, and logs to stderr. NO hooks, NO force-splits, NO
// attribution change, NO writes. Default-ON when the master hook is enabled so the
// operator gets the probe for free; set MAPLECAST_FRAME_ORACLE_PROBE=0 to disable,
// or =1 to enable independently of the master hook.
static bool mc_probeEnabled = []{
	const char* v = getenv("MAPLECAST_FRAME_ORACLE_PROBE");
	if (v) return v[0] != '0';                       // explicit override
	return getenv("MAPLECAST_FRAME_ORACLE_HOOK") != nullptr;  // default = follow master
}();

// --- The game's per-object quad-emit result tables (CONFIRMED addresses,
// marvelous2 bank03 loc_8c033f44 finalize @9403-9409; per-object-quad-capture
// spec §1c). The per-frame display-list builder loc_8c033d78, driven 6× (one per
// fighter body, stride 0x5A4 from 0x8C268340) by loc_8c03dcba, finalizes:
//   *(u16)(QUAD_COUNT_TBL + i*2) = r11        ; quads object i emitted this frame
//   *(u32)(QUAD_PTR_TBL   + i*4) = displaylist ; ptr to object i's 16-byte-quad list
// COUNT is u16 (mov.w store @ finalize), stride 2; PTR is u32 (mov.l), stride 4.
static const u32 QUAD_COUNT_TBL = 0x8C26AA24;   // 6 × u16 body quad counts
static const u32 QUAD_PTR_TBL   = 0x8C26AA34;   // 6 × u32 display-list ptrs
// Sibling cluster — object-pool base (re-catalog/00-README.md, bank04:11748). The
// SATELLITE/pool quad counts are UNRESOLVED (spec R6). The probe hexdumps the whole
// 0x8C26AA00..0x8C26AAF0 window so the operator can SEE which adjacent slot lights
// up when a cape/projectile char is on screen.
static const u32 POOL_BASE      = 0x8C26AA54;   // [INFERRED] satellite/pool region
static const u32 PROBE_DUMP_LO  = 0x8C26AA00;   // hexdump window start
static const u32 PROBE_DUMP_HI  = 0x8C26AAF0;   // hexdump window end (exclusive top row)

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
// SATELLITE / EFFECT render path (CONFIRMED, marvelous2 bank03 loc_8c030af8 @ line 1526):
// The slot-table walk loc_8c0308c2 (Render_sprites, bank03:1200) reads the category
// byte @node+0x3 per slot (loc_8c0308e6, lines 1226-1228) and DISPATCHES:
//   category == 0  -> bsr loc_8c03093c   (Render Main Sprite — the 6 char BODIES)
//   category != 0  -> bsr loc_8c030af8   (the EFFECT/SATELLITE path — line 1236)
// loc_8c030af8 is where projectiles, capes, drones, supers (categories 1..4, the
// cmp/pl + cmp/ge 5 range gate at lines 1539-1553) render. It takes r4 = node base
// (mov r4,r14 @1531), reads the SAME cull byte @node+0x12C (loc_8c030c66, line 1532),
// reads world pos @node+0x34/0x38/0x3C, runs the world->screen transform
// (bank12.loc_8c122560 @1570), and WRITES the result to screen_x@node+0xE0
// (loc_8c030c68, line 1572-1575) and screen_y@node+0xE4 (loc_8c030c6a, line 1578-1579)
// — exactly like Render Main Sprite. The satellite record is char-struct-shaped, so
// sprite_id@+0x144, category@+0x3, owner@+0x80, xflip@+0x130 all apply (matching
// readAllDrawn in maplecast_gamestate.cpp). This is the routine the Oracle was BLIND
// to: a Sentinel-drones capture showed 4 nodes, ALL char bases, ZERO satellites,
// because drones render here, not at loc_8c03093c.
static const u32 PC_SAT_BEGIN = 0x8C030AF8;
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
static const u32 PC_SAT_BEGIN_M = PC_SAT_BEGIN & SH4_AREA_MASK;  // 0x0C030AF8
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
static const u32 OFF_OWNER_80   = 0x080;   // ptr -> owner char base (satellite pool node)
static const u32 OFF_OWNER_18   = 0x018;   // ptr -> owner char base (alt convention)
static const u32 OFF_CHAR_ID    = 0x001;   // u8 character_id (in the owner char struct)

// The 6 fighter body char-struct bases (P1C1,P2C1,P1C2,P2C2,P1C3,P2C3; base
// 0x8C268340, stride 0x5A4). Used to (a) resolve a satellite's owner_cid from its
// owner pointer and (b) tell a body node from a satellite node.
static const u32 CHAR_BASE[6] = {
	0x8C268340, 0x8C2688E4, 0x8C268E88, 0x8C26942C, 0x8C2699D0, 0x8C269F74
};

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
	// SATELLITE enrichment (objects rendered by loc_8c030af8, not loc_8c03093c).
	bool isSat;       // true => this node came through the effect/satellite path
	u32  ownerPtr;    // raw owner char-base ptr (+0x80 or +0x18); 0 = global effect
	int  ownerSlot;   // 0..5 = which of the 6 bodies owns it; -1 = none/global
	int  ownerCid;    // owner's character_id (CHAR_BASE[slot]+0x1); -1 = unknown
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

// Resolve a satellite node's owner (which fighter spawned this projectile/cape/drone).
// The slot-table record keeps the owner char-base ptr at +0x80 (primary) or +0x18
// (alt convention) — same as readAllDrawn in maplecast_gamestate.cpp. Match it
// against the 6 body bases (area-masked so a P0/P1 alias still matches) to get the
// owner SLOT, then read character_id from that body. Global effects (owner-less
// supers) leave ownerSlot=-1 / ownerCid=-1.
static void resolveOwner(Obj& o, u32 node)
{
	u32 oA = addrspace::read32(node + OFF_OWNER_18);
	u32 oB = addrspace::read32(node + OFF_OWNER_80);
	u32 oAm = norm(oA), oBm = norm(oB);
	o.ownerPtr = oB ? oB : oA;
	o.ownerSlot = -1; o.ownerCid = -1;
	for (int s = 0; s < 6; s++) {
		u32 cbm = CHAR_BASE[s] & 0x1FFFFFFF;
		if (oAm == cbm || oBm == cbm) {
			o.ownerSlot = s;
			o.ownerCid  = (int)(u8)addrspace::read8(CHAR_BASE[s] + OFF_CHAR_ID);
			break;
		}
	}
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
	o.ownerSlot = -1; o.ownerCid = -1;
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
	if (mc_probeEnabled)
		fprintf(stderr, "[ORACLE-PROBE] ENABLED (Phase 0, READ-ONLY) — body quad-count tbl "
		                "0x%08X[6×u16] + dlPtr 0x%08X[6×u32]; R6 dump 0x%08X..0x%08X. "
		                "Set MAPLECAST_FRAME_ORACLE_PROBE=0 to disable.\n",
		        QUAD_COUNT_TBL, QUAD_PTR_TBL, PROBE_DUMP_LO, PROBE_DUMP_HI);
}

bool mc_isHookedPC(u32 pc)
{
	// Compare on the area-masked PC so the cached (0x8C..) disasm label matches the
	// physical/P0 (0x0C..) address the recompiler actually compiles from (and any
	// other alias). This is the FIX for the never-fires bug: block->vaddr is 0x0C..,
	// the literals are 0x8C.., and an exact == never matched.
	u32 m = pc & SH4_AREA_MASK;
	if (m == PC_OBJ_BEGIN_M) return true;
	// The satellite/effect render path (loc_8c030af8). Same block-entry treatment as
	// OBJ_BEGIN: r4 = node, writes screen_xy to +0xE0/+0xE4. This is what makes
	// projectiles/capes/drones first-class Oracle objects. 0x8C030AF8 is a bsr target
	// (bank03:1236) so it's already a block start; the decoder force-split is a no-op
	// guarded by rpc!=vaddr, but mc_isHookedPC must return true so the GenCall injects.
	if (m == PC_SAT_BEGIN_M) return true;
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
static unsigned long s_fireSatBegin = 0;
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

	if (mpc == PC_SAT_BEGIN_M) {
		// SATELLITE / EFFECT object (loc_8c030af8). r4 = node base, exactly as
		// OBJ_BEGIN. Register it as a first-class object with its OWN anchor so the
		// post-walk TA screen-quad attribution gives each satellite its own clean
		// quads. Also resolve owner (which fighter spawned it) so the client can pick
		// the right atlas/palette bank. NOTE: loc_8c030af8 runs BEFORE it writes the
		// transform to +0xE0/+0xE4 (block entry); so the screen_xy read here is the
		// PREVIOUS frame's value. That's fine for attribution this frame (the object
		// barely moves in 16ms) and the next frame's read is exact — same one-frame
		// property the OBJ_BEGIN read has (it reads at entry too, before the write).
		if (s_fireSatBegin++ == 0)
			fprintf(stderr, "[ORACLE-HOOK] SAT_BEGIN first fired (pc=0x%08X masked 0x%08X) "
			                "node=0x%08X\n", pc, mpc, r[4]);
		u32 node = norm(r[4]);
		if (!inRam(node)) return;
		int oi = findOrCreateObj(node);
		if (oi >= 0) {
			enrichObj(s_objs[oi], node);
			resolveOwner(s_objs[oi], node);
			s_objs[oi].isSat     = true;
			s_objs[oi].fromBegin = true;   // anchor on it like a body (own screen_xy)
		}
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

// ---- PHASE-0 PROBE (READ-ONLY) ---------------------------------------------
// Confirms R1 (the per-object quad-COUNT table is populated EVERY in-match frame,
// refuting the old "fires once at frame 2568" note) and gathers data for R6 (where
// satellite/pool counts live). Called once/frame from frameFlush, in-match, AFTER
// ta_parse + collectScreenQuads so it can also report the total parsed TA sprite
// quad count for the SAME frame. Pure addrspace reads + stderr logging — no writes,
// no hooks, no force-splits. Throttled so it doesn't flood the journal.
//
//   tapp = raw PolyParam counts in the parsed TA (op/pt/tr list sizes)
//   tspr = sprite-filtered screen quads (s_nscreen) — the count we'd segment
static void mc_phase0Probe(u32 frame, int tappOp, int tappPt, int tappTr, int tspr)
{
	// Read the 6 body quad COUNTS (u16) + display-list PTRS (u32). addrspace::read*
	// takes the cached (P1, 0x8C..) guest address directly — the same alias these
	// tables are labelled with in the disasm and the same form the existing hook
	// reads CHAR_BASE[] / 0x8C289624 through, so no P0/P1 masking is needed for a
	// fixed RAM data address (the mask only matters for executable PCs).
	u16 cnt[6]; u32 ptr[6]; int sum = 0;
	for (int i = 0; i < 6; i++) {
		cnt[i] = (u16)addrspace::read16(QUAD_COUNT_TBL + i * 2);
		ptr[i] = addrspace::read32(QUAD_PTR_TBL   + i * 4);
		sum += cnt[i];
	}

	// PASS line (throttled every 60 frames). PASS for R1 = the 6 body counts are
	// NON-ZERO every in-match frame. Format so the operator can eyeball it:
	//   counts[...] sum=N  ta{op,pt,tr,sprite}  ptrs[...]
	static unsigned long s_probeCalls = 0;
	if ((s_probeCalls++ % 60) == 0) {
		fprintf(stderr,
			"[ORACLE-PROBE] frame=%u R1 bodyCounts[%u,%u,%u,%u,%u,%u] sum=%d "
			"ta{op=%d pt=%d tr=%d sprite=%d} "
			"dlPtr[0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X]\n",
			frame,
			cnt[0],cnt[1],cnt[2],cnt[3],cnt[4],cnt[5], sum,
			tappOp, tappPt, tappTr, tspr,
			ptr[0],ptr[1],ptr[2],ptr[3],ptr[4],ptr[5]);

		// R6 — find the satellite/pool count table. Hexdump the whole count region
		// 0x8C26AA00..0x8C26AAF0 as u32 words (4 per row), with the addr at row head,
		// so when the operator plays a projectile/cape char we can SEE which adjacent
		// table goes non-zero for satellites. The known POOL_BASE 0x8C26AA54 row is
		// tagged inline. NOTE: the body COUNT table at +0xAA24 reads as packed u16
		// pairs inside these u32 words (low+high halfword = two object counts).
		fprintf(stderr, "[ORACLE-PROBE] R6 dump 0x%08X..0x%08X (u32 words):\n",
			PROBE_DUMP_LO, PROBE_DUMP_HI);
		for (u32 a = PROBE_DUMP_LO; a < PROBE_DUMP_HI; a += 16) {
			u32 w0 = addrspace::read32(a + 0);
			u32 w1 = addrspace::read32(a + 4);
			u32 w2 = addrspace::read32(a + 8);
			u32 w3 = addrspace::read32(a + 12);
			const char* tag = "";
			if (a <= QUAD_COUNT_TBL && QUAD_COUNT_TBL < a + 16) tag = "  <-COUNT_TBL";
			else if (a <= QUAD_PTR_TBL && QUAD_PTR_TBL < a + 16) tag = "  <-PTR_TBL";
			else if (a <= POOL_BASE   && POOL_BASE   < a + 16) tag = "  <-POOL_BASE?";
			fprintf(stderr, "[ORACLE-PROBE]   0x%08X: %08X %08X %08X %08X%s\n",
				a, w0, w1, w2, w3, tag);
		}
	}
}

void mc_oracle_frameFlush(void* ctxv, u32 frame)
{
	// Run when EITHER the master hook OR the Phase-0 probe is enabled. The probe
	// only needs ta_parse + the table reads (no block-entry buffer), so it can run
	// standalone (MAPLECAST_FRAME_ORACLE_PROBE=1 with the master hook off) — useful
	// for a minimal de-risk pass with zero recompiler GenCall injection.
	if (!mc_oracleHookEnabled && !mc_probeEnabled) { s_nobj = 0; s_nquad = 0; s_nscreen = 0; return; }

	// IN-MATCH GATE (in_match flag @0x8C289624, same as the serverPublish oracle).
	// The per-object draw routine (loc_8c03093c) only fires during gameplay, and
	// the menu/attract screens emit ~700 textured quads/frame with no characters —
	// running ta_parse + emitting them just burns CPU and fills the /dev/shm cap
	// with unattributable noise. Skip everything (incl. the heavy ta_parse) when
	// not in a match. This keeps the instrument cheap AND focused on real frames.
	bool inMatch = addrspace::read8(0x8C289624) != 0;

	// Recover this frame's SCREEN quads from the completed TA list and attribute
	// them to the OBJ_BEGIN objects. ta_parse here is read-only w.r.t. guest state
	// (it builds ctx->rend; the mirror re-parses later for the wire) -> no
	// determinism risk, same call the serverPublish oracle/EFCT paths already make.
	s_nscreen = 0;
	TA_context* ctx = (TA_context*)ctxv;
	if (ctx && inMatch) {
		ta_parse(ctx, true);
		collectScreenQuads(ctx->rend);
		attributeScreenQuads();

		// PHASE-0 PROBE (R1 + R6) — read the game's per-object body quad-count/ptr
		// tables and dump the surrounding count region. Runs here so it can report
		// the SAME frame's parsed TA quad counts (op/pt/tr PolyParam list sizes +
		// the sprite-filtered s_nscreen). READ-ONLY; throttled inside.
		if (mc_probeEnabled)
			mc_phase0Probe(frame,
				(int)ctx->rend.global_param_op.size(),
				(int)ctx->rend.global_param_pt.size(),
				(int)ctx->rend.global_param_tr.size(),
				s_nscreen);
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
	{
		int nSat = 0; for (int i = 0; i < s_nobj; i++) if (s_objs[i].isSat) nSat++;
		fprintf(stderr, "[ORACLE-HOOK] flush #%lu frame=%u objs=%d sats=%d screenQuads=%d attributed=%d "
		                "decodeQuads=%d fired{OBJ_BEGIN=%lu SAT_BEGIN=%lu QUAD=%lu} totalWritten=%ld\n",
		        s_flushCalls, frame, s_nobj, nSat, s_nscreen, attributed, s_nquad,
		        s_fireObjBegin, s_fireSatBegin, s_fireQuad, ow);
	}

	// Only emit for in-match frames (the gate above already skipped the heavy
	// recovery off-match, so s_nscreen is 0 there) AND only when something was
	// captured. This keeps the jsonl to real gameplay frames.
	if (!inMatch || (s_nobj == 0 && s_nquad == 0 && s_nscreen == 0)) {
		s_nobj = 0; s_nquad = 0; s_nscreen = 0; return;
	}

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
					"\"kind\":\"%s\",\"owner_slot\":%d,\"owner_cid\":%d,"
					"\"owner_ptr\":\"0x%08X\","
					"\"screen_xy\":[%.1f,%.1f],\"scale\":[%.3f,%.3f],"
					"\"category\":%d,\"facing\":%d,"
					"\"tex_src\":{\"gfx1_ptr\":\"0x%08X\",\"pal_ptr\":\"0x%08X\","
					"\"region\":\"%s\"},"
					"\"asm_src\":{\"extras_ptr\":\"0x%08X\",\"file_ptr\":\"0x%08X\","
					"\"fac_ptr\":\"0x%08X\"},\"screen_quads\":[",
					i ? "," : "", o.node, o.sprite_id,
					o.isSat ? "satellite" : "body", o.ownerSlot, o.ownerCid,
					o.ownerPtr,
					o.sx, o.sy,
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
