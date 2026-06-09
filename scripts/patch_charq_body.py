#!/usr/bin/env python3
# One-shot patch: fix collectScreenQuads so the character BODY's op-list textured
# part-quads are collected (CHARQ Phase-1 blocker), plus a gated MAPLECAST_QDIAG
# one-shot pre-filter poly dump. Idempotent; keyed to exact unique source strings.
import sys, re

P = "core/network/maplecast_oracle_hook.cpp"
src = open(P, "r", encoding="utf-8").read()

if "MAPLECAST_QDIAG" in src:
    print("ALREADY PATCHED — no change"); sys.exit(0)

OLD = '''	auto collect = [&](std::vector<PolyParam>& lst) {
		for (PolyParam& pp : lst) {
			if (s_nscreen >= MAX_SCREEN) return;
			if (pp.count < 3) continue;
			u32 pcw = pp.pcw.full, tcw = pp.tcw.full, tsp = pp.tsp.full;
			bool textured = ((pcw >> 3) & 1) != 0;'''

NEW = '''	// MAPLECAST_QDIAG — one-shot (one in-match frame) pre-filter dump of EVERY parsed
	// TA poly to /dev/shm/mc_qdiag.log: listType, screen Y range, tcw/tsp/pcw, textured,
	// w/h, and the computed cull flags. Answers: ARE there body-region (y~240-433)
	// textured polys in rc.global_param_*, and which flag drops them? READ-ONLY.
	static int s_qdiag = getenv("MAPLECAST_QDIAG") ? 1 : 0;
	static FILE* s_qf = nullptr;
	if (s_qdiag == 1) { s_qf = fopen("/dev/shm/mc_qdiag.log", "w"); s_qdiag = s_qf ? 2 : 0; }
	auto collect = [&](std::vector<PolyParam>& lst, int listType) {
		for (PolyParam& pp : lst) {
			if (s_nscreen >= MAX_SCREEN) return;
			if (pp.count < 3) continue;
			u32 pcw = pp.pcw.full, tcw = pp.tcw.full, tsp = pp.tsp.full;
			bool textured = ((pcw >> 3) & 1) != 0;'''

assert OLD in src, "collect-lambda head not found"
src = src.replace(OLD, NEW, 1)

# Insert the qdiag dump + FIX the predicate. Replace the classify+filter block.
OLD2 = '''			if (seen == 0) continue;
			float w = mxX-mnX, h = mxY-mnY;
			if (w < 2.f || h < 2.f) continue;
			float cy = (mnY+mxY)*0.5f; if (cy <= 20.f) continue;   // strip top HUD row
			int srcB = (int)((tsp>>29)&7), dstB = (int)((tsp>>26)&7);
			bool tiled = (uMn < -0.05f || uMx > 1.05f || vMn < -0.05f || vMx > 1.05f);
			bool opaque = (srcB == 1 && dstB == 0);
			bool oversized = (w > 200.f || h > 200.f);
			bool isSprite = textured && !tiled && !opaque && !oversized && tcw != 0;
			if (!isSprite) continue;'''

NEW2 = '''			if (seen == 0) continue;
			float w = mxX-mnX, h = mxY-mnY;
			float cy = (mnY+mxY)*0.5f;
			int srcB = (int)((tsp>>29)&7), dstB = (int)((tsp>>26)&7);
			bool tiled = (uMn < -0.05f || uMx > 1.05f || vMn < -0.05f || vMx > 1.05f);
			bool opaque = (srcB == 1 && dstB == 0);
			bool oversized = (w > 200.f || h > 200.f);
			int fmt = (int)((tcw>>27)&7);
			if (s_qf) {
				static const char* LN[3] = {"op","pt","tr"};
				fprintf(s_qf, "%s tex=%d yMin=%.1f yMax=%.1f cy=%.1f w=%.1f h=%.1f "
				              "tcw=%08X tsp=%08X pcw=%08X fmt=%d src=%d dst=%d "
				              "tiled=%d opaque=%d oversized=%d uv[%.2f,%.2f,%.2f,%.2f]\\n",
				        LN[listType], (int)textured, mnY, mxY, cy, w, h,
				        tcw, tsp, pcw, fmt, srcB, dstB, (int)tiled, (int)opaque, (int)oversized,
				        uMn, uMx, vMn, vMx);
			}
			if (w < 2.f || h < 2.f) continue;
			if (cy <= 20.f) continue;   // strip top HUD row
			// FIX (CHARQ Phase-1): the BODY renders as TEXTURED quads that flycast places
			// in the OPAQUE list (global_param_op); ta_parse FORCES SrcInstr=1/DstInstr=0 on
			// EVERY op-list poly (ta_vtx.cpp:1285-1288), so the old `!opaque` term flagged
			// every textured body part as opaque and dropped it -> bodies got 0 quads. Opaque
			// blend is therefore NOT a valid sprite-vs-clear discriminator for TEXTURED polys.
			// The real non-sprite cases (screen clears / full-page backdrops) are already
			// rejected by `oversized` (>200px) + `tiled` (UV outside ~[0,1]) + the untextured
			// gate. So: keep a TEXTURED, non-tiled, modest-size quad with a real texture even
			// when its (forced) blend is opaque. `opaque` is retained ONLY to reject UNtextured
			// solid fills (redundant with `textured`, kept for clarity). fmt 7 = reserved/bumpmap
			// -> not a sprite. This keeps HUD (pt/tr, non-opaque) + effects + bodies (op, opaque).
			bool isSprite = textured && !tiled && !oversized && tcw != 0 && fmt != 7;
			if (!isSprite) continue;'''

assert OLD2 in src, "classify/filter block not found"
src = src.replace(OLD2, NEW2, 1)

# `fmt` is now computed above; the later q.fmt assignment recomputes the same value —
# leave it (harmless) but it now shadows. Rename the local read to avoid redefinition:
OLD3 = '''		q.fmt = (int)((tcw>>27)&7); q.vq = (int)((tcw>>30)&1);'''
NEW3 = '''		q.fmt = fmt; q.vq = (int)((tcw>>30)&1);'''
assert OLD3 in src, "q.fmt assignment not found"
src = src.replace(OLD3, NEW3, 1)

# Update the three call sites to pass listType.
OLD4 = '''	collect(rc.global_param_op);
	collect(rc.global_param_pt);
	collect(rc.global_param_tr);'''
NEW4 = '''	collect(rc.global_param_op, 0);
	collect(rc.global_param_pt, 1);
	collect(rc.global_param_tr, 2);
	if (s_qf) { fclose(s_qf); s_qf = nullptr; s_qdiag = 0;
		fprintf(stderr, "[QDIAG] one-shot dump written -> /dev/shm/mc_qdiag.log "
		                "(op=%zu pt=%zu tr=%zu polys, %d kept)\\n",
		        rc.global_param_op.size(), rc.global_param_pt.size(),
		        rc.global_param_tr.size(), s_nscreen); }'''
assert OLD4 in src, "collect call sites not found"
src = src.replace(OLD4, NEW4, 1)

open(P, "w", encoding="utf-8").write(src)
print("PATCHED OK")
