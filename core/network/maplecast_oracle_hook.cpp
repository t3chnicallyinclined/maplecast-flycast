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
// DECODE-TIME PART HOOK (MAPLECAST_DECODEHOOK) — see mc_decodeHookEnabled below.
// Declared first because the master compile-time gate (mc_oracleHookEnabled) must be
// true whenever EITHER the frame oracle OR the decode-hook is requested, so the
// recompiler injects the GenCall + the decoder force-splits at our hooked PCs.
static bool mc_decodeHookEnabled = (getenv("MAPLECAST_DECODEHOOK") != nullptr);

bool mc_oracleHookEnabled = (getenv("MAPLECAST_FRAME_ORACLE_HOOK") != nullptr)
                         || mc_decodeHookEnabled;

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

// === DECODE-TIME PART HOOK (MAPLECAST_DECODEHOOK) ===========================
// THE LOAD-DECODE INSTANT. The character-load part driver loc_8c032696 (bank03:5668)
// decodes EACH gameplay part with the LZSS word decoder loc_8c03552a (bank03:12740,
// r5 = "Decompress Buffer location") into the scratch buffer 0x0CE60000, then COPIES
// the result OUT to the part's persistent DM00 slot (the two `mov.l @r6+`/`mov.l r3,@r7`
// loops at bank03:5828/5849). 0x0CE60000 is a TRANSIENT scratch reused for every part,
// so the ONLY instant a given part exists cleanly there is BETWEEN the decoder return
// and the copy-out.
//
// THE HOOK PC = the LZSS decoder's return target, just before copy-out:
//   loc_8c03276e (0x8C03276E)  mov.l @(..),r3        ; r3 = loc_8c03552a (decoder)
//   0x8C032770                 mov.l @(..),r11        ; r11 = 0x0CE60000 (dest, loc_8c032854)
//   0x8C032772                 jsr  @r3               ; DECODE part -> 0x0CE60000
//   0x8C032774                 mov  r11,r5            ; (delay slot: r5 = 0x0CE60000)
//   0x8C032776                 mov.b @r9,r7           ; <== PC_DECODE_DONE — decoder RETURNED,
//                                                     ;     copy-out NOT yet started.
// At 0x8C032776 (the jsr return address, a natural block start):
//   - 0x0CE60000 holds the FRESH single decoded part (4bpp PAL4 index data, linear).
//   - r9  = the selector-byte cursor; *r9 (u8) = this part's GFX1 +6 SELECTOR
//           (the key rip_gfx2_assembly.py --realparts matches; r9 advances +1/part @5859).
//   - r8  = the DM00 directory base (= *(0x0CE80008), set `r8=@(0x8,r4)` @5682) — its
//           entry for this part (e = r8 + sel*0x10 + 0xA0; texels@+8 are the copy-out
//           DEST) gives the part DIMS (e0: w=lo16,h=hi16) + PVR format (e4).
//   - r14 = the CURRENT character struct base (mov r12,r14 @5700, never reclobbered to
//           the hook) -> character_id @r14+0x1, live ARGB4444 palette @r14+0x164.
// So at this one PC we have: fresh pixels (0x0CE60000) + selector (*r9) + dims/fmt (DM00
// entry via r8) + char_id/palette (r14). That is the complete CLEAN part source.
static const u32 PC_DECODE_DONE  = 0x8C032776;
static const u32 PC_DECODE_DONE_M = PC_DECODE_DONE & SH4_AREA_MASK;  // 0x0C032776
// The DM00 directory entry stride + the selector->entry bias the driver applies
// (bank03:5811-5816: r7 = r8 + (sel<<4) + 0xA0).
static const u32 DM00_ENTRY_STRIDE = 0x10;
static const u32 DM00_ENTRY_BIAS   = 0xA0;
// The LZSS decompress scratch buffer (bank03:5949 loc_8c032854 = 0x0ce60000).
static const u32 DECODE_BUF        = 0x0CE60000;

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
// R2 fallback candidate (per-object current-cell part count). The per-frame quad
// emitter loc_8c033d78 (bank03:9092) reads the per-character CELL TABLE head at
// node+0x160 (loc_8c033e18=0x0160), indexes a cell-data record, and reads the
// cell's PART COUNT as the first u16 of that record (`mov.w @r13+,r3; extu.w`,
// bank03 loc_8c033dd4:9147-9148). The "current cell" anim field the spec names
// lives at node+0x154 (loc_8c0342ac=0x0154, the anim/cell-step routine). We read
// BOTH per object so the next match shows which (if either) tracks the per-frame
// translucent render count. READ-ONLY.
static const u32 OFF_CELL_TBL   = 0x160;   // ptr -> per-char cell table (emitter head)
static const u32 OFF_CUR_CELL   = 0x154;   // u32 current-cell field (anim/cell-step)
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

// ---- R2 PROBE (per-object-quad-capture spec §6 R2) -------------------------
// QUESTION: does MVC2 submit TA polys PER-OBJECT (so the running TA poly count
// STEPS UP at each per-object render-call entry → we can mark+segment), or does
// it BULK-DMA the whole frame at once (running count flat/zero through the walk,
// only the final frame total non-zero → bulk/end-of-frame → segmentation by TA
// position is impossible)?
//
// THE SIGNAL WE CAN READ MID-FRAME: flycast's Dreamcast (non-Naomi2) TA path does
// NOT materialize rc.verts/global_param_* incrementally. ta_thd_data32_i
// (core/hw/pvr/ta.cpp:527) only COPIES raw 32-byte TA records into the per-context
// raw buffer ta_tad (thd_data cursor += 32) and ticks a state machine; the parsed
// PolyParam vectors are built ONLY at end-of-frame by ta_parse_vdrc
// (core/hw/pvr/ta_vtx.cpp:1255, walks getTADataBegin()..getTADataEnd() in one shot
// at render time). So rc.global_param_*.size() is 0 mid-frame and there is NO
// incremental parsed-poly count to read at a per-object render-call entry.
//
// The ONLY mid-frame-growing TA signal is the RAW TA byte cursor
//   ta_tad.thd_data - ta_tad.thd_root   (= bytes of TA submitted so far this frame)
// declared extern in ta_ctx.h. We sample THAT at each PC_OBJ_BEGIN/PC_SAT_BEGIN.
//   - If it STEPS UP per object → per-object TA submission (R2 PASS-ish on the byte
//     cursor; the parsed count would step the same way).
//   - If it's FLAT/0 through every per-object entry and only jumps after the whole
//     walk → bulk-DMA (R2 FAIL) → no per-object TA-position marking.
// Per the spec, MVC2's emitter loc_8c033d78 writes a RAM display list during the
// walk and a SEPARATE bulk pass DMAs it to the TA FIFO afterward — so we EXPECT
// the byte cursor to be flat across the slot walk (R2 FAIL). The probe PROVES it
// live rather than asserting it statically, and ALSO records the per-object
// current-cell part count (node+0x154 / cell-table head) as the fallback candidate.
struct R2Rec {
	int   slot;          // s_objs index this frame
	u32   node;          // object node base
	u32   taBytes;       // ta_tad.thd_data - thd_root AT this object's render entry
	u16   cellParts;     // OLD candidate: first u16 @ *(*(node+0x160)) (cell index 0) — was 0
	u16   cellParts2;    // NEW (CORRECT): per-frame current-pose count =
	                     //   first u16 of GFX2[sprite_id&0x7FFF] cell record (mc_cellPartCount2)
	u16   cellIdx;       // sprite_id & 0x7FFF (the GFX2 index used)
	u32   cellRec;       // resolved cell record ptr (debug)
	u32   curCell;       // node+0x154 raw (anim/cell-step candidate, the 20-byte anim keyframe)
	float sx, sy;        // screen_xy (placement anchor)
	bool  isSat;
};
static const int MAX_R2 = 512;
static R2Rec s_r2[MAX_R2];
static int   s_nr2 = 0;

// Read the raw TA byte cursor (bytes of TA data submitted so far THIS frame). This
// is the per-context raw-buffer write head minus its root; it advances 32 bytes per
// TA record as ta_thd_data32_i ingests the FIFO. Guarded so a null/unreset context
// can't fault. Returns 0 if the buffer pointers aren't usable.
static inline u32 mc_taBytesSoFar()
{
	if (ta_tad.thd_root == nullptr || ta_tad.thd_data == nullptr) return 0;
	ptrdiff_t d = ta_tad.thd_data - ta_tad.thd_root;
	if (d < 0 || d > (ptrdiff_t)(64u * 1024 * 1024)) return 0;   // sanity clamp
	return (u32)d;
}

// Read the per-object current-cell part count candidate. cell table head =
// *(node+0x160); its first dword points at a cell-data record whose FIRST u16 is
// the part count (bank03 loc_8c033dd4:9147). We read it best-effort (any unreadable
// pointer → 0). Pure addrspace reads.
static inline u16 mc_cellPartCount(u32 node)
{
	u32 cellTbl = addrspace::read32(node + OFF_CELL_TBL);
	if (!cellTbl || !inRam(cellTbl)) return 0;
	u32 cellData = addrspace::read32(cellTbl);      // first cell-data record ptr
	if (!cellData || !inRam(cellData)) return 0;
	return (u16)addrspace::read16(cellData);        // part count = first u16
}

// --- mc_cellPartCount2: the CORRECT per-frame current-pose part count ---------
// RE FINDING (marvelous2 bank03, the per-frame sprite emitters loc_8c0344d4 @10218
// and loc_8c0348c8 @10800 — the two routines loc_8c034bea dispatches every in-match
// frame from loc_8c03093c). Both open with the IDENTICAL current-cell read:
//
//   loc_8c0344d4 / loc_8c0348c8 prologue:
//     r0  = 0x0160                       ; loc_8c0345fc / loc_8c03492a
//     r4  = *(node + 0x160)              ; Dat_GFX2  (the per-char CELL TABLE base)
//     add 0xE4,r0                        ; 0xE4 is a SIGNED imm = -0x1C
//                                        ; r0 = 0x0160 - 0x1C = 0x0144  (sprite_id offset!)
//     r0  = *(node + 0x144)              ; sprite_id (32-bit load; low 15 bits = the cell id)
//     and 0x7FFF,r0                      ; mask off the 0x8000 dispatcher-select bit
//     shll2 r0                           ; index*4
//     r11 = *(Dat_GFX2 + index*4)        ; offset (relative to Dat_GFX2) of THIS pose's cell record
//     add r4,r11                         ; r11 = Dat_GFX2 + offset = the current cell record ptr
//     mov.w @r11+,r2 ; extu.w            ; r2 = FIRST u16 of the cell record = the loop bound
//     mov.l r2,@(0x28,r15)               ; -> the OUTER loop count the emitter walks
//
// That first u16 is the number of 8-byte EXTRAS/OAM groups (r11 advances +8 per group
// at loc_8c03488e; group sub-fields @+0x2/+0x4/+0x6 match the documented 8-byte OAM
// record) that the emitter draws for THIS pose. It is the per-frame, current-pose
// count — NOT the 228-constant full-atlas decode (loc_8c033d78, indexed by an
// iterating cell counter over the whole table) and NOT the empty cell-0 the old
// mc_cellPartCount read (it deref'd *(cellTbl) = index 0, not index sprite_id&0x7FFF).
//
// DISTINCTION vs +0x154: +0x154 (current_cell_data, used by the hitbox builder
// loc_8c034174 @9692: r13=*(node+0x154); idx=@(0x12,r13); hitbox=*(node+0x16c)+idx*16)
// points at the live 20-byte ANIM keyframe (anotak duration/sprite/hitbox-group). The
// SPRITE part count lives one level out: GFX2[sprite_id&0x7FFF] -> cell record -> first
// u16. We log BOTH so the match reveals which sums to the translucent TA count.
//
// READ-ONLY. Returns 0 on any unreadable pointer. `outIdx`/`outRec` optional debug.
static inline u16 mc_cellPartCount2(u32 node, u16* outId = nullptr, u32* outRec = nullptr)
{
	u32 gfx2 = addrspace::read32(node + OFF_CELL_TBL);          // *(node+0x160) Dat_GFX2
	if (!gfx2 || !inRam(gfx2)) return 0;
	u16 sid = (u16)addrspace::read16(node + OFF_SPRITE_ID);     // *(node+0x144)
	u16 idx = sid & 0x7FFF;                                     // mask 0x8000 select bit
	if (outId) *outId = idx;
	u32 off = addrspace::read32(gfx2 + (u32)idx * 4);           // *(GFX2 + idx*4) = rel offset
	u32 rec = gfx2 + off;                                       // cell record ptr
	if (outRec) *outRec = rec;
	if (!inRam(rec)) return 0;
	return (u16)addrspace::read16(rec);                         // first u16 = EXTRAS group count
}

// Append an R2 record for the object whose render-call just entered. Called from
// the OBJ_BEGIN / SAT_BEGIN handlers. READ-ONLY.
static void mc_r2Record(int slot, u32 node, bool isSat)
{
	if (s_nr2 >= MAX_R2 || slot < 0) return;
	R2Rec& r = s_r2[s_nr2++];
	r.slot       = slot;
	r.node       = node;
	r.taBytes    = mc_taBytesSoFar();
	r.cellParts  = mc_cellPartCount(node);                  // OLD (index-0) candidate
	r.cellIdx    = 0; r.cellRec = 0;
	r.cellParts2 = mc_cellPartCount2(node, &r.cellIdx, &r.cellRec);  // NEW current-pose count
	r.curCell    = addrspace::read32(node + OFF_CUR_CELL);
	r.sx         = rdF(node + OFF_SCREEN_X);
	r.sy         = rdF(node + OFF_SCREEN_Y);
	r.isSat      = isSat;
}

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

// ===========================================================================
// DECODE-TIME PART DUMP (MAPLECAST_DECODEHOOK) — READ-ONLY.
//
// Fires at PC_DECODE_DONE (0x8C032776) per part, the instant 0x0CE60000 holds the
// freshly LZSS-decoded part (before the driver copies it out + reuses the buffer).
// Reads the fresh pixels + selector + dims/fmt + char_id/palette from the live regs
// (see PC_DECODE_DONE comment) and writes one clean PPM per (char_id,selector):
//   /dev/shm/PL%02X_gfx1_%04u.ppm     P6, magenta = transparent (index 0)
//   /dev/shm/PL%02X_part_%03u.ppm     same pixels, the --realparts contract name
//   /dev/shm/PL%02X_gfx1.manifest     "<selector> <palRow> <w> <h> <fmt> <texptr> <ppm>"
//   /dev/shm/mc_decodehook.log        per-fire trace (dumpedThisFire visibility)
// First-seen gate per (char_id,selector) so a re-decode (next load) doesn't churn.
// PVR PixelFmt (ta_structs.h): 0=ARGB1555 1=RGB565 2=ARGB4444 5=PAL4 6=PAL8. The
// LZSS output at 0x0CE60000 is LINEAR (row-major), NOT twiddled.

static const u32 DH_OFF_CHAR_ID = 0x001;   // u8 character_id (in the char struct r14)
static const u32 DH_OFF_PAL_PTR = 0x164;   // ptr Dat_Pal (live ARGB4444 palette)

static unsigned long s_fireDecode   = 0;
static bool          s_dhCleared    = false;
static bool          s_dhSeen[0x40][512] = {{false}};   // [char_id][selector] first-seen

// e4 byte1 -> PVR PixelFmt (same proven map as maplecast_gamestate.cpp partFmtFromE4).
static inline int dhFmtFromE4(u32 e4) {
	switch ((u8)((e4 >> 8) & 0xff)) {
		case 0x00: return 0;   // ARGB1555
		case 0x01: return 1;   // RGB565
		case 0x02: return 2;   // ARGB4444
		case 0x03: return 6;   // PAL8
		case 0x04: return 5;   // PAL4
		default:   return 5;   // unknown -> PAL4 (the gameplay-part norm)
	}
}

// Decode w*h texels at texPtr (LINEAR) -> PPM (P6, magenta=transparent). Mirrors
// the proven partDecodeToPPM paletted/16-bit logic; linear order only (0x0CE60000).
static void dhWritePPM(u32 texPtr, int w, int h, int fmt, u32 palBase, const char* fn)
{
	FILE* pf = fopen(fn, "wb");
	if (!pf) return;
	fprintf(pf, "P6\n%d %d\n255\n", w, h);
	bool paletted = (fmt == 5 || fmt == 6);
	for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
		u32 idx = (u32)(y * w + x);            // LINEAR (LZSS scratch is row-major)
		u8 rr = 0, gg = 0, bb = 0, aa = 0;
		if (!paletted) {
			u16 px = (u16)addrspace::read16(texPtr + idx * 2);
			if (fmt == 1) { rr = ((px>>11)&0x1f)<<3; gg = ((px>>5)&0x3f)<<2; bb = (px&0x1f)<<3; aa = 255; }
			else if (fmt == 2) { aa = ((px>>12)&0xf)*17; rr = ((px>>8)&0xf)*17; gg = ((px>>4)&0xf)*17; bb = (px&0xf)*17; }
			else { aa = (px&0x8000)?255:0; rr = ((px>>10)&0x1f)<<3; gg = ((px>>5)&0x1f)<<3; bb = (px&0x1f)<<3; }
		} else {
			u32 pidx;
			if (fmt == 5) { u8 b = (u8)addrspace::read8(texPtr + (idx >> 1)); pidx = (idx & 1) ? (b >> 4) : (b & 0xf); }
			else          { pidx = (u8)addrspace::read8(texPtr + idx); }
			if (pidx == 0 || !inRam(palBase)) { aa = 0; }
			else { u16 pe = (u16)addrspace::read16(palBase + pidx * 2);
			       aa = ((pe>>12)&0xf)*17; rr = ((pe>>8)&0xf)*17; gg = ((pe>>4)&0xf)*17; bb = (pe&0xf)*17; }
		}
		if (aa == 0) { rr = 0xff; gg = 0x00; bb = 0xff; }   // magenta = transparent
		u8 rgb[3] = {rr, gg, bb}; fwrite(rgb, 1, 3, pf);
	}
	fclose(pf);
}

// The decode-time handler. r = Sh4cntx.r[] at 0x8C032776 (READ-ONLY).
static void mc_decodeHandler(const u32* r)
{
	if (s_fireDecode++ == 0)
		fprintf(stderr, "[DECODEHOOK] first fired (pc=0x%08X) — clean LOAD-decode parts "
		                "from 0x0CE60000 -> /dev/shm/PL*_gfx1_*.ppm\n", PC_DECODE_DONE);

	// One-time stale-dump clear (as the maplecast user; /dev/shm is maplecast-owned).
	if (!s_dhCleared) {
		for (int c = 0; c < 0x40; c++) {
			char mn[96];
			snprintf(mn, sizeof mn, "/dev/shm/PL%02X_gfx1.manifest", c); remove(mn);
		}
		s_dhCleared = true;
	}

	u32 charBase = norm(r[14]) | 0x0C000000u;     // r14 = current char struct base
	u32 selPtr   = r[9];                          // r9  = selector-byte cursor
	u32 dirBase  = r[8];                          // r8  = DM00 directory base
	if (!inRam(selPtr) || !inRam(dirBase)) return;

	u8  cid    = (u8)addrspace::read8(charBase + DH_OFF_CHAR_ID);
	u32 palP   = addrspace::read32(charBase + DH_OFF_PAL_PTR);
	u16 sel    = (u16)addrspace::read8(selPtr);   // this part's +6 GFX selector (u8)
	if (cid >= 0x40 || sel >= 512) return;

	// DM00 directory entry for this part: e = dirBase + sel*0x10 + 0xA0 (bank03:5811-5816).
	u32 e   = dirBase + (u32)sel * DM00_ENTRY_STRIDE + DM00_ENTRY_BIAS;
	if (!inRam(e)) return;
	u32 e0  = addrspace::read32(e);
	u32 e4  = addrspace::read32(e + 4);
	int w   = (int)(e0 & 0xffff), h = (int)((e0 >> 16) & 0xffff);
	if (w <= 0 || h <= 0 || w > 256 || h > 256) return;   // gameplay parts 8x8..64x128

	int  fmt    = dhFmtFromE4(e4);
	// palRow: paletted parts pick a 16-color row of Dat_Pal; the per-pose row lives in
	// the assembly record, not available at decode time. Row 0 is the load-time default
	// (the char's base skin) — correct for the at-load capture; offline can re-row.
	u32  palRow = 0;
	u32  palBase = inRam(palP) ? (palP + palRow * 32) : 0;

	if (cid < 0x40 && sel < 512 && s_dhSeen[cid][sel]) return;   // first-seen gate
	if (cid < 0x40 && sel < 512) s_dhSeen[cid][sel] = true;

	char pfn[96];  snprintf(pfn, sizeof pfn, "PL%02X_part_%03u.ppm", cid, (unsigned)sel);
	char pfp[112]; snprintf(pfp, sizeof pfp, "/dev/shm/%s", pfn);
	dhWritePPM(DECODE_BUF, w, h, fmt, palBase, pfp);
	char gfn[112]; snprintf(gfn, sizeof gfn, "/dev/shm/PL%02X_gfx1_%04u.ppm", cid, (unsigned)sel);
	dhWritePPM(DECODE_BUF, w, h, fmt, palBase, gfn);

	char mn[96]; snprintf(mn, sizeof mn, "/dev/shm/PL%02X_gfx1.manifest", cid);
	FILE* mf = fopen(mn, "a");
	if (mf) {
		if (ftell(mf) == 0)
			fprintf(mf, "# selector palRow w h fmt texptr ppm  (clean LOAD-decode parts from 0x0CE60000, keyed by +6 selector)\n");
		fprintf(mf, "%u %u %d %d %d %08x %s\n", (unsigned)sel, (unsigned)palRow, w, h, fmt, DECODE_BUF, pfn);
		fclose(mf);
	}

	static unsigned long s_dhLog = 0;
	if ((s_dhLog++ % 64) == 0) {
		FILE* lg = fopen("/dev/shm/mc_decodehook.log", s_dhLog == 1 ? "w" : "a");
		if (lg) {
			fprintf(lg, "[DECODE] fire#%lu cid=%u(PL%02X) sel=%u %dx%d fmt=%d palP=%08x dir=%08x -> %s\n",
			        s_fireDecode, cid, cid, (unsigned)sel, w, h, fmt, palP, dirBase, pfn);
			fclose(lg);
		}
	}
}

void mc_oracleInit()
{
	static bool logged = false;
	if (logged) return;
	logged = true;
	if (mc_decodeHookEnabled)
		fprintf(stderr, "[DECODEHOOK] ENABLED — LOAD-decode part capture at 0x%08X "
		                "(loc_8c032696 LZSS return, pre-copy-out): fresh 0x0CE60000 part "
		                "-> /dev/shm/PL*_gfx1_*.ppm (keyed by +6 selector)\n",
		        PC_DECODE_DONE);
	// The frame-oracle line only when the frame-oracle is what was asked for (the
	// decode flag also forces mc_oracleHookEnabled, but that path stays dormant).
	if (getenv("MAPLECAST_FRAME_ORACLE_HOOK") != nullptr)
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
	// The LOAD-DECODE part hook (loc_8c032696 / 0x8C032776). Only hooked when the
	// decode-hook flag is set. 0x8C032776 is the jsr return target (a natural block
	// start), so the decoder force-split in decoder.cpp is a no-op for it (rpc==vaddr)
	// — but mc_isHookedPC must return true so rec_x64 injects the GenCall.
	if (m == PC_DECODE_DONE_M) return mc_decodeHookEnabled;
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

	// DECODE-TIME part hook (fires PRE-match at character load, independent of the
	// frame-oracle paths). Handle first + return: it shares no per-frame buffer.
	if (mpc == PC_DECODE_DONE_M) {
		if (mc_decodeHookEnabled) mc_decodeHandler(r);
		return;
	}

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
		// R2: record the running TA byte cursor + cell part count AT this per-object
		// render-call entry, in walk order. (READ-ONLY.)
		if (mc_probeEnabled) mc_r2Record(oi, node, /*isSat=*/false);
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
		// R2: same running-TA-cursor capture for satellite/effect render entries, in
		// walk order, so a projectile/cape/super shows up in the per-object sequence.
		if (mc_probeEnabled && oi >= 0) mc_r2Record(oi, node, /*isSat=*/true);
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

// ---- R2 / KEYSTONE PROBE LOG (READ-ONLY) -----------------------------------
// We already PROVED R2 = bulk-DMA (taBytes flat across the per-object walk -> the raw
// TA cursor cannot mark per-object boundaries). So the segmentation MUST come from a
// per-object COUNT. This log now drives the KEYSTONE GATE for that count:
//
//   mc_cellPartCount2(node) = first u16 of the current-pose cell record
//     = first u16 of *( GFX2[sprite_id&0x7FFF] ), GFX2 = *(node+0x160)
//   (CONFIRMED: the exact read both per-frame emitters loc_8c0344d4 / loc_8c0348c8
//    do at entry, dispatched every in-match frame by loc_8c03093c -> loc_8c034bea.)
//
//   PASS  = sum over all rendered objects of cellParts2 ≈ the frame's TRANSLUCENT TA
//           count (tr ≈ 72-90; op=573 is stage/HUD). That sum being the per-frame
//           translucent quad total proves cellParts2 IS the per-object rendered-quad
//           count -> the keystone can segment the bulk-DMA'd TA, and the SAME cell
//           record is the sprite_id->assembly part list for step C.
//   FAIL  = cellParts2 all 0 (wrong field) -> fall back to +0x154 (the 20-byte anim
//           keyframe) or a sub-offset of the cell header.
//   CHECK = non-zero but not ≈ tr -> the first u16 is the EXTRAS-group count and a
//           group expands to >1 quad; report the delta so we know the expansion factor.
//
// The taBytes column is kept (now folded into the per-object seq) only as the standing
// proof that the bulk-DMA conclusion still holds frame to frame.
static void mc_r2Log(u32 frame, int tappOp, int tappPt, int tappTr, int tspr)
{
	static unsigned long s_r2Calls = 0;
	if ((s_r2Calls++ % 60) != 0) return;

	// Header line + the per-object sequence. Now shows BOTH cell candidates per object:
	//   cp1 = OLD mc_cellPartCount (cell index 0; expected 0/wrong)
	//   cp2 = NEW mc_cellPartCount2 (current-pose count = first u16 of
	//         GFX2[sprite_id&0x7FFF] cell record) — the KEYSTONE candidate
	// plus the GFX2 index (sid&0x7FFF) and resolved cell record ptr for debugging.
	int sumCp1 = 0, sumCp2 = 0;
	char seq[1800]; int p = 0;
	for (int i = 0; i < s_nr2 && p < (int)sizeof(seq) - 96; i++) {
		const R2Rec& r = s_r2[i];
		sumCp1 += (int)r.cellParts;
		sumCp2 += (int)r.cellParts2;
		p += snprintf(seq + p, sizeof(seq) - p,
			"%so%d%s[cp2=%u idx=%u rec=0x%X cp1=%u cur=0x%X xy=%.0f,%.0f ta=%u]",
			i ? " " : "", i, r.isSat ? "S" : "",
			(unsigned)r.cellParts2, (unsigned)r.cellIdx, r.cellRec,
			(unsigned)r.cellParts, r.curCell, r.sx, r.sy, r.taBytes);
	}

	// THE KEYSTONE GATE. PASS = sum(cellParts2) ≈ translucent TA count (tappTr).
	// The per-frame translucent quads (tr ≈ 72-90 in a real match; op=573 is stage/HUD)
	// are the ~88 character/effect sprite quads. If the per-object current-pose counts
	// sum to that, the cell read gives the per-object rendered quad count -> the keystone
	// can segment the bulk-DMA'd TA, AND the same cell record IS the sid->assembly for C.
	// "≈" tolerance: within 25% or ±12 quads (the emitter's EXTRAS groups can each expand
	// to a few quads, and the sprite filter / HUD strip add slop). We report the raw delta
	// so the operator judges; the verdict is a guide.
	int dTr = sumCp2 - tappTr;
	int adTr = dTr < 0 ? -dTr : dTr;
	bool passTr = (tappTr > 0) && (adTr <= 12 || adTr * 4 <= tappTr);
	int dSpr = sumCp2 - tspr;
	int adSpr = dSpr < 0 ? -dSpr : dSpr;
	bool passSpr = (tspr > 0) && (adSpr <= 12 || adSpr * 4 <= tspr);
	const char* verdict =
		(s_nr2 < 1)        ? "NO-OBJS(not in match / no render entries)"
		: (sumCp2 == 0)    ? "FAIL(cellParts2 all 0 -> wrong field, try +0x154 cur or sub-offset)"
		: passTr           ? "PASS(sum cellParts2 ~= tr -> per-object render count CONFIRMED)"
		: passSpr          ? "PASS-spr(sum cellParts2 ~= sprite-filtered count)"
		                   : "CHECK(cellParts2 non-zero but != tr; inspect delta vs tr/sprite)";

	fprintf(stderr,
		"[ORACLE-R2] frame=%u nObj=%d sumCellParts2=%d (cp1sum=%d) "
		"vs TA{op=%d pt=%d tr=%d sprite=%d}  dTr=%+d dSpr=%+d  verdict=%s\n"
		"[ORACLE-R2]   objSeq: %s\n",
		frame, s_nr2, sumCp2, sumCp1,
		tappOp, tappPt, tappTr, tspr, dTr, dSpr, verdict, seq);
}

void mc_oracle_frameFlush(void* ctxv, u32 frame)
{
	// Run when EITHER the master hook OR the Phase-0 probe is enabled. The probe
	// only needs ta_parse + the table reads (no block-entry buffer), so it can run
	// standalone (MAPLECAST_FRAME_ORACLE_PROBE=1 with the master hook off) — useful
	// for a minimal de-risk pass with zero recompiler GenCall injection.
	// NOTE: the decode-hook (MAPLECAST_DECODEHOOK) forces mc_oracleHookEnabled true so
	// the recompiler injects/force-splits, but its capture is entirely in the block-entry
	// handler (pre-match) — it needs NOTHING from frameFlush. So the per-frame ta_parse /
	// jsonl work runs only for the REAL frame oracle or the probe.
	bool frameOracleActive = (getenv("MAPLECAST_FRAME_ORACLE_HOOK") != nullptr) || mc_probeEnabled;
	if (!frameOracleActive) { s_nobj = 0; s_nquad = 0; s_nscreen = 0; s_nr2 = 0; return; }

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
		if (mc_probeEnabled) {
			mc_phase0Probe(frame,
				(int)ctx->rend.global_param_op.size(),
				(int)ctx->rend.global_param_pt.size(),
				(int)ctx->rend.global_param_tr.size(),
				s_nscreen);
			// R2: per-object running-TA-cursor sequence captured during THIS frame's
			// draw walk (the OBJ_BEGIN/SAT_BEGIN block-entry handlers appended to
			// s_r2). Logged here so it can report the SAME frame's final parsed TA
			// poly counts. PASS = taBytes steps up per object; FAIL = flat/0 (bulk).
			mc_r2Log(frame,
				(int)ctx->rend.global_param_op.size(),
				(int)ctx->rend.global_param_pt.size(),
				(int)ctx->rend.global_param_tr.size(),
				s_nscreen);
		}
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
		s_nobj = 0; s_nquad = 0; s_nscreen = 0; s_nr2 = 0; return;
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

	s_nobj = 0; s_nquad = 0; s_nscreen = 0; s_nr2 = 0;
}

}
