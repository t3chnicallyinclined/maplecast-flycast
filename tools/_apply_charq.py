#!/usr/bin/env python3
# One-shot: apply the CHARQ-RENDER additions to a copy of maplecast_oracle_hook.cpp.
# Each (old, new) is an exact-substring replacement; old must appear exactly once.
import sys

path = sys.argv[1]
s = open(path, "r", encoding="utf-8", newline="").read()
# Normalize CRLF->LF for matching (search strings below use LF); restore on write.
had_crlf = "\r\n" in s
s = s.replace("\r\n", "\n")

EDITS = []

# 1) flag declaration + master gate
EDITS.append((
'''static bool mc_bodyCapEnabled = (getenv("MAPLECAST_BODYCAP") != nullptr);

bool mc_oracleHookEnabled = (getenv("MAPLECAST_FRAME_ORACLE_HOOK") != nullptr)
                         || mc_decodeHookEnabled
                         || mc_decodeTraceEnabled
                         || mc_quadCaptureEnabled
                         || mc_asmTraceEnabled
                         || mc_bodyCapEnabled;''',
'''static bool mc_bodyCapEnabled = (getenv("MAPLECAST_BODYCAP") != nullptr);

// CHARQ-RENDER (MAPLECAST_CHARQ_RENDER) — THE SOURCE-OF-TRUTH per-part body quad:
// the SH4 render's OWN final PVR list entries, NOT the flaky post-hoc ta_parse (which
// depends on which TA pass flycast's single-slot rqueue keeps). Captures at the point
// the per-character BODY geometry routine loc_8c0344d4 (bank03:10218) submits each
// part to the bank12 PVR-list builder loc_8C1244B0 (bank12:9795), AFTER that builder
// has fully resolved + written the final PVR poly record (PCW/ISP/TSP/TCW + the 4
// transformed verts x,y,u,v) into the display list. CAPTURE PC = 0x8C1248CC
// (bank12 `pref @r14`, right after the LAST record write and BEFORE the cursor
// advances): r14 = recordBase+0x20, record at base..base+0x3F is COMPLETE
// (base+0x08 = RESOLVED TCW = *(r12+8) | template[*(0x8C2AA508)[idx]+4]). Body-vs-HUD
// filter: Sh4cntx.pr == 0x0C03487A (the loc_8c0344d4 submit-jsr return; the other ~7
// callers of loc_8C1244B0 are HUD/effects with different pr). READ-ONLY (addrspace
// reads + /dev/shm append). Forces the master gate so rec_x64 injects + the decoder
// force-splits at 0x8C1248CC.
static bool mc_charqRenderEnabled = (getenv("MAPLECAST_CHARQ_RENDER") != nullptr);

bool mc_oracleHookEnabled = (getenv("MAPLECAST_FRAME_ORACLE_HOOK") != nullptr)
                         || mc_decodeHookEnabled
                         || mc_decodeTraceEnabled
                         || mc_quadCaptureEnabled
                         || mc_asmTraceEnabled
                         || mc_bodyCapEnabled
                         || mc_charqRenderEnabled;'''))

# 2) PC constants
EDITS.append((
'''static const u32 PC_ASM_PART   = 0x8C034864;
static const u32 PC_ASM_PART_M = PC_ASM_PART & SH4_AREA_MASK;  // 0x0C034864''',
'''static const u32 PC_ASM_PART   = 0x8C034864;
static const u32 PC_ASM_PART_M = PC_ASM_PART & SH4_AREA_MASK;  // 0x0C034864
// CHARQ-RENDER: the per-part PVR-list-record completion PC inside loc_8C1244B0
// (bank12). At 0x8C1248CC (`pref @r14`, right after the final record write and before
// the cursor advances) the 0x40-byte PVR poly record at r14-0x20 is fully resolved.
static const u32 PC_CHARQ_SUBMIT   = 0x8C1248CC;
static const u32 PC_CHARQ_SUBMIT_M = PC_CHARQ_SUBMIT & SH4_AREA_MASK;  // 0x0C1248CC
// The body render's submit-jsr return address (loc_8c0344d4: jsr @0x8C034876 ->
// pr = 0x8C03487A). This pr at PC_CHARQ_SUBMIT identifies the BODY caller (vs the ~7
// HUD/effect callers of loc_8C1244B0, which have other pr's).
static const u32 PC_BODY_SUBMIT_RET   = 0x8C03487A;
static const u32 PC_BODY_SUBMIT_RET_M = PC_BODY_SUBMIT_RET & SH4_AREA_MASK;  // 0x0C03487A
static const u32 CHARQ_REC_OFF   = 0x20;   // r14 at PC_CHARQ_SUBMIT = base + 0x20
static const u32 CHARQ_REC_BYTES = 0x40;   // poly header 0x20 + one vertex block 0x20'''))

# 3) isHookedPC
EDITS.append((
'''	if (m == PC_ASM_PART_M) return mc_asmTraceEnabled || mc_bodyCapEnabled;
	return false;
}''',
'''	if (m == PC_ASM_PART_M) return mc_asmTraceEnabled || mc_bodyCapEnabled;
	// CHARQ-RENDER: the per-part PVR-record completion PC inside loc_8C1244B0
	// (0x8C1248CC). Only hooked when the charq-render flag is set. Mid-block -> the
	// decoder force-split makes it a block start; return true so rec_x64 injects.
	if (m == PC_CHARQ_SUBMIT_M) return mc_charqRenderEnabled;
	return false;
}'''))

# 4) dispatcher
EDITS.append((
'''	if (mpc == PC_ASM_PART_M) {
		if (mc_asmTraceEnabled) mc_asmTraceHandler(r);
		if (mc_bodyCapEnabled)  mc_bodyCapHandler(r);
		return;
	}''',
'''	if (mpc == PC_ASM_PART_M) {
		if (mc_asmTraceEnabled) mc_asmTraceHandler(r);
		if (mc_bodyCapEnabled)  mc_bodyCapHandler(r);
		return;
	}

	// CHARQ-RENDER per-part PVR-record capture — loc_8C1244B0 completion PC (0x8C1248CC).
	// Filters to the BODY caller by Sh4cntx.pr inside the handler. Accumulates per
	// (cid, vframe) -> /dev/shm/mc_charq_render.jsonl.
	if (mpc == PC_CHARQ_SUBMIT_M) {
		if (mc_charqRenderEnabled) mc_charqRenderHandler(r);
		return;
	}'''))

# 5) OBJ_BEGIN segmentation signal
EDITS.append((
'''		u32 node = norm(r[4]);
		if (!inRam(node)) return;
		int oi = findOrCreateObj(node);
		if (oi >= 0) { enrichObj(s_objs[oi], node); s_objs[oi].fromBegin = true; }  // refresh post-transform screen_xy''',
'''		u32 node = norm(r[4]);
		if (!inRam(node)) return;
		// CHARQ-RENDER run segmentation: this body's parts (emitted next by loc_8c0344d4)
		// belong to THIS node. Record its identity for charqGetRun.
		if (mc_charqRenderEnabled) {
			u32 cnode = node | 0x0C000000u;
			s_charqBeginNode = cnode;
			s_charqBeginCid  = (u32)(u8)addrspace::read8(cnode + OFF_CHAR_ID);
			s_charqBeginSid  = (u32)(u16)addrspace::read16(cnode + OFF_SPRITE_ID);
		}
		int oi = findOrCreateObj(node);
		if (oi >= 0) { enrichObj(s_objs[oi], node); s_objs[oi].fromBegin = true; }  // refresh post-transform screen_xy'''))

# 6) SAT_BEGIN segmentation signal
EDITS.append((
'''		u32 node = norm(r[4]);
		if (!inRam(node)) return;
		int oi = findOrCreateObj(node);
		if (oi >= 0) {
			enrichObj(s_objs[oi], node);
			resolveOwner(s_objs[oi], node);
			s_objs[oi].isSat     = true;
			s_objs[oi].fromBegin = true;   // anchor on it like a body (own screen_xy)
		}''',
'''		u32 node = norm(r[4]);
		if (!inRam(node)) return;
		// CHARQ-RENDER run segmentation (satellite path). Satellites render via
		// loc_8c030af8 + a DIFFERENT submit jsr, so the pr filter rejects satellite
		// parts — but we still update the begin node so a body run after a satellite
		// re-opens correctly.
		if (mc_charqRenderEnabled) {
			u32 cnode = node | 0x0C000000u;
			s_charqBeginNode = cnode;
			s_charqBeginCid  = (u32)(u8)addrspace::read8(cnode + OFF_CHAR_ID);
			s_charqBeginSid  = (u32)(u16)addrspace::read16(cnode + OFF_SPRITE_ID);
		}
		int oi = findOrCreateObj(node);
		if (oi >= 0) {
			enrichObj(s_objs[oi], node);
			resolveOwner(s_objs[oi], node);
			s_objs[oi].isSat     = true;
			s_objs[oi].fromBegin = true;   // anchor on it like a body (own screen_xy)
		}'''))

# 7) the handler + statics block, inserted before BODYCAP
HANDLER = open(sys.argv[2], "r", encoding="utf-8", newline="").read().replace("\r\n", "\n")
EDITS.append((
'''// ===========================================================================
// BODYCAP (MAPLECAST_BODYCAP) — body part DECODED pixels keyed by the RENDER selector.''',
HANDLER + '''
// ===========================================================================
// BODYCAP (MAPLECAST_BODYCAP) — body part DECODED pixels keyed by the RENDER selector.'''))

for i, (old, new) in enumerate(EDITS):
    c = s.count(old)
    if c != 1:
        sys.stderr.write("EDIT %d: expected 1 match, found %d\n" % (i, c))
        sys.exit(2)
    s = s.replace(old, new)

if had_crlf:
    s = s.replace("\n", "\r\n")
open(path, "w", encoding="utf-8", newline="").write(s)
sys.stderr.write("OK: applied %d edits\n" % len(EDITS))
