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

// DECODE-TRACE (MAPLECAST_DECODETRACE) — the DEFINITIVE caller-attribution trace.
// Hooks the WORD LZSS texture decoder loc_8c03552a (bank03:12740, self-labeled
// ";Texture_compression") at its ENTRY and logs, per call during a match load:
//   - caller   = Sh4cntx.pr  (the jsr return address -> name via the disasm). For
//                the TAIL-CALL callers (jmp @r3, where pr is the grandcaller), the
//                src/dest pair still uniquely identifies the routine via the static
//                catalog, so we log BOTH pr and src/dest.
//   - dest     = r5  (the decoder header's documented "Decompress Buffer location")
//   - source   = r4  (the decoder header's documented "Texture Location" = compressed)
//   - a window of the source header (first 16 bytes) for dims/format inference
//   - a per-caller call counter
// READ-ONLY. The trace disambiguates portrait/UI decodes (few calls, fixed dest)
// from the per-char gameplay-atlas decode (the looping caller that walks ~1190 +6
// selectors into 0x0CE60000). Forces the master gate so the recompiler injects.
static bool mc_decodeTraceEnabled = (getenv("MAPLECAST_DECODETRACE") != nullptr);

// QUADCAPTURE (MAPLECAST_QUADCAPTURE) — THE EXPERT-CONFIRMED CLEAN-PIXEL CAPTURE.
// Hooks the jsr-RETURN PC 0x8C033ED0 in loc_8c033e90 — the instruction right AFTER the
// BYTE-LZSS decoder loc_8c0354c0 (jsr @r2 @bank03:9291) fills the part pixels. The emit
// loop stores texptr=r8 to the quad at 0x8C033EBC, but the decoder doesn't run until the
// jsr at 0x8C033ECC; it writes to r6 (= r8's value, `mov r8,r6` @9260) and r8 is NOT
// advanced until 0x8C033ED4 — so at 0x8C033ED0 r8 points at the FRESHLY DECODED part.
// (The earlier 0x8C033EC0 hook read r8 BEFORE the decode -> stripe-noise; this is the fix.)
// DECODETRACE proved loc_8c0354c0 fires ~84k×/match: 0x0CE60000 is a PER-FRAME transient
// scratch, so parts decode continuously DURING the match (not only at load) — capture
// fires live in-match as poses change. Per fire (first-seen per char_id+selector) writes:
//   /dev/shm/PL%02X_gfx1_%04u.ppm   P6, magenta = transparent (index 0)  [+6 selector]
//   /dev/shm/PL%02X_raw_%04u.bin    RAW 4bpp indices (w*h/2 bytes) — offline-palette fallback
//   /dev/shm/PL%02X_gfx1.manifest   "<selector> <palRow> <w> <h> <texptr> <node> <char_id> <palettePtr> <ppm> <raw>"
//   /dev/shm/mc_quadcapture.log     per-fire trace
// At 0x8C033ED0 (decoder saves/restores r14,r12,r11,r10,r9; never touches r8/r13):
//   node=r10, sel=read_u16(r13+6), texptr=r8 (absolute into 0x0CE60000, now decoded),
//   gfx1=read_u32(node+0x15c), blob=gfx1+read_u32(gfx1+sel*4),
//   w=read_u8(blob+2)<<3, h=read_u8(blob+3)<<3, bytes=w*h/2, palette=read_u32(node+0x164).
// First-seen per (char_id, selector). CRITICAL: the RAW 4bpp dump means a null
// node+0x164 (the earlier 0x8C032776 hook saw palP=0) is recoverable OFFLINE from
// PALETTE_DATA.BIN — we log the palettePtr value either way. READ-ONLY; forces the
// master gate AND the PC_QUAD_DEC hook so the recompiler injects + force-splits.
static bool mc_quadCaptureEnabled = (getenv("MAPLECAST_QUADCAPTURE") != nullptr);

// ASMTRACE (MAPLECAST_ASMTRACE) — THE GROUND-TRUTH SPRITE-ASSEMBLY RECIPE.
// Hooks the per-PART point inside the per-frame BODY geometry routine loc_8c0344d4
// (bank03:10218, the routine loc_8c034bea dispatches every in-match frame from
// loc_8c03093c — see mc_cellPartCount2's RE note). loc_8c0344d4 walks the current
// pose's cell record: an OUTER loop over 8-byte EXTRAS groups (cursor r11, +=8 at
// loc_8c03488e) and an INNER loop over the parts of each group (cursor r13, +=4 at
// the end of loc_8c034864). Per part it accumulates a facing-gated cumulative PEN
// (X-acc in r10: `sub r5,r10` @bank03:10479 for facing-left / `add r5,r10` @10570
// for facing-right; the per-record dx=read_u16(r11+0)@10443, dy=read_u16(r11+2)@10442),
// then composes the FINAL screen position = node screen anchor (seeded at
// loc_8c034588 from node+0xE0/+0xE4) + (acc + tile) * scale (node+0xEC/+0xF0):
//   screenX = f32 @ (r15+0x30)   (written @10677 simple path / @10719 rotated path)
//   screenY = f32 @ (r15+0x34)   (written @10683 simple path / @10737 rotated path)
// THE HOOK PC = loc_8c034864 (0x8C034864) — the convergence label where BOTH paths
// have finished writing the final screen X/Y to (r15+0x30)/(r15+0x34), JUST BEFORE the
// per-part submit jsr. It's a `bra` target (from the simple path @10682) AND a
// fall-through (from the rotated path @10737) -> mid-block -> the decoder force-split
// makes it a block start so the GenCall injects. At 0x8C034864 ALL of these are live:
//   node = r14 (mov r4,r14 @prologue, never reclobbered)
//   r11  = outer EXTRAS-group cursor: sel=read_u16(r11+6)@10353, dx=read_u16(r11+0),
//          dy=read_u16(r11+2), flags/select=read_u16(r11+4) (0x4000 facing, 0x8000 dispatch)
//   r13  = inner part cursor (4-byte stride); read_u8(r13+0)=tile/dim byte
//   r10  = live cumulative pen X-acc (sign-extended word)
//   screenX/Y = f32 @ (r15+0x30)/(r15+0x34)  (final, post-transform)
// Per fire appends ONE line to /dev/shm/mc_assembly.log:
//   frame sid slot cid sel dx dy accX accY screenX screenY pal row flip
// where sid=node+0x144, slot=which of the 6 bodies (-1 if none), cid=node+0x1,
// accX=r10, accY=stack@0x14 (the Y pen, mov.w r0,@(0x14,r15)@10466), pal=read_u16(r11+2),
// row=(pal>>4)&7, flip=(read_u16(r11+4)&0x10)?1:0. READ-ONLY (addrspace reads +
// /dev/shm append). Forces the master gate so the recompiler injects + force-splits.
static bool mc_asmTraceEnabled = (getenv("MAPLECAST_ASMTRACE") != nullptr);

// BODYCAP (MAPLECAST_BODYCAP) — capture the BODY part DECODED PIXELS keyed by the
// RENDER selector (the 533-541 namespace), NOT the load-time effect-atlas namespace.
//
// SELECTOR-SPACE RECONCILIATION (marvelous2 bank03, CONFIRMED + live-verified):
//   * loc_8c0344d4 (the per-frame BODY render, ASMTRACE PC 0x8C034864): per part the
//     GFX1 index = read_u16(r11+6). r11 is the body's per-part record cursor seeded
//     from the GLOBAL cell-output table 0x8C1F9F9C (bank03:10410 #data 0x8c1f9f9c,
//     loaded `add r2,r13`/the r11 walk) — NOT node+0x160. For PL00 (Ryu) this field
//     ranges 252-771 (live mc_assembly.log: cid=0 sel 252..771; standing pose = the
//     530-541 cluster). This is the GROUND-TRUTH GFX1 part index.
//   * loc_8c033e90 (QUADCAPTURE, r13+6 -> 0-251): the LOAD-TIME effect/UI atlas
//     builder (driver loc_8c033d78). It reads the SAME GFX1 base (node+0x15c) and the
//     SAME read_u16(<rec>+6)*4 -> *(GFX1+sel*4) formula, but its <rec> cursor walks a
//     DIFFERENT record stream (the effect-sheet assembly), so its +6 fields are a
//     different (small, 0-251) part set. SAME table mechanism, DIFFERENT record source
//     -> different selector population. The directory 0x8C26AA24/0x8C26AA34 is NOT a
//     per-selector decode-offset table (live mc_partdir.log = the 6-entry body
//     quad-count/dl-ptr tables + adjacent RAM, all 0 or garbage); it does NOT hold
//     533-541 NOR 0-251. So neither directory keys the body pixels.
//
// WHERE THE BODY (533-541) PIXELS LIVE: loc_8c0344d4 itself calls NO decoder; the
// pixels are pre-decoded once at character load into PERSISTENT slots. Two candidate
// sources, both dumped + logged so the first capture is self-diagnosing:
//   (A) GFX1 blob: gfx1=*(node+0x15c); off=read_u32(gfx1+sel*4); blob=gfx1+off;
//       dims w=read_u8(blob+2)<<3, h=read_u8(blob+3)<<3; texels follow the header.
//       The blob may carry the 4bpp texels inline (linear) OR a pointer.
//   (B) DM00 persistent directory: dirBase=*(0x0CE80008), stride 0x10; entry+0 = (w,h)
//       u16 pixels, entry+4 = fmt word, entry+8 = ptr to DECODED TWIDDLED texels
//       (the proven-clean source the offline partDump uses). Keyed by a small DM00 key,
//       not the render selector — so we resolve the render selector's DIMS from the
//       GFX1 blob and MATCH them against the DM00 run to find the right key.
// Decode = the proven partDecodeToPPM PAL4 path (flycast TWIDDLE for DM00, linear for
// the GFX1 blob), palette = node+0x164 at row (rec_pal>>4)&7. READ-ONLY (addrspace
// reads + /dev/shm writes). Forces the master gate so the recompiler injects +
// force-splits at the ASMTRACE PC (0x8C034864) — shared, no extra hook needed.
static bool mc_bodyCapEnabled = (getenv("MAPLECAST_BODYCAP") != nullptr);

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

// ===========================================================================
// GENERIC RUNTIME-CONFIGURABLE PROBE (MAPLECAST_ORACLE_PROBE) — THE REBUILD KILLER.
//
// The problem this solves: every prior hook above is a COMPILED-IN PC + handler, so
// each new "where does X get called / what's in this register" hypothesis = a new
// constant + a new handler + a full rebuild + a redeploy. This probe makes the hook
// table DATA: a config file lists up to MC_PROBE_MAX PCs and, per PC, a dumpspec of
// what to read (regs / a single reg / pr / absolute memory / register-relative memory
// / a stack window). Edit the config + restart — NO recompile. It reuses the EXACT
// existing JIT-hook infrastructure: mc_isHookedPC (so rec_x64 injects the GenCall and
// decoder.cpp force-splits mid-block PCs) + the masked-PC (& 0x1FFFFFFF) convention.
//
// v1 (this) parses the config ONCE at static-init (before any block compiles), so the
// recompiler's compile-time mc_isHookedPC gate is correct from the first block — no
// runtime block-flush needed. To RECONFIGURE you restart the process (the standard
// "edit config + restart" loop). [v2, NOT built: a SIGHUP handler could re-parse the
// config and call bm_Reset() to flush all compiled blocks so the recompiler re-runs
// mc_isHookedPC against the new PC set — enabling no-restart reconfig. Deliberately
// out of scope here to keep v1 small and determinism-trivially-safe.]
//
// READ-ONLY w.r.t. guest: the handler only reads Sh4cntx.r[] + addrspace::read* and
// appends to /dev/shm/mc_probe.log. Gated MAPLECAST_ORACLE_PROBE; default OFF -> the
// probe table is empty, mc_isHookedPC returns false for every probe PC, and prod is
// byte-stock. (Note: distinct from the older MAPLECAST_FRAME_ORACLE_PROBE Phase-0
// stderr probe above — different env var, different mechanism.)
//
// CONFIG FILE: /dev/shm/mc_oracle_probe.conf (override path via env
// MAPLECAST_ORACLE_PROBE_CONF). One probe per line:
//     <pc_hex> <label> <dumpspec>
//   - pc_hex   : the guest PC, e.g. 0x8C1248CC or 8c1248cc (matched area-masked).
//   - label    : a no-space tag echoed on every dump line.
//   - dumpspec : a COMMA-separated list of tokens (no spaces), any mix of:
//       regs                 dump r0..r15 + pr + gbr + macl + mach
//       r<N>                 a single reg, N = 0..15 decimal (e.g. r12)
//       pr                   the procedure-return reg (Sh4cntx.pr; the JSR caller)
//       gbr / macl / mach    those control regs
//       mem:<addr_hex>:<len> hexdump <len> bytes at absolute guest addr <addr_hex>
//       rmem:r<N>[+<off_hex>]:<len>
//                            hexdump <len> bytes at r[N] (+ optional hex offset).
//                            e.g. rmem:r11+0x6:2  or  rmem:r14:0x40
//       stack:<n>            dump <n> 32-bit words from r15 (the SH4 call stack) —
//                            recovers grand-callers / spilled return addresses.
//     Lines starting with '#' and blank lines are ignored. len/n are decimal unless
//     0x-prefixed; len is clamped to MC_PROBE_MEM_MAX, n to MC_PROBE_STACK_MAX.
//   EXAMPLE (the first validation use-case — read the real bank12 PVR-builder caller
//   chain so we can see body vs HUD/stage callers instead of guessing):
//       0x8C1248CC stack8 stack:8,pr,r12,r13,r14
//       0x8C034864 asmtrace regs,rmem:r11+0x6:2
//
// OUTPUT: /dev/shm/mc_probe.log (override via env MAPLECAST_ORACLE_PROBE_LOG). Append
// with per-line fflush so `tail -f` is live; truncate-and-rewind at the cap (default
// 16 MiB, env MAPLECAST_ORACLE_PROBE_CAP) so a long session can't fill /dev/shm. Each
// dump is prefixed `[PROBE pc=0x0CXXXXXX label vframe=N]` (vframe = the SH4 video-frame
// counter @0x8C3496B0) so dumps interleave readably and align to a frame.
struct ProbeTok {
	enum Kind { REGS, REG, PR, GBR, MACL, MACH, MEM, RMEM, STACK } kind;
	int   reg;     // REG / RMEM base register index (0..15)
	u32   addr;    // MEM absolute address
	u32   off;     // RMEM register offset
	u32   len;     // MEM/RMEM byte length, or STACK word count
};
struct Probe {
	u32      pcMasked;          // pc & 0x1FFFFFFF
	char     label[40];
	ProbeTok toks[12];
	int      ntok;
	unsigned long fires;        // per-probe fire counter (diagnostic)
};
static const int MC_PROBE_MAX       = 16;     // max probe PCs
static const int MC_PROBE_MEM_MAX   = 256;    // max bytes per mem/rmem token
static const int MC_PROBE_STACK_MAX = 64;     // max words per stack token
static Probe s_probes[MC_PROBE_MAX];
static int   s_nprobe = 0;

// Parse "r<N>" -> 0..15, else -1.
static int mc_probeParseReg(const char* s, int len) {
	if (len < 2 || (s[0] != 'r' && s[0] != 'R')) return -1;
	int n = 0; for (int i = 1; i < len; i++) { if (s[i] < '0' || s[i] > '9') return -1; n = n*10 + (s[i]-'0'); }
	return (n >= 0 && n <= 15) ? n : -1;
}
// Parse a numeric token (0x-prefixed hex, else decimal). Returns false on garbage.
static bool mc_probeParseNum(const char* s, int len, u32* out) {
	if (len <= 0) return false;
	u32 v = 0; int i = 0; bool hex = false;
	if (len >= 2 && s[0]=='0' && (s[1]=='x'||s[1]=='X')) { hex = true; i = 2; if (i>=len) return false; }
	for (; i < len; i++) {
		char c = s[i]; int d;
		if (c>='0'&&c<='9') d = c-'0';
		else if (hex && c>='a'&&c<='f') d = c-'a'+10;
		else if (hex && c>='A'&&c<='F') d = c-'A'+10;
		else return false;
		v = hex ? (v<<4)+d : v*10+d;
	}
	*out = v; return true;
}
// Parse one comma token of a dumpspec into a ProbeTok. Returns false to skip it.
static bool mc_probeParseTok(const char* t, int len, ProbeTok* tk) {
	auto eq = [&](const char* lit){ int n=(int)strlen(lit); return n==len && strncmp(t,lit,n)==0; };
	if (eq("regs")) { tk->kind = ProbeTok::REGS; return true; }
	if (eq("pr"))   { tk->kind = ProbeTok::PR;   return true; }
	if (eq("gbr"))  { tk->kind = ProbeTok::GBR;  return true; }
	if (eq("macl")) { tk->kind = ProbeTok::MACL; return true; }
	if (eq("mach")) { tk->kind = ProbeTok::MACH; return true; }
	// stack:<n>
	if (len > 6 && strncmp(t, "stack:", 6) == 0) {
		u32 n; if (!mc_probeParseNum(t+6, len-6, &n)) return false;
		if (n == 0) return false; if (n > (u32)MC_PROBE_STACK_MAX) n = MC_PROBE_STACK_MAX;
		tk->kind = ProbeTok::STACK; tk->len = n; return true;
	}
	// mem:<addr_hex>:<len>
	if (len > 4 && strncmp(t, "mem:", 4) == 0) {
		const char* p = t + 4; int rem = len - 4;
		const char* colon = (const char*)memchr(p, ':', rem); if (!colon) return false;
		u32 addr, l;
		if (!mc_probeParseNum(p, (int)(colon-p), &addr)) return false;
		if (!mc_probeParseNum(colon+1, (int)(t+len-(colon+1)), &l)) return false;
		if (l == 0) return false; if (l > (u32)MC_PROBE_MEM_MAX) l = MC_PROBE_MEM_MAX;
		tk->kind = ProbeTok::MEM; tk->addr = addr; tk->len = l; return true;
	}
	// rmem:r<N>[+<off_hex>]:<len>
	if (len > 5 && strncmp(t, "rmem:", 5) == 0) {
		const char* p = t + 5; int rem = len - 5;
		const char* colon = (const char*)memchr(p, ':', rem); if (!colon) return false;
		u32 l; if (!mc_probeParseNum(colon+1, (int)(t+len-(colon+1)), &l)) return false;
		if (l == 0) return false; if (l > (u32)MC_PROBE_MEM_MAX) l = MC_PROBE_MEM_MAX;
		// reg part is p..colon, optionally "r<N>+<off>"
		const char* plus = (const char*)memchr(p, '+', (int)(colon-p));
		int regLen = plus ? (int)(plus-p) : (int)(colon-p);
		int reg = mc_probeParseReg(p, regLen); if (reg < 0) return false;
		u32 off = 0;
		if (plus) { if (!mc_probeParseNum(plus+1, (int)(colon-(plus+1)), &off)) return false; }
		tk->kind = ProbeTok::RMEM; tk->reg = reg; tk->off = off; tk->len = l; return true;
	}
	// r<N>
	int reg = mc_probeParseReg(t, len);
	if (reg >= 0) { tk->kind = ProbeTok::REG; tk->reg = reg; return true; }
	return false;
}

// Parse the whole config file into s_probes[]. Run ONCE at static init (below) so the
// recompiler's mc_isHookedPC sees the probe PCs before the first block compiles. Pure
// stdio (no guest state) -> safe at static-init time. Silent (a stderr summary is
// printed once from mc_oracleInit when the operator's serverPublish loop starts).
static int s_probeParsed = 0;     // 0=not yet, 1=done (0 probes ok)
static void mc_probeParseConfig() {
	if (s_probeParsed) return;
	s_probeParsed = 1;
	s_nprobe = 0;
	if (getenv("MAPLECAST_ORACLE_PROBE") == nullptr) return;   // gated OFF -> empty table
	const char* path = getenv("MAPLECAST_ORACLE_PROBE_CONF");
	if (!path) path = "/dev/shm/mc_oracle_probe.conf";
	FILE* f = fopen(path, "r");
	if (!f) { fprintf(stderr, "[ORACLE-PROBE] config not found: %s (no probes armed)\n", path); return; }
	char line[512];
	while (fgets(line, sizeof line, f) && s_nprobe < MC_PROBE_MAX) {
		// strip trailing newline/cr
		int ln = (int)strlen(line);
		while (ln > 0 && (line[ln-1]=='\n' || line[ln-1]=='\r')) line[--ln] = 0;
		// skip leading whitespace
		char* p = line; while (*p==' '||*p=='\t') p++;
		if (*p == 0 || *p == '#') continue;
		// field 1: pc_hex
		char* sp = p; while (*sp && *sp!=' ' && *sp!='\t') sp++;
		u32 pc; if (!mc_probeParseNum(p, (int)(sp-p), &pc)) { fprintf(stderr, "[ORACLE-PROBE] bad pc: %s\n", p); continue; }
		while (*sp==' '||*sp=='\t') sp++;
		// field 2: label
		char* lp = sp; while (*sp && *sp!=' ' && *sp!='\t') sp++;
		int lblLen = (int)(sp-lp); if (lblLen <= 0) continue;
		while (*sp==' '||*sp=='\t') sp++;
		// field 3: dumpspec (comma list, no spaces)
		char* dp = sp; while (*sp && *sp!=' ' && *sp!='\t') sp++;
		int dsLen = (int)(sp-dp); if (dsLen <= 0) continue;
		Probe& pr = s_probes[s_nprobe];
		pr.pcMasked = pc & 0x1FFFFFFF;
		int cpy = lblLen < (int)sizeof(pr.label)-1 ? lblLen : (int)sizeof(pr.label)-1;
		memcpy(pr.label, lp, cpy); pr.label[cpy] = 0;
		pr.ntok = 0; pr.fires = 0;
		// split dumpspec on commas
		const char* tok = dp; const char* end = dp + dsLen;
		while (tok < end && pr.ntok < (int)(sizeof(pr.toks)/sizeof(pr.toks[0]))) {
			const char* comma = (const char*)memchr(tok, ',', (int)(end-tok));
			const char* te = comma ? comma : end;
			if (te > tok) {
				ProbeTok tk; memset(&tk, 0, sizeof tk);
				if (mc_probeParseTok(tok, (int)(te-tok), &tk)) pr.toks[pr.ntok++] = tk;
				else fprintf(stderr, "[ORACLE-PROBE] bad dumpspec token '%.*s' (pc=0x%08X) skipped\n",
				             (int)(te-tok), tok, pc);
			}
			if (!comma) break; tok = comma + 1;
		}
		if (pr.ntok > 0) {
			fprintf(stderr, "[ORACLE-PROBE] armed pc=0x%08X (masked 0x%08X) label=%s tokens=%d\n",
			        pc, pr.pcMasked, pr.label, pr.ntok);
			s_nprobe++;
		}
	}
	fclose(f);
	fprintf(stderr, "[ORACLE-PROBE] %d probe(s) armed from %s\n", s_nprobe, path);
}

// Static-init trigger: parse the config before any guest block compiles so the
// compile-time mc_isHookedPC gate includes the probe PCs from the very first block.
static bool mc_probeEnabledStatic = []{
	if (getenv("MAPLECAST_ORACLE_PROBE") == nullptr) return false;
	mc_probeParseConfig();
	return true;
}();

bool mc_oracleHookEnabled = (getenv("MAPLECAST_FRAME_ORACLE_HOOK") != nullptr)
                         || mc_decodeHookEnabled
                         || mc_decodeTraceEnabled
                         || mc_quadCaptureEnabled
                         || mc_asmTraceEnabled
                         || mc_bodyCapEnabled
                         || mc_charqRenderEnabled
                         || mc_probeEnabledStatic;   // generic probe forces the master gate

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
// PC_QUAD_DEC is the POST-DECODE clean-pixel capture point for QUADCAPTURE. The
// quad-store at 0x8C033EC0 writes texptr=r8 @+8 BEFORE the pixels at r8 are decoded:
// the emit loop (marvelous2/build/bank03.asm, loc_8c033e90 lines 9258-9301) goes
//   0x8C033EBC  mov.l r8,@(0x8,r14)   ; quad+0x8 = texptr (r8)            @9283
//   0x8C033EBE  mov.l r12,@(0xC,r14)  ; quad+0xC = palptr (r12)           @9284
//   0x8C033EC0  mov.w @r14,r3         ; <- PC_QUAD_DONE (pixels NOT decoded yet) @9285
//   ...         (compute byte count r9 = w*h/2 @9287-9290)
//   0x8C033ECC  jsr  @r2              ; r2 = loc_8c0354c0 BYTE-LZSS decoder @9291
//   0x8C033ECE  mov  r9,r5            ; delay slot: r5 = byte count        @9292
//   0x8C033ED0  mov.l @(0xC,r15),r3   ; <- PC_QUAD_DEC (jsr RETURN target) @9293
//   0x8C033ED2  add  0x10,r14         ; advance display cursor             @9294
//   0x8C033ED4  add  r9,r8            ; advance r8 PAST this part          @9295
// The decoder writes its output to r6 (set `mov r8,r6` @9260), i.e. to the address r8
// HOLDS; it advances r6 (the copy) and SAVES/RESTORES r14,r12,r11,r10,r9 (push @12662-
// 12668, pop @12719-12724) and never touches r8/r13. So at the jsr RETURN PC 0x8C033ED0:
//   - r8  = STILL the texptr (NOT advanced until 0x8C033ED4) -> the FRESHLY DECODED part.
//   - r10 = node base (restored by decoder; set `mov r4,r10` @9103, kept through loop).
//   - r13 = cell-record cursor (untouched by decoder) -> sel = read_u16(r13+6).
//   - r12 = palptr (restored by decoder).
//   - r14 = STILL this quad (advanced at 0x8C033ED2, AFTER this PC) -> w@+0,h@+2 readable.
// All five live together here -> selector + dims + decoded pixels + palette in one read.
// (The old 0x8C033EC0 capture read r8 BEFORE the decode -> stripe-noise. This is the fix.)
static const u32 PC_QUAD_DEC   = 0x8C033ED0;
// SH4 external-area mask: drops the P0/P1/P2/U0 cache/region bits so any alias of a
// RAM line compares equal. 0x8C03093C & MASK == 0x0C03093C == 0x0C03093C & MASK.
static const u32 SH4_AREA_MASK = 0x1FFFFFFF;
static const u32 PC_OBJ_BEGIN_M = PC_OBJ_BEGIN & SH4_AREA_MASK;  // 0x0C03093C
static const u32 PC_SAT_BEGIN_M = PC_SAT_BEGIN & SH4_AREA_MASK;  // 0x0C030AF8
static const u32 PC_QUAD_DONE_M = PC_QUAD_DONE & SH4_AREA_MASK;  // 0x0C033EC0
static const u32 PC_QUAD_DEC_M  = PC_QUAD_DEC  & SH4_AREA_MASK;  // 0x0C033ED0

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
// DECODE-TRACE: the word LZSS texture decoder ENTRY (loc_8c03552a, bank03:12740,
// ";Texture_compression"). Block start (it's a jsr/jmp target) so the decoder
// force-split is a no-op (rpc==vaddr) but mc_isHookedPC must return true so the
// GenCall injects. At entry: r4=source(compressed "Texture Location"),
// r5=dest("Decompress Buffer location"), Sh4cntx.pr=caller return addr.
static const u32 PC_DECODE_ENTRY  = 0x8C03552A;
static const u32 PC_DECODE_ENTRY_M = PC_DECODE_ENTRY & SH4_AREA_MASK;  // 0x0C03552A
// DECODE-TRACE (BYTE-LZSS) — the SECOND texture decoder loc_8c0354c0 (bank03), the
// one the RE expert says actually decodes the ~1190 GAMEPLAY parts (driven by the
// load-time atlas builder loc_8c033d78, bank03:9092). The prior DECODETRACE only
// hooked the WORD-LZSS loc_8c03552a and thus only caught effects — this is the
// "one unverified-on-live edge". Block start (jsr/jmp target) so the force-split is
// a no-op; mc_isHookedPC must return true so the GenCall injects. At entry the
// decoder convention is the same: r4=source(compressed), r5/r6=dest, pr=caller.
static const u32 PC_DECODE_ENTRY2  = 0x8C0354C0;
static const u32 PC_DECODE_ENTRY2_M = PC_DECODE_ENTRY2 & SH4_AREA_MASK; // 0x0C0354C0
// ASMTRACE: the per-part convergence PC inside loc_8c0344d4 (see mc_asmTraceEnabled).
// Mid-block (bra target + fall-through) so the decoder force-split makes it a block
// start; mc_isHookedPC must return true so rec_x64 injects the GenCall.
static const u32 PC_ASM_PART   = 0x8C034864;
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
static const u32 CHARQ_REC_BYTES = 0x40;   // poly header 0x20 + one vertex block 0x20
// loc_8c0344d4 body-routine stack slots (relative to the ADJUSTED r15 after the
// prologue's `add 0x84,r15`; the same frame the body uses at the hook PC).
static const u32 ASM_S_SCREENX = 0x30;   // f32 final screen X (fadd of pen*scale to anchor)
static const u32 ASM_S_SCREENY = 0x34;   // f32 final screen Y
static const u32 ASM_S_ACCY    = 0x14;   // u16 Y pen accumulator (mov.w r0,@(0x14,r15))
// The load-time atlas builder return target. After loc_8c033d78 (bank03:9092) runs,
// the per-part directory at QUAD_COUNT_TBL/QUAD_PTR_TBL is finalized — we dump it as a
// cross-check (mc_partDirDump). We don't hook this PC for a block-entry call; the dump
// runs opportunistically from the quad-capture handler (it has node+char context).
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
	u32 tcw, tsp, pcw, isp, vramAddr;   // isp = pp.isp.full (depth/culling/shade-mode word)
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

// CHARQ accessor snapshot — published at the END of frameFlush (before the per-frame
// statics reset) so the CHARQ emit block in serverPublish can read this frame's
// object identities + the kept-sprite-quad -> object map. s_screen index == the
// kept-sprite-quad ordinal (collectScreenQuads pushes one ScreenQuad per kept sprite
// quad in op->pt->tr order), so s_charqMap[ordinal] = s_screen[ordinal].obj.
static CharqObj s_charqObjs[MAX_OBJS];
static int      s_charqNobj = 0;
static int      s_charqMap[MAX_SCREEN];
static int      s_charqNmap = 0;
static void publishCharqSnapshot();   // defined after frameFlush
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

// ===========================================================================
// QUADCAPTURE (MAPLECAST_QUADCAPTURE) — THE EXPERT-CONFIRMED CLEAN-PIXEL CAPTURE.
// Fires at PC_QUAD_DEC (0x8C033ED0, the jsr-RETURN target right AFTER the BYTE-LZSS
// decoder loc_8c0354c0 fills the part pixels), once per emitted part. The decoder
// writes its output to r6 (= r8's value, set `mov r8,r6` @bank03:9260) into the
// per-frame scratch 0x0CE60000, then returns; r8 is NOT advanced until 0x8C033ED4,
// so at 0x8C033ED0 r8 still points at the FRESHLY DECODED part. Reads the 4bpp PAL4
// pixels at r8=texptr and writes a clean PPM + a RAW 4bpp dump + a manifest line,
// keyed by the +6 selector. (The old 0x8C033EC0 read r8 BEFORE the decode -> stripes.)
//
// Per the RE expert (marvelous2 bank03; CONFIRMED via bank03 9258-9301 + 12661-12724):
//   node    = r10                                  (object/char struct base)
//   sel     = read_u16(r13+6)                      (the +6 GFX selector — the part key)
//   texptr  = r8                                   (absolute addr into 0x0CE60000)
//   gfx1    = read_u32(node+0x15c)                 (GFX_DATA_00 base)
//   blob    = gfx1 + read_u32(gfx1 + sel*4)        (this part's GFX1 blob)
//   w       = read_u8(blob+2) << 3                 (8-px granular width)
//   h       = read_u8(blob+3) << 3                 (8-px granular height)
//   bytes   = w*h/2                                (4bpp)
//   palette = read_u32(node+0x164)                 (ARGB4444, 16-color PAL4 — may be null)
// First-seen per (char_id, selector). READ-ONLY (addrspace reads + /dev/shm writes).
static const u32 QC_OFF_CHAR_ID = 0x001;   // u8 character_id (in the char struct r10)
static const u32 QC_R13_SEL_OFF = 0x006;   // u16 +6 GFX selector, relative to r13

static unsigned long s_fireQuadCap = 0;
static bool          s_qcCleared   = false;
static bool          s_qcSeen[0x40][512] = {{false}};   // [char_id][selector] first-seen
static bool          s_qcDirDumped = false;             // one-shot directory cross-check

// Dump the part directory cross-check (mc_partdir.log). The RE expert says the per-part
// directory is parallel arrays: QUAD_COUNT_TBL (0x8C26AA24) = u16 selector per part,
// QUAD_PTR_TBL (0x8C26AA34) = u32 cumulative byte-offset into 0x0CE60000 per part. The
// SAME addresses are documented in the Phase-0 probe as the 6-entry body quad-count /
// dl-ptr tables — a conflict. So we dump BOTH interpretations: (a) a wide hexdump of the
// 0x8C26AA00..0x8C26AB00 region as u16 and u32, and (b) the prompt's parallel-array read
// for the first 1536 parts, so the operator can SEE live which layout holds and confirm
// the captured selectors match the directory. READ-ONLY. One-shot per match load.
static void mc_partDirDump()
{
	if (s_qcDirDumped) return;
	s_qcDirDumped = true;
	FILE* f = fopen("/dev/shm/mc_partdir.log", "w");
	if (!f) return;
	fprintf(f, "# QUADCAPTURE part-directory cross-check (one-shot at load).\n"
	           "# Expert layout: SEL[i]=u16 @ 0x%08X+i*2 ; OFF[i]=u32 @ 0x%08X+i*4 into 0x0CE60000.\n"
	           "# (NOTE: same addrs are documented elsewhere as 6-entry body quad-count/dl-ptr tables.)\n",
	        QUAD_COUNT_TBL, QUAD_PTR_TBL);

	// (a) Wide region hexdump so the real layout is unambiguous live.
	fprintf(f, "\n[REGION] 0x8C26AA00..0x8C26AB00 (u32 words, 4/row):\n");
	for (u32 a = 0x8C26AA00; a < 0x8C26AB00; a += 16) {
		fprintf(f, "  0x%08X: %08X %08X %08X %08X\n",
		        a,
		        addrspace::read32(a), addrspace::read32(a + 4),
		        addrspace::read32(a + 8), addrspace::read32(a + 12));
	}

	// (b) Parallel-array read per the expert layout. Walk until a long run of
	// zero-selector entries (likely the table end). Cap at 1536 parts.
	fprintf(f, "\n[PARTDIR] i selector(u16@+0xAA24) byteOffset(u32@+0xAA34)\n");
	int zeros = 0, shown = 0;
	for (int i = 0; i < 1536; i++) {
		u16 sel = (u16)addrspace::read16(QUAD_COUNT_TBL + (u32)i * 2);
		u32 off = addrspace::read32(QUAD_PTR_TBL + (u32)i * 4);
		if (sel == 0 && off == 0) { if (++zeros > 32) break; continue; }
		zeros = 0; shown++;
		fprintf(f, "  %4d sel=%-5u off=0x%08X (texptr=0x%08X)\n",
		        i, (unsigned)sel, off, DECODE_BUF + off);
	}
	fprintf(f, "[PARTDIR] non-empty entries shown=%d\n", shown);
	fclose(f);
}

// Write w*h 4bpp texels at texPtr (LINEAR; the LZSS scratch 0x0CE60000 is row-major)
// -> PPM (P6, magenta=transparent). PAL4: 2 indices/byte, index 0 = transparent. If
// palBase is unusable, every texel reads as transparent (magenta) — the RAW .bin still
// carries the indices for offline palette application.
static void qcWritePPM(u32 texPtr, int w, int h, u32 palBase, const char* fn)
{
	FILE* pf = fopen(fn, "wb");
	if (!pf) return;
	fprintf(pf, "P6\n%d %d\n255\n", w, h);
	bool palOk = inRam(palBase);
	for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
		u32 idx = (u32)(y * w + x);                       // LINEAR (row-major scratch)
		u8  b   = (u8)addrspace::read8(texPtr + (idx >> 1));
		u32 pidx = (idx & 1) ? (b >> 4) : (b & 0xf);      // PAL4: low nibble first
		u8 rr = 0xff, gg = 0x00, bb = 0xff;               // magenta = transparent default
		if (pidx != 0 && palOk) {
			u16 pe = (u16)addrspace::read16(palBase + pidx * 2);   // ARGB4444 LE
			u8 aa = ((pe >> 12) & 0xf) * 17;
			if (aa != 0) { rr = ((pe>>8)&0xf)*17; gg = ((pe>>4)&0xf)*17; bb = (pe&0xf)*17; }
		}
		u8 rgb[3] = {rr, gg, bb}; fwrite(rgb, 1, 3, pf);
	}
	fclose(pf);
}

// Write the RAW 4bpp index bytes (w*h/2) straight from the scratch — NO decode, NO
// palette. This is the offline-fallback when node+0x164 is null/unset at this PC.
static void qcWriteRaw(u32 texPtr, int nbytes, const char* fn)
{
	FILE* rf = fopen(fn, "wb");
	if (!rf) return;
	for (int i = 0; i < nbytes; i++) fputc((u8)addrspace::read8(texPtr + (u32)i), rf);
	fclose(rf);
}

// The quad-capture handler. r = Sh4cntx.r[] at 0x8C033ED0 (post-decode). READ-ONLY.
static void mc_quadCaptureHandler(const u32* r)
{
	if (s_fireQuadCap++ == 0)
		fprintf(stderr, "[QUADCAPTURE] first fired (pc=0x%08X post-decode jsr-return) — clean "
		                "decoded parts from 0x0CE60000 -> /dev/shm/PL*_gfx1_*.ppm (+raw,+manifest)\n",
		        PC_QUAD_DEC);

	// One-time stale-dump clear (as the maplecast user; /dev/shm is maplecast-owned).
	if (!s_qcCleared) {
		for (int c = 0; c < 0x40; c++) {
			char mn[96];
			snprintf(mn, sizeof mn, "/dev/shm/PL%02X_gfx1.manifest", c); remove(mn);
		}
		remove("/dev/shm/mc_quadcapture.log");
		s_qcCleared = true;
	}

	u32 node   = norm(r[10]) | 0x0C000000u;       // r10 = object/char struct base
	u32 r13    = r[13];                           // r13 = 16-byte cell-record cursor (+6 = selector)
	u32 texptr = r[8];                            // r8  = absolute texel ptr into 0x0CE60000
	if (!inRam(node) || !inRam(r13) || !inRam(texptr)) return;

	u16 sel  = (u16)addrspace::read16(r13 + QC_R13_SEL_OFF);   // +6 GFX selector (the key)
	if (sel == 0x00FF || sel >= 512) return;                   // assembly terminator / out of range
	u8  cid  = (u8)addrspace::read8(node + QC_OFF_CHAR_ID);
	if (cid >= 0x40) return;

	// Dims from the GFX1 blob header (expert: w=blob+2<<3, h=blob+3<<3, 8-px granular).
	u32 gfx1 = addrspace::read32(node + OFF_GFX1);             // node+0x15c
	if (!inRam(gfx1)) return;
	u32 blob = gfx1 + addrspace::read32(gfx1 + (u32)sel * 4);
	if (!inRam(blob)) return;
	int w = ((int)(u8)addrspace::read8(blob + 2)) << 3;
	int h = ((int)(u8)addrspace::read8(blob + 3)) << 3;
	if (w <= 0 || h <= 0 || w > 256 || h > 256) return;        // gameplay parts 8x8..64x128

	u32 palP = addrspace::read32(node + OFF_PAL_PTR);          // node+0x164 (may be null)
	int bytes = (w * h) / 2;                                   // 4bpp

	// First-seen gate per (char_id, selector).
	if (s_qcSeen[cid][sel]) return;
	s_qcSeen[cid][sel] = true;

	// Clean PPM (palette applied if available) — keyed by the +6 selector.
	char gfn[112]; snprintf(gfn, sizeof gfn, "/dev/shm/PL%02X_gfx1_%04u.ppm", cid, (unsigned)sel);
	qcWritePPM(texptr, w, h, palP, gfn);
	// --realparts contract alias (PLxx_part_NNN.ppm).
	char pfn[96];  snprintf(pfn, sizeof pfn, "PL%02X_part_%03u.ppm", cid, (unsigned)sel);
	char pfp[112]; snprintf(pfp, sizeof pfp, "/dev/shm/%s", pfn);
	qcWritePPM(texptr, w, h, palP, pfp);
	// RAW 4bpp indices — offline-palette fallback if palP is null/unset.
	char rfn[96];  snprintf(rfn, sizeof rfn, "PL%02X_raw_%04u.bin", cid, (unsigned)sel);
	char rfp[112]; snprintf(rfp, sizeof rfp, "/dev/shm/%s", rfn);
	qcWriteRaw(texptr, bytes, rfp);

	// Manifest line (keyed by selector). palRow 0 = the load-time default skin row.
	char mn[96]; snprintf(mn, sizeof mn, "/dev/shm/PL%02X_gfx1.manifest", cid);
	FILE* mf = fopen(mn, "a");
	if (mf) {
		if (ftell(mf) == 0)
			fprintf(mf, "# selector palRow w h texptr node char_id palettePtr ppm raw  "
			            "(clean LOAD-decode parts from 0x0CE60000 quad-store, keyed by +6 selector)\n");
		fprintf(mf, "%u 0 %d %d %08x %08x %u %08x %s %s\n",
		        (unsigned)sel, w, h, texptr, node, cid, palP, pfn, rfn);
		fclose(mf);
	}

	// Per-fire trace (first 8 verbatim, then every 64th) — and the palettePtr ALWAYS so
	// we know whether node+0x164 was live at this PC.
	if (s_fireQuadCap <= 8 || (s_fireQuadCap % 64) == 0) {
		FILE* lg = fopen("/dev/shm/mc_quadcapture.log", s_fireQuadCap == 1 ? "w" : "a");
		if (lg) {
			fprintf(lg, "[QC] fire#%lu cid=%u(PL%02X) sel=%u %dx%d bytes=%d texptr=%08x "
			            "node=%08x gfx1=%08x blob=%08x palettePtr=%08x%s -> %s (+%s)\n",
			        s_fireQuadCap, cid, cid, (unsigned)sel, w, h, bytes, texptr,
			        node, gfx1, blob, palP, inRam(palP) ? "" : " [PAL-NULL: use offline]",
			        pfn, rfn);
			fclose(lg);
		}
	}

	// Cross-check: dump the part directory once (after enough parts to be populated).
	if (s_fireQuadCap == 64) mc_partDirDump();
}

// ===========================================================================
// DECODE-TRACE (MAPLECAST_DECODETRACE) — READ-ONLY caller attribution at the
// LZSS texture decoder ENTRY (loc_8c03552a). Logs every call during a match load
// so the operator can SEE, unambiguously, which caller routine decodes the
// gameplay part-atlas vs the portrait/UI decodes.
//
// STATIC CALLER CATALOG (from marvelous2 bank03; the 10 sites that load
// #data loc_8c03552a and jsr/jmp through it). The trace names the caller by the
// nearest catalog routine to Sh4cntx.pr, AND by the (src,dest) literal pair (the
// reliable id for the tail-call sites where pr is the grandcaller). The names
// below are the routine prologues that own each decode call.
struct DtCaller { u32 pcLo, pcHi; const char* name; };
static const DtCaller DT_CALLERS[] = {
	// pcLo..pcHi = the routine body range; name = role inferred from src/dest literals.
	{ 0x0C0320CE, 0x0C032183, "loc_8c0320ce dec:0x0cd00000<-0x0cc00000 (UI/HUD blob)" },
	{ 0x0C032184, 0x0C0321DB, "loc_8c032184 dec:0x0ce2ea00<-0x0ce60000 (small/portrait)" },
	{ 0x0C0321DC, 0x0C03223D, "loc_8c0321dc dec:0x0cc00000<-0x0ce60000 (small/portrait)" },
	{ 0x0C03223E, 0x0C032363, "loc_8c03223e dec (table-driven blob)" },
	{ 0x0C032364, 0x0C03246B, "loc_8c032364 dec:0x0cc00000 (datfile A blob)" },
	{ 0x0C03246C, 0x0C032525, "loc_8c03246c dec:0x0cd85000<-0x0cc00000 (big blob 0x1f000)" },
	{ 0x0C032526, 0x0C032695, "loc_8c032526 dec:0x0cc00000 (datfile A blob)" },
	{ 0x0C032696, 0x0C03281B, "loc_8c032696 dec->0x0CE60000 LOOP (GAMEPLAY PART ATLAS, +6 sel walk)" },
	{ 0x0C03281C, 0x0C0328ED, "loc_8c03281c dec:0x0ce60000 (single big char blob)" },
	{ 0x0C0328EE, 0x0C0399FF, "loc_8c0328ee dec:0x0cc60000 (effects/shared blob)" },
	{ 0x0C03990C, 0x0C039F99, "loc_8c03990c dec:0x0c520000/0x0c720000 (stage/bg blob)" },
	{ 0x0C039F9A, 0x0C03A003, "loc_8c039f9a dec:0x0ce60000 (datfile-driven blob)" },
};
static const char* dtNameForPr(u32 pr)
{
	u32 m = pr & SH4_AREA_MASK;
	for (const DtCaller& c : DT_CALLERS)
		if (m >= c.pcLo && m <= c.pcHi) return c.name;
	return "(pr outside catalog — TAIL-CALL grandcaller; id by src/dest)";
}

static unsigned long s_fireTrace = 0;
static bool          s_dtCleared = false;
// Per-(caller-route) aggregate so the summary at the tail is compact: index by the
// catalog slot; track count + last src/dest/dims.
static const int DT_NCAT = (int)(sizeof(DT_CALLERS)/sizeof(DT_CALLERS[0]));
static struct { unsigned long n; u32 src, dst; u16 w, h; u32 prLast; } s_dtAgg[DT_NCAT + 1] = {};

static int dtCatIndex(u32 pr)
{
	u32 m = pr & SH4_AREA_MASK;
	for (int i = 0; i < DT_NCAT; i++)
		if (m >= DT_CALLERS[i].pcLo && m <= DT_CALLERS[i].pcHi) return i;
	return DT_NCAT;   // "outside catalog" bucket
}

// Per-decoder fire counts (word-LZSS loc_8c03552a vs byte-LZSS loc_8c0354c0). The
// byte-LZSS is the one the RE expert flags as the GAMEPLAY-part decoder expected to
// fire ~1190× from caller loc_8c033d78 — kept SEPARATE so the operator can confirm.
static unsigned long s_fireTraceWord = 0;
static unsigned long s_fireTraceByte = 0;

// The decode-trace handler. r = Sh4cntx.r[] at the decoder ENTRY (READ-ONLY).
//   which 0 = WORD-LZSS loc_8c03552a (r4=src, r5=dest)
//   which 1 = BYTE-LZSS loc_8c0354c0 (r4=src, dest in r5 or r6 — both logged)
// Both: Sh4cntx.pr = caller (jsr return) / grandcaller (jmp tail-call).
static void mc_decodeTraceHandler(const u32* r, int which)
{
	u32 pr  = Sh4cntx.pr;        // caller return address (jsr) / grandcaller (jmp tail-call)
	u32 src = r[4];
	u32 dst = r[5];
	u32 dst6 = r[6];             // byte-LZSS may pass dest in r6
	const char* dec = which ? "BYTE-LZSS(loc_8c0354c0)" : "WORD-LZSS(loc_8c03552a)";

	s_fireTrace++;
	if (which) s_fireTraceByte++; else s_fireTraceWord++;
	if (s_fireTrace == 1)
		fprintf(stderr, "[DECODETRACE] first fired (%s) — per-call caller/src/dest "
		                "-> /dev/shm/mc_decodetrace.log\n", dec);

	if (!s_dtCleared) { remove("/dev/shm/mc_decodetrace.log"); s_dtCleared = true; }

	// Source header window: the decoder reads 16-bit words from r4; the first words
	// are the per-part header. We log the first 8 bytes raw so dims/format can be
	// inferred offline / correlated with the DM00 entry. Best-effort (guarded reads).
	u32 h0 = inRam(src) ? addrspace::read32(src)     : 0;
	u32 h1 = inRam(src) ? addrspace::read32(src + 4) : 0;
	// The decoder's documented r5=dest. For the gameplay loop caller (loc_8c032696)
	// dest is the 0x0CE60000 scratch and the dims live in the DM00 entry, not the
	// source header — so we don't try to over-interpret w/h here; offline correlates.
	u16 w = (u16)(h0 & 0xffff), hgt = (u16)((h0 >> 16) & 0xffff);

	int ci = dtCatIndex(pr);
	s_dtAgg[ci].n++;
	s_dtAgg[ci].src = src; s_dtAgg[ci].dst = dst;
	s_dtAgg[ci].w = w; s_dtAgg[ci].h = hgt; s_dtAgg[ci].prLast = pr;

	// Per-call line. Throttle the verbose per-call log so a 1000+-part atlas walk
	// doesn't flood /dev/shm, but ALWAYS log the first 8 calls of each caller bucket
	// (so the first portrait + first gameplay-part are both captured verbatim), then
	// every 64th. The aggregate summary (written every call, overwriting) carries the
	// full per-caller totals regardless.
	static unsigned long s_perCat[DT_NCAT + 1] = {};
	unsigned long cn = ++s_perCat[ci];
	bool verbose = (cn <= 8) || (cn % 64 == 0);

	FILE* lg = fopen("/dev/shm/mc_decodetrace.log", "a");
	if (lg) {
		if (s_fireTrace == 1)
			fprintf(lg, "# call# decoder pr caller=<route> src dst[r5] dst[r6] srcHdr[h0 h1] (w h from h0)\n");
		if (verbose)
			fprintf(lg, "[%lu] %s pr=0x%08X dst5=0x%08X dst6=0x%08X src=0x%08X hdr=%08X %08X (w=%u h=%u) | %s\n",
			        s_fireTrace, dec, pr, dst, dst6, src, h0, h1, (unsigned)w, (unsigned)hgt, dtNameForPr(pr));
		fclose(lg);
	}

	// Rewrite the compact per-caller SUMMARY every 32 calls (and on call 1) so the
	// operator can read totals at a glance without scanning the whole log. Leads with
	// the per-decoder totals (the headline: byte-LZSS ~1190× = the gameplay atlas).
	if (s_fireTrace == 1 || (s_fireTrace % 32) == 0) {
		FILE* sf = fopen("/dev/shm/mc_decodetrace.summary", "w");
		if (sf) {
			fprintf(sf, "# DECODETRACE summary — total fires=%lu  [WORD-LZSS loc_8c03552a=%lu, "
			            "BYTE-LZSS loc_8c0354c0=%lu]\n"
			            "# (expect BYTE-LZSS ~1190 from caller loc_8c033d78 at match load = the gameplay atlas)\n",
			        s_fireTrace, s_fireTraceWord, s_fireTraceByte);
			for (int i = 0; i <= DT_NCAT; i++) {
				if (s_dtAgg[i].n == 0) continue;
				const char* nm = (i < DT_NCAT) ? DT_CALLERS[i].name : "(outside catalog / tail-call)";
				fprintf(sf, "calls=%-6lu lastSrc=0x%08X lastDst=0x%08X lastPr=0x%08X | %s\n",
				        s_dtAgg[i].n, s_dtAgg[i].src, s_dtAgg[i].dst, s_dtAgg[i].prLast, nm);
			}
			fclose(sf);
		}
	}
}

// ===========================================================================
// ASMTRACE (MAPLECAST_ASMTRACE) — GROUND-TRUTH per-part sprite-assembly recipe.
// Fires at PC_ASM_PART (0x8C034864) once per emitted body part. See mc_asmTraceEnabled
// for the full register/stack map. READ-ONLY: addrspace reads + a /dev/shm append.
static unsigned long s_fireAsm   = 0;
static bool          s_asmCleared = false;

static void mc_asmTraceHandler(const u32* r)
{
	// Global MVC2 frame counter (CLAUDE.md / work.asm: 0x8C3496B0, u32).
	u32 frame = addrspace::read32(0x8C3496B0);
	if (s_fireAsm++ == 0)
		fprintf(stderr, "[ASMTRACE] first fired (pc=0x%08X loc_8c0344d4 per-part) — "
		                "ground-truth body assembly -> /dev/shm/mc_assembly.log\n", PC_ASM_PART);

	// One-time stale-log clear (truncate). /dev/shm is maplecast-owned.
	if (!s_asmCleared) { remove("/dev/shm/mc_assembly.log"); s_asmCleared = true; }

	u32 node = norm(r[14]) | 0x0C000000u;   // r14 = char/render struct base
	u32 r11  = r[11];                       // outer EXTRAS-group cursor
	u32 r13  = r[13];                       // inner part cursor
	u32 sp   = r[15];                       // SH4 stack pointer (the body frame)
	if (!inRam(node) || !inRam(r11) || !inRam(sp)) return;

	// Record fields off the outer cursor r11 (the 8-byte EXTRAS group).
	int  dx    = (s16)addrspace::read16(r11 + 0x0);   // X component (->r5 @bank03:10443)
	int  dy    = (s16)addrspace::read16(r11 + 0x2);   // Y component (->stack@0x8 @10442)
	u16  flags = (u16)addrspace::read16(r11 + 0x4);   // 0x4000 facing-select, 0x8000 dispatch
	u16  sel   = (u16)addrspace::read16(r11 + 0x6);   // +6 GFX selector (the part key)
	if (sel == 0x00FF) return;                        // assembly terminator

	// Pen accumulators + final screen position from the live regs / body stack frame.
	int   accX    = (s16)(r[10] & 0xffff);                  // r10 = X pen acc (word)
	int   accY    = (s16)addrspace::read16(sp + ASM_S_ACCY);// Y pen acc (stack@0x14)
	float screenX = rdF(sp + ASM_S_SCREENX);                // final screen X (stack@0x30)
	float screenY = rdF(sp + ASM_S_SCREENY);                // final screen Y (stack@0x34)

	// Identity: sprite_id @ node+0x144, character_id @ node+0x1, owner slot 0..5.
	u16 sid = (u16)addrspace::read16(node + OFF_SPRITE_ID);
	u8  cid = (u8)addrspace::read8(node + OFF_CHAR_ID);
	int slot = -1;
	for (int s = 0; s < 6; s++) if ((node & 0x1FFFFFFF) == (CHAR_BASE[s] & 0x1FFFFFFF)) { slot = s; break; }

	// pal/row/flip as the spec requests: pal = the +2 record word (the prompt's "pal field"),
	// row = the 3-bit palette row (pal>>4)&7 (PalMod-confirmed), flip = (flags & 0x10)?1:0.
	u16 pal  = (u16)dy & 0xffff;
	int row  = (pal >> 4) & 0x7;
	int flip = (flags & 0x10) ? 1 : 0;

	FILE* lg = fopen("/dev/shm/mc_assembly.log", "a");
	if (!lg) return;
	if (ftell(lg) == 0)
		fprintf(lg, "# frame sid slot cid sel dx dy accX accY screenX screenY pal row flip "
		            "flags r11 r13 node  (loc_8c0344d4 @0x%08X, per body part)\n", PC_ASM_PART);
	fprintf(lg, "%u %u %d %u %u %d %d %d %d %.2f %.2f %u %d %d %04x %08x %08x %08x\n",
	        frame, (unsigned)sid, slot, (unsigned)cid, (unsigned)sel,
	        dx, dy, accX, accY, screenX, screenY, (unsigned)pal, row, flip,
	        (unsigned)flags, r11, r13, node);
	fclose(lg);
}

// ===========================================================================
// CHARQ-RENDER (MAPLECAST_CHARQ_RENDER) — THE SOURCE-OF-TRUTH per-part body quad.
// Fires at PC_CHARQ_SUBMIT (0x8C1248CC) inside loc_8C1244B0 once per emitted body
// part, AFTER the PVR poly record is fully written. We filter to the BODY caller via
// Sh4cntx.pr == 0x0C03487A (the loc_8c0344d4 submit jsr return), then read the
// 0x40-byte PVR record from r14-0x20 and accumulate it under (cid, vframe). When the
// video frame (0x8C3496B0) advances we flush the accumulated per-character quad lists
// to /dev/shm/mc_charq_render.jsonl. READ-ONLY (addrspace reads + /dev/shm).
//
// We do NOT have the caller's node base in a register at this PC, but the BODY render
// only runs for the (up to 6) active fighter bodies, and the record's TCW + verts are
// what the consumer needs. We resolve the owning character by reading sprite_id/cid
// off the currently-rendering node, which loc_8c0344d4 stashes nowhere reachable here
// — so we attribute by render ORDER within the video frame: each contiguous run of
// body parts (between video-frame boundaries) belongs to the body whose loc_8c0344d4
// pass is executing. To give a STABLE cid we read the active fighter bodies' sprite_id
// and match the record's resolved TCW VRAM region; simplest+robust: tag every part of
// the current run with the nearest preceding ASMTRACE node identity if available, else
// emit under a synthetic per-run object. Here we keep it minimal + deterministic: tag
// by the SH4 r13 record + the resolved TCW; the run-segmentation/cid join is done by
// the ASMTRACE log (same PC family, same per-part cadence) or downstream. We additionally
// stamp the cid we can read cheaply: the 6 body bases' active+sprite_id, choosing the
// active body whose pass index matches this run (s_charqRun).

struct CharqPart {
	u32  pcw, isp, tcw, tsp;      // PVR header words (resolved)
	u32  rec[CHARQ_REC_BYTES/4];  // the full 0x40-byte PVR record (verts decode downstream)
	u32  recBase;                 // guest addr of the record (debug)
};
static const int CHARQ_MAX_PARTS = 256;
struct CharqRunObj {
	u32 node; int cid; int sprite_id;        // owning body identity (best-effort)
	CharqPart parts[CHARQ_MAX_PARTS];
	int nparts;
};
static const int CHARQ_MAX_OBJS = 8;         // <= 6 bodies + slack
static CharqRunObj s_charqRun[CHARQ_MAX_OBJS];
static int         s_charqRunN   = 0;
static u32         s_charqVframe  = 0xFFFFFFFFu;
static unsigned long s_fireCharq  = 0;
static FILE*       s_charqFile    = nullptr;
static long        s_charqWritten = 0;
// Run-segmentation signal: the node base of the most recent OBJ_BEGIN/SAT_BEGIN
// (loc_8c03093c / loc_8c030af8). loc_8c0344d4 renders that node's body parts right
// after, so a CHANGE in this value marks a new per-body run. Set by the begin
// handlers; consumed (and its "consumed" generation tracked) by charqOpenRun.
static u32 s_charqBeginNode = 0;
static u32 s_charqBeginCid  = 0xFFFFFFFFu;
static u32 s_charqBeginSid  = 0xFFFFFFFFu;
static u32 s_charqRunNode   = 0;   // node the current run is bound to

// Return the run object the current body part belongs to. Run = the contiguous
// stream of body parts following one OBJ_BEGIN/SAT_BEGIN. We bind a run to the
// most-recent begin node (s_charqBeginNode). If the begin node changed since the
// current run was opened, OR no run exists this frame, we open a new run.
static CharqRunObj* charqGetRun()
{
	u32 begin = s_charqBeginNode;
	// Same node as the current run -> keep appending to it.
	if (s_charqRunN > 0 && s_charqRunNode == begin && begin != 0)
		return &s_charqRun[s_charqRunN - 1];
	// New body run (or first run of the frame): open one bound to the begin node.
	if (s_charqRunN >= CHARQ_MAX_OBJS) {
		// Cap: keep folding into the last run rather than dropping parts.
		return &s_charqRun[CHARQ_MAX_OBJS - 1];
	}
	CharqRunObj* o = &s_charqRun[s_charqRunN];
	memset(o, 0, sizeof *o);
	o->node = begin;
	// Identity from the begin handler (already read off the node), with a fallback to
	// reading the node directly if the begin handler hasn't populated it yet.
	if (begin && inRam(begin)) {
		o->cid       = (s_charqBeginCid != 0xFFFFFFFFu) ? (int)s_charqBeginCid
		                                                : (int)(u8)addrspace::read8(begin + OFF_CHAR_ID);
		o->sprite_id = (s_charqBeginSid != 0xFFFFFFFFu) ? (int)s_charqBeginSid
		                                                : (int)(u16)addrspace::read16(begin + OFF_SPRITE_ID);
	} else {
		o->cid = -1; o->sprite_id = -1;
	}
	s_charqRunNode = begin;
	s_charqRunN++;
	return o;
}

// Flush the accumulated per-character runs for the just-finished video frame.
static void charqFlushFrame(u32 vframe)
{
	if (s_charqRunN == 0) return;
	static const long CHARQ_CAP = []{
		const char* v = getenv("MAPLECAST_CHARQ_JSONL_CAP");
		if (v) { long c = atol(v); if (c >= (1L << 20)) return c; }
		return 16L * 1024 * 1024;
	}();
	if (!s_charqFile) {
		s_charqFile = fopen("/dev/shm/mc_charq_render.jsonl", "w");
		s_charqWritten = 0;
		if (s_charqFile) setvbuf(s_charqFile, nullptr, _IOFBF, 1 << 16);
	}
	if (s_charqFile && s_charqWritten >= CHARQ_CAP) {
		s_charqFile = freopen("/dev/shm/mc_charq_render.jsonl", "w", s_charqFile);
		s_charqWritten = 0;
		if (s_charqFile) setvbuf(s_charqFile, nullptr, _IOFBF, 1 << 16);
	}
	if (!s_charqFile) { s_charqRunN = 0; return; }

	char b[4096]; int n;
	for (int i = 0; i < s_charqRunN; i++) {
		CharqRunObj& o = s_charqRun[i];
		if (o.nparts == 0) continue;
		n = snprintf(b, sizeof b,
			"{\"frame\":%u,\"node\":\"0x%08X\",\"cid\":%d,\"sprite_id\":%d,\"nquads\":%d,\"quads\":[",
			vframe, o.node, o.cid, o.sprite_id, o.nparts);
		s_charqWritten += fwrite(b, 1, n, s_charqFile);
		for (int p = 0; p < o.nparts; p++) {
			const CharqPart& q = o.parts[p];
			n = snprintf(b, sizeof b,
				"%s{\"pcw\":\"0x%08X\",\"isp\":\"0x%08X\",\"tcw\":\"0x%08X\",\"tsp\":\"0x%08X\","
				"\"vram\":\"0x%08X\",\"rec_base\":\"0x%08X\",\"rec\":[",
				p ? "," : "", q.pcw, q.isp, q.tcw, q.tsp,
				(q.tcw & 0x1FFFFF) << 3, q.recBase);     // TCW texel addr = (tcw&0x1FFFFF)<<3
			s_charqWritten += fwrite(b, 1, n, s_charqFile);
			for (int w = 0; w < (int)(CHARQ_REC_BYTES/4); w++) {
				n = snprintf(b, sizeof b, "%s\"0x%08X\"", w ? "," : "", q.rec[w]);
				s_charqWritten += fwrite(b, 1, n, s_charqFile);
			}
			n = snprintf(b, sizeof b, "]}");
			s_charqWritten += fwrite(b, 1, n, s_charqFile);
		}
		n = snprintf(b, sizeof b, "]}\n");
		s_charqWritten += fwrite(b, 1, n, s_charqFile);
	}
	fflush(s_charqFile);
	s_charqRunN = 0;
}

static void mc_charqRenderHandler(const u32* r)
{
	// DIAG (one-shot): prove the handler is REACHED + reveal the actual caller pr's, so
	// we can confirm/adjust the body-vs-HUD filter on live. Logs the first 16 distinct pr
	// values seen (area-masked + raw) regardless of the filter below.
	{
		static u32  s_prSeen[16]; static int s_prN = 0; static unsigned long s_reach = 0;
		u32 prm = Sh4cntx.pr & SH4_AREA_MASK;
		if (s_reach++ == 0)
			fprintf(stderr, "[CHARQ-RENDER] handler REACHED (pc=0x%08X) — collecting caller pr's\n",
			        PC_CHARQ_SUBMIT);
		bool known = false; for (int i=0;i<s_prN;i++) if (s_prSeen[i]==prm) { known=true; break; }
		if (!known && s_prN < 16) {
			s_prSeen[s_prN++] = prm;
			fprintf(stderr, "[CHARQ-RENDER] caller pr=0x%08X (masked 0x%08X) %s\n",
			        Sh4cntx.pr, prm, (prm==PC_BODY_SUBMIT_RET_M) ? "<== BODY (loc_8c0344d4)" : "");
		}
	}

	// BODY-vs-HUD filter: only capture when this loc_8C1244B0 invocation came from the
	// body render (loc_8c0344d4) submit jsr. pr (caller return) must be 0x0C03487A.
	u32 pr = Sh4cntx.pr & SH4_AREA_MASK;
	if (pr != PC_BODY_SUBMIT_RET_M) return;

	if (s_fireCharq++ == 0)
		fprintf(stderr, "[CHARQ-RENDER] first fired (pc=0x%08X loc_8C1244B0 per body part, "
		                "pr=0x%08X) -> /dev/shm/mc_charq_render.jsonl\n",
		        PC_CHARQ_SUBMIT, Sh4cntx.pr);

	u32 vframe = addrspace::read32(0x8C3496B0);

	// Video-frame boundary: flush the previous frame's accumulated runs, reset.
	if (vframe != s_charqVframe) {
		if (s_charqVframe != 0xFFFFFFFFu) charqFlushFrame(s_charqVframe);
		s_charqVframe = vframe;
		s_charqRunN   = 0;
	}

	// The PVR record base: r14 at this PC = base + 0x20 (one `add 0x20,r14` executed).
	u32 recBase = (norm(r[14]) | 0x0C000000u) - CHARQ_REC_OFF;
	if (!inRam(recBase)) return;

	// Per-part run attribution: bind this part to the body whose OBJ_BEGIN/SAT_BEGIN
	// (s_charqBeginNode) most recently fired. loc_8c0344d4 renders one body's full
	// part list contiguously right after its begin, so a change in s_charqBeginNode
	// segments the runs. (The begin hooks already fire under the master gate.)
	CharqRunObj* o = charqGetRun();
	if (!o) return;
	if (o->nparts >= CHARQ_MAX_PARTS) return;

	CharqPart& q = o->parts[o->nparts];
	q.recBase = recBase;
	for (int w = 0; w < (int)(CHARQ_REC_BYTES/4); w++)
		q.rec[w] = addrspace::read32(recBase + (u32)w * 4);
	q.pcw = q.rec[0];      // base+0x00
	q.isp = q.rec[1];      // base+0x04
	q.tcw = q.rec[2];      // base+0x08  (RESOLVED: VRAM texel addr + fmt + pal bank)
	q.tsp = q.rec[5];      // base+0x14  (TSP/pal extra)
	o->nparts++;
}


// ===========================================================================
// BODYCAP (MAPLECAST_BODYCAP) — body part DECODED pixels keyed by the RENDER selector.
// Fires at the SAME PC as ASMTRACE (0x8C034864, loc_8c0344d4 per-part convergence).
// See mc_bodyCapEnabled for the full selector-space reconciliation. READ-ONLY.
//
// Standard flycast twiddle (Morton) index — same math as core/rend/texconv.cpp and
// the proven gamestate.cpp partDecodeToPPM (flycast-canonical, y-first), self-contained
// here so this file links without the gamestate file-statics.
static inline u32 bc_twiddle(u32 x, u32 y, u32 x_sz, u32 y_sz) {
	u32 rv = 0, sh = 0; x_sz >>= 1; y_sz >>= 1;
	while (x_sz != 0 || y_sz != 0) {
		if (y_sz != 0) { rv |= (y & 1) << sh; y_sz >>= 1; y >>= 1; sh++; }
		if (x_sz != 0) { rv |= (x & 1) << sh; x_sz >>= 1; x >>= 1; sh++; }
	}
	return rv;
}
// Write w*h PAL4 texels at texPtr -> PPM (P6, magenta = transparent). `twid`=true uses
// the flycast TWIDDLE order (DM00 persistent slots); false = LINEAR (the GFX1 blob /
// 0x0CE60000 scratch are row-major). PAL4: 2 indices/byte, index 0 = transparent.
static void bcWritePPM(u32 texPtr, int w, int h, u32 palBase, bool twid, const char* fn) {
	FILE* pf = fopen(fn, "wb"); if (!pf) return;
	fprintf(pf, "P6\n%d %d\n255\n", w, h);
	bool palOk = inRam(palBase);
	for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
		u32 idx = twid ? bc_twiddle((u32)x, (u32)y, (u32)w, (u32)h) : (u32)(y * w + x);
		u8  b   = (u8)addrspace::read8(texPtr + (idx >> 1));
		u32 pidx = (idx & 1) ? (b >> 4) : (b & 0xf);
		u8 rr = 0xff, gg = 0x00, bb = 0xff;            // magenta = transparent default
		if (pidx != 0 && palOk) {
			u16 pe = (u16)addrspace::read16(palBase + pidx * 2);   // ARGB4444 LE
			u8 aa = ((pe >> 12) & 0xf) * 17;
			if (aa != 0) { rr = ((pe>>8)&0xf)*17; gg = ((pe>>4)&0xf)*17; bb = (pe&0xf)*17; }
		}
		u8 rgb[3] = {rr, gg, bb}; fwrite(rgb, 1, 3, pf);
	}
	fclose(pf);
}
static void bcWriteRaw(u32 texPtr, int nbytes, const char* fn) {
	FILE* rf = fopen(fn, "wb"); if (!rf) return;
	for (int i = 0; i < nbytes; i++) fputc((u8)addrspace::read8(texPtr + (u32)i), rf);
	fclose(rf);
}

static unsigned long s_fireBody = 0;
static bool          s_bcCleared = false;
static bool          s_bcSeen[0x40][1024] = {{false}};   // [char_id][render selector] first-seen

static void mc_bodyCapHandler(const u32* r)
{
	if (s_fireBody++ == 0)
		fprintf(stderr, "[BODYCAP] first fired (pc=0x%08X loc_8c0344d4 per-part) — BODY part "
		                "pixels keyed by RENDER selector (read_u16(r11+6)) -> /dev/shm/PL*_body_*.ppm\n",
		        PC_ASM_PART);

	// One-time stale clear (the body manifest + log); /dev/shm is maplecast-owned.
	if (!s_bcCleared) {
		for (int c = 0; c < 0x40; c++) {
			char mn[96]; snprintf(mn, sizeof mn, "/dev/shm/PL%02X_body.manifest", c); remove(mn);
		}
		remove("/dev/shm/mc_bodycap.log");
		s_bcCleared = true;
	}

	u32 node = norm(r[14]) | 0x0C000000u;   // r14 = char/render struct base
	u32 r11  = r[11];                       // per-part record cursor (+6 = render selector)
	if (!inRam(node) || !inRam(r11)) return;

	u16 sel = (u16)addrspace::read16(r11 + 0x6);     // RENDER selector — THE KEY (533-541 for Ryu)
	if (sel == 0x00FF || sel >= 1024) return;        // terminator / out of range
	u16 recpal = (u16)addrspace::read16(r11 + 0x2);  // the +2 palette word (dy field doubles as pal)
	unsigned palRow = (recpal >> 4) & 0x7;           // 3-bit palette row (PalMod-confirmed)

	u8 cid = (u8)addrspace::read8(node + OFF_CHAR_ID);
	if (cid >= 0x40) return;
	if (s_bcSeen[cid][sel]) return;                  // first-seen per (char,render-selector)

	// (A) GFX1 blob — authoritative DIMS for this render selector (disasm loc_8c0345c4):
	//   gfx1=*(node+0x15c); off=read_u32(gfx1+sel*4); blob=gfx1+off; w=blob[2]<<3,h=blob[3]<<3.
	u32 gfx1 = addrspace::read32(node + OFF_GFX1);
	if (!inRam(gfx1)) return;
	u32 blob = (gfx1 + addrspace::read32(gfx1 + (u32)sel * 4)) & 0x0FFFFFFFu; blob |= 0x0C000000u;
	if (!inRam(blob)) return;
	int w = ((int)(u8)addrspace::read8(blob + 2)) << 3;
	int h = ((int)(u8)addrspace::read8(blob + 3)) << 3;
	if (w <= 0 || h <= 0 || w > 512 || h > 512) return;
	int bytes = (w * h) / 2;                          // 4bpp

	u32 palP = addrspace::read32(node + OFF_PAL_PTR); // node+0x164 (ARGB4444)
	u32 palBase = inRam(palP) ? (palP + palRow * 32) : 0;

	s_bcSeen[cid][sel] = true;

	// (B) DM00 persistent decoded twiddled slots — the proven-CLEAN source. dirBase =
	// *(0x0CE80008), stride 0x10: entry+0=(w,h) u16 px, entry+4=fmt word, entry+8=texels.
	// Render selector != DM00 key, so MATCH this selector's GFX1 dims (w,h) against the
	// DM00 run to find the right key (the body's parts are a contiguous run from a small
	// base). We scan keys 0..255 and take the first PAL4/PAL8 entry whose dims equal w,h.
	u32 dirBase = addrspace::read32(0x0CE80008);
	if (!inRam(dirBase)) { u32 alt = addrspace::read32(0x8CE80008); if (inRam(alt)) dirBase = alt; }
	int   dmKey = -1; u32 dmTex = 0; int dmFmt = 5;
	if (inRam(dirBase)) {
		for (int k = 0; k < 256; k++) {
			u32 e  = dirBase + (u32)k * DM00_ENTRY_STRIDE;
			u32 e0 = addrspace::read32(e), e4 = addrspace::read32(e + 4), e8 = addrspace::read32(e + 8);
			int dw = (int)(e0 & 0xffff), dh = (int)((e0 >> 16) & 0xffff);
			if (dw == w && dh == h && inRam(e8)) {
				dmKey = k; dmTex = e8 | 0x0C000000u;
				dmFmt = ((u8)((e4 >> 8) & 0xff) == 0x03) ? 6 : 5;   // 0x03=PAL8 else PAL4
				break;
			}
		}
	}

	// Primary CLEAN dump = DM00 twiddled if a dims-match was found; the file is keyed by
	// the RENDER selector so it matches the ASMTRACE assembly (sel field).
	char ppmA[112]; snprintf(ppmA, sizeof ppmA, "/dev/shm/PL%02X_body_%04u.ppm", cid, (unsigned)sel);
	if (dmKey >= 0 && dmFmt == 5) bcWritePPM(dmTex, w, h, palBase, /*twid=*/true, ppmA);
	else                          bcWritePPM(blob + 4, w, h, palBase, /*twid=*/false, ppmA);  // GFX1-blob fallback

	// Cross-dump BOTH candidate sources + RAW so the first capture is self-diagnosing:
	//   _gfx1lin  = GFX1 blob texels (blob+4), LINEAR  (tests source A)
	//   _dm00twid = DM00 slot texels, TWIDDLE          (tests source B)
	char ppmL[112]; snprintf(ppmL, sizeof ppmL, "/dev/shm/PL%02X_body_%04u_gfx1lin.ppm", cid, (unsigned)sel);
	bcWritePPM(blob + 4, w, h, palBase, /*twid=*/false, ppmL);
	if (dmKey >= 0) {
		char ppmT[112]; snprintf(ppmT, sizeof ppmT, "/dev/shm/PL%02X_body_%04u_dm00twid.ppm", cid, (unsigned)sel);
		bcWritePPM(dmTex, w, h, palBase, /*twid=*/true, ppmT);
	}
	char rfp[112]; snprintf(rfp, sizeof rfp, "/dev/shm/PL%02X_body_%04u.raw", cid, (unsigned)sel);
	bcWriteRaw(blob + 4, bytes, rfp);   // GFX1-blob raw 4bpp (offline-palette fallback)

	char mn[96]; snprintf(mn, sizeof mn, "/dev/shm/PL%02X_body.manifest", cid);
	FILE* mf = fopen(mn, "a");
	if (mf) {
		if (ftell(mf) == 0)
			fprintf(mf, "# rsel palRow w h gfx1 blob dmKey dmTex dmFmt palBase ppm  "
			            "(body parts keyed by RENDER selector read_u16(r11+6))\n");
		fprintf(mf, "%u %u %d %d %08x %08x %d %08x %d %08x PL%02X_body_%04u.ppm\n",
		        (unsigned)sel, palRow, w, h, gfx1, blob, dmKey, dmTex, dmFmt, palBase, cid, (unsigned)sel);
		fclose(mf);
	}

	if ((s_fireBody % 32) == 1) {
		FILE* dl = fopen("/dev/shm/mc_bodycap.log", "a");
		if (dl) {
			fprintf(dl, "[BODY] fire#%lu cid=%u(PL%02X) rsel=%u %dx%d gfx1=%08x blob=%08x "
			            "dmKey=%d dmTex=%08x dmFmt=%d palBase=%08x\n",
			        s_fireBody, cid, cid, (unsigned)sel, w, h, gfx1, blob, dmKey, dmTex, dmFmt, palBase);
			fclose(dl);
		}
	}
}

void mc_oracleInit()
{
	static bool logged = false;
	if (logged) return;
	logged = true;
	if (mc_asmTraceEnabled)
		fprintf(stderr, "[ASMTRACE] ENABLED — ground-truth per-part body assembly at 0x%08X "
		                "(loc_8c0344d4 per-part convergence): sel=read_u16(r11+6), dx/dy=r11+0/+2, "
		                "accX=r10, screenX/Y=f32@(r15+0x30/0x34) -> /dev/shm/mc_assembly.log. "
		                "Hold a clean STANDING pose to capture the standing part-list.\n", PC_ASM_PART);
	if (mc_bodyCapEnabled)
		fprintf(stderr, "[BODYCAP] ENABLED — BODY part DECODED pixels keyed by the RENDER selector "
		                "(read_u16(r11+6), the 533-541 namespace) at 0x%08X (loc_8c0344d4 per-part). "
		                "Primary = DM00 twiddled slot (dims-matched), fallbacks = GFX1-blob linear + "
		                "DM00 twiddle + RAW 4bpp -> /dev/shm/PL*_body_*.ppm + PL*_body.manifest. "
		                "Hold a clean STANDING Ryu (PL00) to fill the body part-list.\n", PC_ASM_PART);
	if (mc_decodeTraceEnabled)
		fprintf(stderr, "[DECODETRACE] ENABLED — BOTH LZSS decoders traced: WORD loc_8c03552a "
		                "@0x%08X + BYTE loc_8c0354c0 @0x%08X (caller=pr, src=r4, dest=r5/r6) "
		                "-> /dev/shm/mc_decodetrace.log (+summary). Expect BYTE-LZSS ~1190x from "
		                "loc_8c033d78 at match load. Play a fresh match to capture the load decode.\n",
		        PC_DECODE_ENTRY, PC_DECODE_ENTRY2);
	if (mc_quadCaptureEnabled)
		fprintf(stderr, "[QUADCAPTURE] ENABLED — clean DECODED part pixels at 0x%08X "
		                "(loc_8c0354c0 jsr-return, post-decode): 4bpp from 0x0CE60000 (r8) + palette "
		                "(node+0x164) -> /dev/shm/PL*_gfx1_*.ppm + PL*_raw_*.bin + PL*_gfx1.manifest, "
		                "keyed by +6 selector (read_u16(r13+6)). Cross-check -> /dev/shm/mc_partdir.log.\n",
		        PC_QUAD_DEC);
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
	if (mc_probeEnabledStatic)
		fprintf(stderr, "[ORACLE-PROBE] GENERIC probe ENABLED (READ-ONLY) — %d PC(s) armed from config; "
		                "dumps -> /dev/shm/mc_probe.log. Edit config + restart to reconfigure (no recompile).\n",
		        s_nprobe);
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
	// The quad-DONE PC (0x8C033EC0) is the post-WRITE quad-store: the 16-byte quad header
	// {w,h,attr,texptr,palptr} is fully written but the part PIXELS at r8 are NOT decoded
	// yet. Hook it only for the decode-quad sub-flag (the HEADER buffer capture). The
	// force-split makes it a block start so the GenCall injects (it's mid-block).
	if (m == PC_QUAD_DONE_M) return mc_decodeQuadsEnabled;
	// The quad-DEC PC (0x8C033ED0) is the jsr-RETURN target right AFTER the BYTE-LZSS
	// decoder loc_8c0354c0 filled the pixels at r8 — the CLEAN-pixel capture point for
	// QUADCAPTURE (r8=decoded part, r10=node, r13=cell cursor sel@+6, r12=palptr). The
	// force-split makes it a block start so the GenCall injects (it's mid-block).
	if (m == PC_QUAD_DEC_M) return mc_quadCaptureEnabled;
	// The LOAD-DECODE part hook (loc_8c032696 / 0x8C032776). Only hooked when the
	// decode-hook flag is set. 0x8C032776 is the jsr return target (a natural block
	// start), so the decoder force-split in decoder.cpp is a no-op for it (rpc==vaddr)
	// — but mc_isHookedPC must return true so rec_x64 injects the GenCall.
	if (m == PC_DECODE_DONE_M) return mc_decodeHookEnabled;
	// DECODE-TRACE: the LZSS decoder ENTRY (loc_8c03552a / 0x8C03552A). Only hooked
	// when the trace flag is set. It's a jsr/jmp target (block start) so the decoder
	// force-split is a no-op (rpc==vaddr) — mc_isHookedPC just has to return true so
	// rec_x64 injects the GenCall.
	if (m == PC_DECODE_ENTRY_M) return mc_decodeTraceEnabled;
	// DECODE-TRACE (BYTE-LZSS): the second decoder ENTRY (loc_8c0354c0 / 0x8C0354C0).
	// Block start (jsr/jmp target) so the force-split is a no-op; the GenCall injects.
	if (m == PC_DECODE_ENTRY2_M) return mc_decodeTraceEnabled;
	// ASMTRACE: the per-part convergence PC inside loc_8c0344d4 (0x8C034864). Only hooked
	// when the asm-trace flag is set. Mid-block -> the decoder force-split makes it a block
	// start; this returns true so rec_x64 injects the GenCall there.
	if (m == PC_ASM_PART_M) return mc_asmTraceEnabled || mc_bodyCapEnabled;
	// CHARQ-RENDER: the per-part PVR-record completion PC inside loc_8C1244B0
	// (0x8C1248CC). Only hooked when the charq-render flag is set. Mid-block -> the
	// decoder force-split makes it a block start; return true so rec_x64 injects.
	if (m == PC_CHARQ_SUBMIT_M) return mc_charqRenderEnabled;
	// GENERIC PROBE: any PC configured in /dev/shm/mc_oracle_probe.conf (parsed once at
	// static init). Masked compare so the disasm 0x8C.. label matches the executed 0x0C..
	// alias. The probe PCs may be mid-block (e.g. 0x8C1248CC) -> the decoder force-split
	// in decoder.cpp makes them block starts so the GenCall injects.
	if (mc_probeEnabledStatic) {
		for (int i = 0; i < s_nprobe; i++)
			if (s_probes[i].pcMasked == m) return true;
	}
	return false;
}

// GENERIC PROBE handler — dump the matched probe's spec to /dev/shm/mc_probe.log.
// READ-ONLY (Sh4cntx.r[] + addrspace::read* only). Append-with-flush + truncate-and-
// rewind at the cap so the tail is live and /dev/shm can't fill. Returns true if `mpc`
// matched a probe (so the caller can return early).
static bool mc_probeHandler(const u32* r, u32 mpc)
{
	int hit = -1;
	for (int i = 0; i < s_nprobe; i++) if (s_probes[i].pcMasked == mpc) { hit = i; break; }
	if (hit < 0) return false;
	Probe& p = s_probes[hit];
	p.fires++;

	static const long PROBE_CAP = []{
		const char* v = getenv("MAPLECAST_ORACLE_PROBE_CAP");
		if (v) { long c = atol(v); if (c >= (1L << 20)) return c; }   // floor 1 MiB
		return 16L * 1024 * 1024;
	}();
	static const char* s_logPath = []{
		const char* v = getenv("MAPLECAST_ORACLE_PROBE_LOG");
		return v ? v : "/dev/shm/mc_probe.log";
	}();
	static FILE* lf = nullptr;
	static long  lw = 0;
	if (!lf) { lf = fopen(s_logPath, "w"); lw = 0;            // O_TRUNC: drop a stale prior-run file
		if (lf) fprintf(stderr, "[ORACLE-PROBE] logging -> %s (cap %ldMiB)\n", s_logPath, PROBE_CAP >> 20); }
	if (!lf) return true;
	if (lw >= PROBE_CAP) { lf = freopen(s_logPath, "w", lf); lw = 0; if (!lf) return true; }

	u32 vframe = addrspace::read32(0x8C3496B0);   // SH4 video-frame counter (work.asm)
	char b[1024]; int n;
	n = snprintf(b, sizeof b, "[PROBE pc=0x%08X %s vframe=%u fire=%lu]\n",
	             mpc | 0x0C000000u, p.label, vframe, p.fires);
	lw += fwrite(b, 1, n, lf);

	for (int t = 0; t < p.ntok; t++) {
		ProbeTok& tk = p.toks[t];
		switch (tk.kind) {
		case ProbeTok::REGS:
			for (int g = 0; g < 16; g += 4) {
				n = snprintf(b, sizeof b, "  r%-2d=%08X r%-2d=%08X r%-2d=%08X r%-2d=%08X\n",
				             g, r[g], g+1, r[g+1], g+2, r[g+2], g+3, r[g+3]);
				lw += fwrite(b, 1, n, lf);
			}
			n = snprintf(b, sizeof b, "  pr=%08X gbr=%08X macl=%08X mach=%08X\n",
			             Sh4cntx.pr, Sh4cntx.gbr, Sh4cntx.mac.l, Sh4cntx.mac.h);
			lw += fwrite(b, 1, n, lf);
			break;
		case ProbeTok::REG:
			n = snprintf(b, sizeof b, "  r%d=%08X\n", tk.reg, r[tk.reg]); lw += fwrite(b, 1, n, lf); break;
		case ProbeTok::PR:
			n = snprintf(b, sizeof b, "  pr=%08X\n", Sh4cntx.pr); lw += fwrite(b, 1, n, lf); break;
		case ProbeTok::GBR:
			n = snprintf(b, sizeof b, "  gbr=%08X\n", Sh4cntx.gbr); lw += fwrite(b, 1, n, lf); break;
		case ProbeTok::MACL:
			n = snprintf(b, sizeof b, "  macl=%08X\n", Sh4cntx.mac.l); lw += fwrite(b, 1, n, lf); break;
		case ProbeTok::MACH:
			n = snprintf(b, sizeof b, "  mach=%08X\n", Sh4cntx.mac.h); lw += fwrite(b, 1, n, lf); break;
		case ProbeTok::MEM: {
			n = snprintf(b, sizeof b, "  mem[0x%08X..+%u]:\n", tk.addr, tk.len); lw += fwrite(b, 1, n, lf);
			for (u32 o = 0; o < tk.len; o += 16) {
				n = snprintf(b, sizeof b, "    %08X:", tk.addr + o);
				for (u32 j = 0; j < 16 && o + j < tk.len; j++)
					n += snprintf(b + n, sizeof b - n, " %02X", (u8)addrspace::read8(tk.addr + o + j));
				n += snprintf(b + n, sizeof b - n, "\n"); lw += fwrite(b, 1, n, lf);
			}
			break; }
		case ProbeTok::RMEM: {
			u32 base = r[tk.reg] + tk.off;
			n = snprintf(b, sizeof b, "  rmem[r%d+0x%X=0x%08X..+%u]:\n", tk.reg, tk.off, base, tk.len);
			lw += fwrite(b, 1, n, lf);
			for (u32 o = 0; o < tk.len; o += 16) {
				n = snprintf(b, sizeof b, "    %08X:", base + o);
				for (u32 j = 0; j < 16 && o + j < tk.len; j++)
					n += snprintf(b + n, sizeof b - n, " %02X", (u8)addrspace::read8(base + o + j));
				n += snprintf(b + n, sizeof b - n, "\n"); lw += fwrite(b, 1, n, lf);
			}
			break; }
		case ProbeTok::STACK: {
			u32 sp = r[15];
			n = snprintf(b, sizeof b, "  stack[r15=0x%08X, %u words]:\n", sp, tk.len); lw += fwrite(b, 1, n, lf);
			for (u32 w = 0; w < tk.len; w++) {
				u32 v = addrspace::read32(sp + w * 4);
				n = snprintf(b, sizeof b, "    [sp+%02X] %08X = %08X\n", w * 4, sp + w * 4, v);
				lw += fwrite(b, 1, n, lf);
			}
			break; }
		}
	}
	fflush(lf);
	return true;
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
	// Mask to the SH4 external area so we route correctly whether the recompiler
	// passed the cached (0x8C..) or physical (0x0C..) alias of the PC.
	u32 mpc = pc & SH4_AREA_MASK;

	// GENERIC RUNTIME-CONFIGURABLE PROBE (MAPLECAST_ORACLE_PROBE) — checked FIRST so a
	// config-armed PC always dumps its spec, even if it coincides with a built-in hook.
	// Pure read-only dump; returns true (and we return) when this PC is a configured
	// probe. When the probe is OFF, s_nprobe==0 so this is a single int compare.
	if (mc_probeEnabledStatic && mc_probeHandler(r, mpc)) return;

	// DECODE-TIME part hook (fires PRE-match at character load, independent of the
	// frame-oracle paths). Handle first + return: it shares no per-frame buffer.
	if (mpc == PC_DECODE_DONE_M) {
		if (mc_decodeHookEnabled) mc_decodeHandler(r);
		return;
	}

	// DECODE-TRACE: the LZSS decoder ENTRY (BOTH decoders). Logs caller(pr)/src(r4)/
	// dest(r5/r6) per call so the gameplay-atlas decoder is named CONCRETELY.
	// which 0 = WORD-LZSS loc_8c03552a, 1 = BYTE-LZSS loc_8c0354c0 (the ~1190× one).
	// Independent of the frame-oracle paths; shares no per-frame buffer. Handle + return.
	if (mpc == PC_DECODE_ENTRY_M) {
		if (mc_decodeTraceEnabled) mc_decodeTraceHandler(r, /*which=*/0);
		return;
	}
	if (mpc == PC_DECODE_ENTRY2_M) {
		if (mc_decodeTraceEnabled) mc_decodeTraceHandler(r, /*which=*/1);
		return;
	}

	// QUADCAPTURE clean-pixel capture — the jsr-RETURN PC 0x8C033ED0, right AFTER the
	// BYTE-LZSS decoder loc_8c0354c0 filled the pixels at r8. Independent of the
	// per-frame paths; shares no per-frame buffer. Handle + return.
	if (mpc == PC_QUAD_DEC_M) {
		if (mc_quadCaptureEnabled) mc_quadCaptureHandler(r);
		return;
	}

	// ASMTRACE per-part body assembly — loc_8c0344d4 convergence PC (0x8C034864).
	// Independent of the per-frame oracle buffer; appends one line per part. Handle + return.
	if (mpc == PC_ASM_PART_M) {
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
		// CHARQ-RENDER run segmentation: this body's parts (emitted next by loc_8c0344d4)
		// belong to THIS node. Record its identity for charqGetRun.
		if (mc_charqRenderEnabled) {
			u32 cnode = node | 0x0C000000u;
			s_charqBeginNode = cnode;
			s_charqBeginCid  = (u32)(u8)addrspace::read8(cnode + OFF_CHAR_ID);
			s_charqBeginSid  = (u32)(u16)addrspace::read16(cnode + OFF_SPRITE_ID);
		}
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
		}
		// R2: same running-TA-cursor capture for satellite/effect render entries, in
		// walk order, so a projectile/cape/super shows up in the per-object sequence.
		if (mc_probeEnabled && oi >= 0) mc_r2Record(oi, node, /*isSat=*/true);
		return;
	}

	// Only the PC_QUAD_DONE header capture runs below (QUADCAPTURE clean-pixel capture
	// now fires at PC_QUAD_DEC_M / 0x8C033ED0, handled above). If the decode-quad sub-flag
	// is not set this PC isn't hooked (mc_isHookedPC) so we never reach here — guard anyway.
	if (mpc != PC_QUAD_DONE_M) return;
	if (!mc_decodeQuadsEnabled) return;

	// mpc == PC_QUAD_DONE_M — the POST-WRITE capture point (0x8C033EC0). The 16-byte
	// quad at r14 is now FULLY written and r14 has NOT yet advanced. r10 = node base.
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

// ---- Per-pass classification (CHARQ Phase-1 fix) ---------------------------
// MVC2 renders MULTIPLE TA passes per VIDEO frame, each its own serverPublish ->
// DequeueRender -> _localFrameNum++. The CHARACTER pass carries the bodies (the
// large tr list + body-band y coords); a separate HUD/composite pass carries only
// a tiny op/tr list of top-of-screen sprites + the stage backdrop. The Oracle was
// latching whichever pass ran the flush LAST (the HUD pass) -> bodies got 0 quads.
// This struct lets collectScreenQuads report what it saw so frameFlush can pick the
// character pass. PROVEN by the live STAF parse on this build (mc_staf_geom.log):
// the body pass = op~265 tr~2024 with 281 body-band (y 240-439) coords.
struct PassStats {
	int op, pt, tr;       // PolyParam list sizes (raw, pre-filter)
	int sprite;           // kept sprite quads (s_nscreen)
	int bodyBand;         // kept sprite quads with center y in [60,440] (playfield incl HUD)
	int realBody;         // kept sprite quads with center y in [200,460] (TRUE body, excl HUD)
	int isRTT;            // ctx->rend.isRTT (FB_W_SOF1 & 0x1000000) — logged to CONFIRM
};

// ---- Per-frame SCREEN-quad recovery from ta_parse(ctx) ---------------------
// loc_8c033e90 (the LOAD-decode quad emitter) does NOT fire per frame during
// gameplay (proven: it ran once at match load, frame 2568). The per-frame SCREEN
// quads the GPU actually draws are in the TA list — recovered by ta_parse here.
// This reuses the EXACT de-index + sprite-classifier proven in serverPublish's
// MAPLECAST_FRAME_ORACLE block (maplecast_mirror.cpp ~2494-2588): try rc.idx
// de-index, fall back to direct rc.verts (autosort-tr sprites), filter out
// clears / page-tiled bg / oversized stage layers / opaque fills.
static void collectScreenQuads(rend_context& rc, PassStats* ps = nullptr)
{
	s_nscreen = 0;
	const u32 nverts = (u32)rc.verts.size();
	// MAPLECAST_QDIAG — pre-filter dump of EVERY parsed TA poly to /dev/shm/mc_qdiag.log:
	// listType, screen Y range, tcw/tsp/pcw, textured, w/h, and the computed cull flags.
	// Answers: ARE there body-region (y~240-433) textured polys in rc.global_param_*,
	// and which flag drops them? READ-ONLY.
	// CHARQ Phase-1 FIX: the old code opened "w" once on the FIRST in-match flush, so the
	// dump only ever captured the FIRST pass of the FIRST frame (usually the HUD/composite
	// pass) and never the character pass. Now we APPEND and tag each poly with the pass's
	// op/tr counts + a sequential pass index, dumping the first few in-match passes so the
	// CHARACTER pass (op~265 tr~2024) is guaranteed present. Bounded by s_qdiagPasses.
	static int   s_qdiag = getenv("MAPLECAST_QDIAG") ? 1 : 0;  // 0=off, 1=arm, 2=running, 3=done
	static FILE* s_qf = nullptr;
	static int   s_qdiagPasses = 0;
	static const int QDIAG_MAX_PASSES = 8;     // capture the first 8 in-match passes
	if (s_qdiag == 1) {                        // first armed call: truncate + start
		s_qf = fopen("/dev/shm/mc_qdiag.log", "w");
		s_qdiag = s_qf ? 2 : 0;
	}
	int s_qdiagPassIdx = s_qdiag == 2 ? s_qdiagPasses : -1;
	if (s_qf && s_qdiag == 2) {
		bool bodyBandPass = (rc.global_param_tr.size() > 200);  // heuristic: the big tr list
		fprintf(s_qf, "=== PASS %d  op=%zu pt=%zu tr=%zu  (%s) ===\n",
		        s_qdiagPassIdx, rc.global_param_op.size(), rc.global_param_pt.size(),
		        rc.global_param_tr.size(), bodyBandPass ? "LIKELY-CHARACTER" : "likely-hud");
	}
	auto collect = [&](std::vector<PolyParam>& lst, int listType) {
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
				              "tiled=%d opaque=%d oversized=%d uv[%.2f,%.2f,%.2f,%.2f]\n",
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
			if (!isSprite) continue;
			ScreenQuad& q = s_screen[s_nscreen++];
			q.x=mnX; q.y=mnY; q.w=w; q.h=h; q.cx=(mnX+mxX)*0.5f; q.cy=cy;
			q.uMn=uMn; q.uMx=uMx; q.vMn=vMn; q.vMx=vMx;
			q.zMn=(zMn> 1e29f)?0.f:zMn; q.zMx=(zMx<-1e29f)?0.f:zMx;
			q.tcw=tcw; q.tsp=tsp; q.pcw=pcw; q.isp=pp.isp.full; q.srcBlend=srcB; q.dstBlend=dstB;
			q.fmt = fmt; q.vq = (int)((tcw>>30)&1);
			q.tw = 8 << ((tsp>>3)&7); q.th = 8 << (tsp&7);
			q.vramAddr = (tcw & 0x1FFFFF) << 3;
			q.obj = -1;
		}
	};
	collect(rc.global_param_op, 0);
	collect(rc.global_param_pt, 1);
	collect(rc.global_param_tr, 2);

	// Count body-band sprite quads (center y in [60,440], the playfield region; the
	// HUD lives above and the on-screen character parts span ~y65..360 per the live
	// STAF parse) — the discriminator frameFlush uses to pick the character pass
	// without depending on isRTT alone.
	int bodyBand = 0;
	// realBody: TRUE body region only (center y in [200,460]) — EXCLUDES the HUD band
	// (life bars / meters / portraits / timer live at y<~120). The live evidence on this
	// build/state shows the HUD/composite pass carries tr=40-52 with bodyBand>0 because
	// bodyBand[60,440] still counts HUD sprites; tightening to [200,460] isolates the real
	// on-screen body parts (the captured body objects were at y 231-439). This is the
	// character-pass discriminator (>=N realBody) — see frameFlush. The stale "tr>=500"
	// figure was never real on this build; realBody is the robust gate.
	int realBody = 0;
	for (int k = 0; k < s_nscreen; k++) {
		float cy = s_screen[k].cy;
		if (cy >= 60.f  && cy <= 440.f) bodyBand++;
		if (cy >= 200.f && cy <= 460.f) realBody++;
	}
	if (ps) {
		ps->op = (int)rc.global_param_op.size();
		ps->pt = (int)rc.global_param_pt.size();
		ps->tr = (int)rc.global_param_tr.size();
		ps->sprite = s_nscreen;
		ps->bodyBand = bodyBand;
		ps->realBody = realBody;
		// isRTT filled by caller (it owns ctx->rend).
	}

	// QDIAG: advance per pass; close after capturing the first QDIAG_MAX_PASSES
	// in-match passes (so the character pass is guaranteed in the file).
	if (s_qf && s_qdiag == 2) {
		fprintf(s_qf, "--- pass %d kept=%d bodyBand=%d ---\n",
		        s_qdiagPassIdx, s_nscreen, bodyBand);
		if (++s_qdiagPasses >= QDIAG_MAX_PASSES) {
			fclose(s_qf); s_qf = nullptr; s_qdiag = 3;
			fprintf(stderr, "[QDIAG] %d-pass dump written -> /dev/shm/mc_qdiag.log\n",
			        QDIAG_MAX_PASSES);
		}
	}
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

	// (3b) RE-READ the authoritative screen_xy from the node NOW. attributeScreenQuads
	// runs in frameFlush AFTER the full draw walk completed, so node+0xE0/+0xE4 hold
	// the CURRENT frame's post-transform position (Render Main Sprite, loc_8c03093c,
	// writes them DURING the walk). The o.sx/o.sy captured at OBJ_BEGIN entry are the
	// PREVIOUS frame's values (the write lags entry — see ~:1499). Using the stale
	// anchor mis-keyed body quads into the unassigned bucket. Refresh in place.
	for (int i = 0; i < s_nobj; i++) {
		Obj& o = s_objs[i];
		if (!o.fromBegin) continue;
		float nsx = rdF(o.node + OFF_SCREEN_X);
		float nsy = rdF(o.node + OFF_SCREEN_Y);
		if (!(std::isnan(nsx) || std::isnan(nsy) || fabsf(nsx) > 1e6f || fabsf(nsy) > 1e6f)) {
			o.sx = nsx; o.sy = nsy;
		}
	}

	// (3b) BBOX-OVERLAP attribution. The old centroid-distance test (ATTR_RADIUS=160)
	// dropped tall body quads: a torso/leg part can have its bbox center >160px from
	// the object's screen_xy (the foot/pivot anchor). Instead, expand an anchor BOX
	// around each object's screen_xy and attribute a quad to the object whose expanded
	// box contains the quad's bbox center, breaking ties by center distance. The box is
	// asymmetric-tall (chars are tall+narrow) and falls back to the centroid radius for
	// objects whose box doesn't reach the quad (small satellites). nearest-anchor wins.
	const float HALF_W   = 110.f;   // px horizontal half-extent of the anchor box
	const float UP       = 200.f;   // px above screen_xy (bodies extend UP from the foot anchor)
	const float DOWN     = 110.f;   // px below
	const float FALLBACK = 160.f;   // px centroid radius fallback (was ATTR_RADIUS)
	for (int k = 0; k < s_nscreen; k++) {
		ScreenQuad& q = s_screen[k];
		int   best = -1;
		bool  bestInBox = false;
		float bestD2 = FALLBACK * FALLBACK;
		for (int i = 0; i < s_nobj; i++) {
			const Obj& o = s_objs[i];
			if (!o.fromBegin) continue;          // only anchor on routine-confirmed objects
			if (o.sx == 0.f && o.sy == 0.f) continue;
			float dx = q.cx - o.sx, dy = q.cy - o.sy;
			bool inBox = (dx >= -HALF_W && dx <= HALF_W && dy >= -UP && dy <= DOWN);
			float d2 = dx*dx + dy*dy;
			// Prefer any in-box object over any out-of-box; within the same class pick
			// the nearest center. This makes tall body parts attribute (their center is
			// inside the tall box even when >160px from the foot anchor) while keeping a
			// nearest-anchor fallback for small satellites outside every box.
			if (inBox) {
				if (!bestInBox || d2 < bestD2) { bestInBox = true; bestD2 = d2; best = i; }
			} else if (!bestInBox && d2 < bestD2) {
				bestD2 = d2; best = i;
			}
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

// ===========================================================================
// CHARPASS CAPTURE (MAPLECAST_CHARQ) — the DEFINITIVE per-part body-quad capture.
//
// THE PROBLEM (confirmed by the QDIAG/ORACLE-PASS investigation): MVC2 emits
// MULTIPLE STARTRENDER passes per video frame. flycast's QueueRender is SINGLE-SLOT
// (ta_ctx.cpp:67-73): once `rqueue` holds one context, every subsequent STARTRENDER
// context that frame is `tactx_Recycle`'d (DROPPED) and QueueRender returns false.
// So only ONE context per video frame survives to DequeueRender -> render() ->
// serverPublish(). On MVC2 the surviving pass is the HUD/composite pass
// (isRTT=0, op~573/tr~42, the character layer flattened to ONE composite quad). The
// CHARACTER pass (the per-part body quads, op~265/tr~2024, body-band y240-433) is the
// one QueueRender DROPS — so serverPublish/mc_oracle_frameFlush NEVER sees it.
//
// THE FIX (option (a)): capture UPSTREAM at rend_start_render — for EVERY STARTRENDER
// context, BEFORE QueueRender can drop it. This is the ONLY point in the pipeline where
// the per-part character quads exist. We ta_parse the about-to-be-(maybe-)dropped ctx
// READ-ONLY (the exact call norend::Process makes — ctx is fully valid here; ta_parse
// only builds ctx->rend, never touches guest state) and route the CHARACTER pass into
// the existing Oracle capture path (collectScreenQuads + attributeScreenQuads + JSONL +
// the CHARQ accessor). The per-vframe dedup in mc_oracle_frameFlush makes this safe to
// call for BOTH the character and HUD passes (only the character pass emits).
//
// READ-ONLY + determinism-safe: ta_parse(ctx,true) builds ctx->rend just like norend;
// the real render path re-parses for the wire. We never enqueue, never recycle, never
// touch rqueue, never write guest RAM. Gated MAPLECAST_CHARQ + in-match (0x8C289624).
static bool mc_charqEnabled = (getenv("MAPLECAST_CHARQ") != nullptr);

// CHARQ character-pass discriminator threshold: the minimum number of kept sprite quads
// whose center y falls in the TRUE body region [200,460] (excludes the HUD band y<~120)
// required to treat a STARTRENDER pass as the CHARACTER pass and emit the JSONL. The live
// body capture on this build had 34 body-region screen quads at y 231-439, so any modest
// floor isolates a real on-screen body. Env-overridable (MAPLECAST_CHARQ_REALBODY_MIN).
static const int CHARQ_REALBODY_MIN = []{
	const char* e = getenv("MAPLECAST_CHARQ_REALBODY_MIN");
	int v = e ? atoi(e) : 5;
	return (v > 0) ? v : 5;
}();

void mc_oracle_charPassCapture(void* ctxv)
{
	if (!mc_charqEnabled || ctxv == nullptr) return;

	// IN-MATCH GATE (in_match @0x8C289624) — same gate the frame oracle uses. Outside a
	// match the character routine isn't drawing bodies; skip the ta_parse entirely so the
	// instrument is free on menus/attract.
	if (addrspace::read8(0x8C289624) == 0) return;

	TA_context* ctx = (TA_context*)ctxv;

	// STEP 2 — the [CHARPASS] CONFIRMATION LOG. Per STARTRENDER: ta_parse the ctx
	// (read-only) and report isRTT + raw op/pt/tr PolyParam counts + sprite-filtered
	// count + body-band(y200-460) count + the video-frame counter (0x8C3496B0). This
	// PROVES (a) two STARTRENDERs/frame and (b) that the DROPPED one carries the body
	// quads (op~265/tr~2024, bodyBand>0) while the surviving one is HUD (op~573/tr~42,
	// bodyBand==0). We must SEE this before trusting the capture. Throttled lightly.
	ta_parse(ctx, true);                       // read-only: builds ctx->rend (norend's call)
	PassStats ps; memset(&ps, 0, sizeof ps);
	collectScreenQuads(ctx->rend, &ps);
	ps.isRTT = ctx->rend.isRTT ? 1 : 0;
	// CHARQ FIX (2026-06-09): the character pass is the one whose kept sprite quads include
	// REAL body-region quads (center y in [200,460], excludes HUD). The old `tr>=500` gate
	// NEVER passed on this build/state (the real pass has tr=40-52; the tr=2024 figure was
	// stale). Gate on realBody instead so the JSONL writes whenever the body is on screen.
	bool isCharacterPass = (ps.realBody >= CHARQ_REALBODY_MIN);
	u32  vframe = addrspace::read32(0x8C3496B0);

	static unsigned long s_charpassN = 0;
	if ((s_charpassN++ % 30) == 0) {
		fprintf(stderr,
			"[CHARPASS] vframe=%u isRTT=%d op=%d pt=%d tr=%d sprite=%d bodyBand=%d realBody=%d -> %s "
			"(STARTRENDER, pre-QueueRender)\n",
			vframe, ps.isRTT, ps.op, ps.pt, ps.tr, ps.sprite, ps.bodyBand, ps.realBody,
			isCharacterPass ? "CHARACTER(would-be-DROPPED body pass)" : "hud/composite");
	}

	// STEP 3 — route the CHARACTER pass's quads into the Oracle capture path. We call
	// mc_oracle_frameFlush, which re-ta_parses (cheap, idempotent), runs the SAME
	// discriminator, attributeScreenQuads (the bbox/screen_xy fix + opaque-filter fix),
	// populates s_objs[]/s_screen[] + the CHARQ accessor (mc_oracle_objects/quadObjMap),
	// and emits the JSONL keyed on vframe. It is per-vframe deduped (s_lastEmittedVframe),
	// so calling it for both the HUD pass and the character pass emits exactly once — on
	// the character pass. We pass vframe as the frame id; frameFlush overwrites it with
	// 0x8C3496B0 internally anyway, so passing it here is purely informative.
	//
	// OBJECT-TABLE TIMING: the OBJ_BEGIN/SAT_BEGIN screen_xy enrich happens in the SH4
	// block-entry hooks during the SH4 frame (loc_8c03093c writes node+0xE0/+0xE4 DURING
	// the draw walk). rend_start_render fires AFTER the SH4 draw walk completes for this
	// frame's TA list, so node screen_xy is the CURRENT frame's post-transform position —
	// attributeScreenQuads re-reads it fresh (oracle_hook.cpp:1763-1771). The object table
	// is therefore current and available here, same as it is at serverPublish.
	mc_oracle_frameFlush(ctxv, vframe);
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
	// CHARQ: when MAPLECAST_CHARQ is set the flush is driven from rend_start_render's
	// pre-QueueRender hook (mc_oracle_charPassCapture) for the CHARACTER pass, so it must
	// be active even if MAPLECAST_FRAME_ORACLE_HOOK is unset. (In practice both are set per
	// the deploy spec, but make CHARQ self-sufficient.)
	bool frameOracleActive = (getenv("MAPLECAST_FRAME_ORACLE_HOOK") != nullptr) || mc_probeEnabled || mc_charqEnabled;
	if (!frameOracleActive) { s_nobj = 0; s_nquad = 0; s_nscreen = 0; s_nr2 = 0; s_charqNobj = 0; s_charqNmap = 0; return; }

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
	PassStats ps; memset(&ps, 0, sizeof ps);
	bool isCharacterPass = false;
	if (ctx && inMatch) {
		ta_parse(ctx, true);
		collectScreenQuads(ctx->rend, &ps);
		ps.isRTT = ctx->rend.isRTT ? 1 : 0;

		// === CHARQ Phase-1 FIX (1): discriminate the CHARACTER pass from the
		// HUD/composite pass. MVC2 ships multiple TA passes per video frame; only the
		// character pass carries the bodies (the large tr list + body-band coords). The
		// HUD/composite pass is a tiny op/tr list of top-of-screen sprites + the stage
		// backdrop with ZERO body. We key on the parsed-poly shape (proven by the live
		// STAF parse: character pass = op~265 tr~2024, ~281 body-band y240-439 coords):
		//   character pass  <=>  large tr list AND at least one kept body-band sprite quad.
		// isRTT is LOGGED (below) to CONFIRM which pass the body lives in before we trust
		// the heuristic; the heuristic itself does NOT depend on isRTT (MVC2's character
		// sprites are screen-space TR quads in the on-screen pass, not an RTT composite —
		// the prior expert verified this is NOT an RTT/ta_parse-arg issue).
		// Threshold: the character pass tr list is ~2000 polys (STAF: op~265 tr~2024);
		// every other pass (HUD/composite/transition) has tr <= ~204. tr>=500 cleanly
		// separates them. AND require at least one kept body-band sprite quad so a
		// large non-character tr list (e.g. an effects-heavy super) still needs the
		// body region populated. Both must hold.
		// CHARQ FIX (2026-06-09): gate on REAL body-region quads (center y in [200,460],
		// excludes HUD), NOT `tr>=500` (which NEVER passed on this build — the real pass
		// has tr=40-52). The JSONL now writes whenever the body is actually on screen.
		isCharacterPass = (ps.realBody >= CHARQ_REALBODY_MIN);

		// === CHARQ Phase-1 FIX (1, verify): per-pass log of isRTT + op/pt/tr + bodyBand +
		// realBody, throttled, so the operator can CONFIRM exactly which pass carries the
		// body before we key on it. (The expert flagged this as the one thing to verify.)
		static unsigned long s_passLogN = 0;
		if (inMatch && (s_passLogN++ % 30) == 0) {
			fprintf(stderr,
				"[ORACLE-PASS] vframe=%u localFrame=%u isRTT=%d op=%d pt=%d tr=%d "
				"sprite=%d bodyBand=%d realBody=%d -> %s\n",
				addrspace::read32(0x8C3496B0), frame, ps.isRTT, ps.op, ps.pt, ps.tr,
				ps.sprite, ps.bodyBand, ps.realBody, isCharacterPass ? "CHARACTER" : "hud/composite");
		}

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

	// Capacity guard so a long session can't fill /dev/shm. TRUNCATE-AND-REWIND:
	// when the file crosses the cap we re-open it with O_TRUNC ("w") and continue
	// appending fresh frames from byte 0. This keeps the JSONL small AND always
	// current (recent frames only) — exactly what an aligned (quads+VRAM) tail grab
	// needs. The previous behavior (set full=true, stop forever at 64 MiB) froze the
	// on-disk tail ~96s stale while the live hook kept advancing. Default cap 16 MiB,
	// env-overridable via MAPLECAST_ORACLE_JSONL_CAP (bytes).
	static const long ORACLE_CAP = []{
		const char* v = getenv("MAPLECAST_ORACLE_JSONL_CAP");
		if (v) { long c = atol(v); if (c >= (1L << 20)) return c; }   // floor 1 MiB
		return 16L * 1024 * 1024;
	}();
	static FILE* of = nullptr;
	static long  ow = 0;

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

	// === CHARQ Phase-1 FIX (2): key the emit on the SH4 VIDEO-frame counter
	// (0x8C3496B0, work.asm) — NOT _localFrameNum, which ticks per TA PASS. We emit
	// the JSONL/CHARQ snapshot ONCE per video frame, on the CHARACTER pass (the pass
	// that actually carries the bodies). The HUD/composite pass that also fires a
	// serverPublish for the same video frame is skipped for emit (it has zero body),
	// but its publishCharqSnapshot keeps the accessor coherent. This stops the Oracle
	// from latching the HUD pass and overwriting the body capture with an empty frame.
	u32 vframe = addrspace::read32(0x8C3496B0);
	static u32  s_lastEmittedVframe = 0xFFFFFFFFu;
	bool        alreadyEmittedThisVframe = (vframe == s_lastEmittedVframe);

	// Emit ONLY on the character pass (bodies present), once per video frame, in-match,
	// when something was captured. Non-character passes (HUD/composite) and duplicate
	// character passes for the same video frame fall through to the snapshot-only path.
	bool doEmit = inMatch && isCharacterPass && !alreadyEmittedThisVframe
	           && !(s_nobj == 0 && s_nquad == 0 && s_nscreen == 0);

	// Use the VIDEO-frame counter as the JSONL "frame" id so a downstream reader can
	// dedup/align per video frame regardless of how many TA passes a frame had.
	frame = vframe;

	if (!doEmit) {
		// Not the body pass (or already emitted this video frame, or off-match/empty):
		// keep the CHARQ accessor coherent but DON'T write JSONL / DON'T clobber the
		// body capture. Publish the snapshot only if this pass actually has objects
		// (the character pass), else leave the last good snapshot in place so the HUD
		// pass doesn't zero out the body snapshot the character pass just published.
		if (isCharacterPass || s_nobj > 0)
			publishCharqSnapshot();
		s_nobj = 0; s_nquad = 0; s_nscreen = 0; s_nr2 = 0; return;
	}
	s_lastEmittedVframe = vframe;

	{
		if (!of) {
			// First open: O_TRUNC ("w") so a stale 64 MiB file from a previous run is
			// discarded — the tail must reflect THIS run's live frames immediately.
			of = fopen("/dev/shm/mc_oracle_hook.jsonl", "w");
			ow = 0;
			if (of) {
				setvbuf(of, nullptr, _IOFBF, 1 << 16);   // big buffer; we fflush per emit
				fprintf(stderr, "[ORACLE-HOOK] first jsonl flush — frame=%u objs=%d quads=%d "
				                "cap=%ldMiB (truncate-and-rewind) -> /dev/shm/mc_oracle_hook.jsonl\n",
				        frame, s_nobj, s_nquad, ORACLE_CAP >> 20);
			} else
				fprintf(stderr, "[ORACLE-HOOK] FAILED to open /dev/shm/mc_oracle_hook.jsonl "
				                "(errno path) — captured objs=%d but cannot write\n", s_nobj);
		}
		// TRUNCATE-AND-REWIND at the cap: rewind to byte 0 and overwrite. freopen("w")
		// re-opens the SAME path with O_TRUNC; the on-disk file shrinks to 0 then
		// regrows with fresh frames, so the tail is never more than ~cap bytes stale
		// and NEVER freezes.
		if (of && ow >= ORACLE_CAP) {
			of = freopen("/dev/shm/mc_oracle_hook.jsonl", "w", of);
			ow = 0;
			if (of) {
				setvbuf(of, nullptr, _IOFBF, 1 << 16);
				fprintf(stderr, "[ORACLE-HOOK] jsonl cap %ldMiB reached — rewound "
				                "(truncate) to keep the tail live (frame=%u)\n",
				        ORACLE_CAP >> 20, frame);
			}
		}
		if (of) {
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
						"\"vram_addr\":\"0x%08X\",\"tcw\":\"0x%08X\",\"tsp\":\"0x%08X\","
						"\"pcw\":\"0x%08X\",\"isp\":\"0x%08X\",\"fmt\":%d,"
						"\"tex_wh\":[%d,%d],\"vq\":%d,\"blend\":[%d,%d]}",
						firstQ ? "" : ",",
						(int)q.x,(int)q.y,(int)q.w,(int)q.h,
						q.uMn,q.uMx,q.vMn,q.vMx, q.zMn,q.zMx,
						q.vramAddr, q.tcw, q.tsp, q.pcw, q.isp, q.fmt, q.tw, q.th, q.vq,
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
			fflush(of);   // per-emit flush: the on-disk tail tracks the live frame
			// Per-flush wrote-bytes (sampled with the periodic flush log above).
			if ((s_flushCalls % 120) == 1)
				fprintf(stderr, "[ORACLE-HOOK] flush wrote=%ld bytes total=%ld (frame=%u objs=%d screenQuads=%d)\n",
				        ow - owBefore, ow, frame, s_nobj, s_nscreen);
		}
	}

	// CHARQ snapshot — publish THIS frame's object identities + the kept-sprite-quad
	// -> object map for the CHARQ emit block (serverPublish, after this flush). Built
	// here, just before the per-frame statics reset, so the accessors return valid
	// data for the same serverPublish call. s_screen[] index == kept-sprite-quad
	// ordinal (collectScreenQuads order), so the map is a direct copy of .obj.
	publishCharqSnapshot();

	s_nobj = 0; s_nquad = 0; s_nscreen = 0; s_nr2 = 0;
}

// Copy the per-frame s_objs[] identities + the s_screen[] ordinal->obj map into the
// CHARQ snapshot statics. Cheap: a flat copy of <=256 objs + <=4096 ints. Called at
// the end of frameFlush (snapshot survives the per-frame reset). Mask sprite_id to
// 0x7FFF here; resolve cid (body=node+0x1, satellite=ownerCid) so CHARQ reads it raw.
static void publishCharqSnapshot()
{
	int n = s_nobj; if (n > MAX_OBJS) n = MAX_OBJS;
	for (int i = 0; i < n; i++) {
		const Obj& o = s_objs[i];
		CharqObj& c = s_charqObjs[i];
		c.node        = o.node;
		c.sprite_id   = (o.sprite_id >= 0) ? (o.sprite_id & 0x7FFF) : o.sprite_id;
		c.isSatellite = o.isSat;
		c.ownerSlot   = o.ownerSlot;
		c.ownerCid    = o.ownerCid;
		c.screen_x    = o.sx;
		c.screen_y    = o.sy;
		// cid: bodies carry character_id at node+0x1; satellites have none on the node
		// (it's the OWNER's), so report the owner cid for sats.
		c.cid = o.isSat ? o.ownerCid : (int)(u8)addrspace::read8(o.node + OFF_CHAR_ID);
	}
	s_charqNobj = n;

	int m = s_nscreen; if (m > MAX_SCREEN) m = MAX_SCREEN;
	for (int k = 0; k < m; k++) s_charqMap[k] = s_screen[k].obj;
	s_charqNmap = m;
}

const CharqObj* mc_oracle_objects(int* outCount)
{
	if (outCount) *outCount = s_charqNobj;
	return s_charqNobj ? s_charqObjs : nullptr;
}

const int* mc_oracle_quadObjMap(int* outCount)
{
	if (outCount) *outCount = s_charqNmap;
	return s_charqNmap ? s_charqMap : nullptr;
}

}
