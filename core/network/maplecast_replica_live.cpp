/*
	MapleCast Render-Replica LIVE feed (Phase 4c) — see maplecast_replica_live.h.

	Streams the MCRR render read-set (docs/RENDER-REPLICA-RECORDING-FORMAT.md) off
	the live headless over a loopback WebSocket so a browser can drive the off-SH4
	render_frame() on the moving game. READ-ONLY w.r.t. guest state, gated OFF by
	default, and structured so the SH4/render thread never blocks on the socket.

	THREADING MODEL (the determinism + budget contract):
	  SH4/render thread (onRenderFrame, inside mc_oracle_charPassCapture):
	    - membership test: one relaxed atomic (_clientCount). 0 ⇒ return now.
	    - build the static prefix ONCE (lazy, first armed in-match frame) into a
	      heap buffer via addrspace reads (alias-safe) + the resident vram/pvr arrays.
	    - per frame: memcpy the DYNAMIC regions into one of two staging buffers,
	      then publish {ptr,size} to the WS thread under _pubMutex. If the WS thread
	      hasn't drained the previous frame we OVERWRITE it (drop-old). We NEVER
	      block on the socket and NEVER touch guest memory for writing.
	  WS thread (websocketpp asio, _wsThread):
	    - accept loopback clients; on open, send the cached static prefix (or, if
	      not built yet, mark the client "needs prefix" and the next published frame
	      triggers the prefix send first).
	    - a sender loop (condvar) wakes on each publish, zstd's the dynamic payload
	      into a ZCST envelope, broadcasts the FRAME RECORD to all open conns.

	All of this is compiled in unconditionally but is completely inert unless
	MAPLECAST_REPLICA_LIVE is set (init() returns before creating any thread).
*/
#include "maplecast_replica_live.h"
#include "maplecast_compress.h"          // MirrorCompressor / ZCST envelope
#include "maplecast_oracle_hook.h"       // HudQuad + mc_oracle_hudQuads (HUDQ tail)

#include "hw/sh4/sh4_mem.h"              // addrspace::read*, mem_b, RAM_SIZE
#include "hw/pvr/pvr_mem.h"              // vram, VRAM_SIZE
#include "hw/pvr/pvr_regs.h"             // pvr_regs[], pvr_RegSize
#include "types.h"                       // RAM_SIZE / VRAM_SIZE macros

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace maplecast_replica_live
{

using RlServer  = websocketpp::server<websocketpp::config::asio>;
using RlConnHdl = websocketpp::connection_hdl;

// ===========================================================================
// Module state
// ===========================================================================

static bool                _armed   = false;     // env was set at init (immutable after init)
static int                 _port    = 7212;
static std::atomic<bool>   _active{false};        // WS thread is running

static RlServer            _ws;
static std::thread         _wsThread;
static std::set<RlConnHdl, std::owner_less<RlConnHdl>> _conns;
static std::mutex          _connMutex;
// Number of connected clients — the SH4-thread membership test. Relaxed loads on
// the hot path; the only writers are the WS thread's open/close handlers.
static std::atomic<int>    _clientCount{0};

// Region tables (built once on the first armed in-match frame).
struct Reg { u32 addr; u32 len; char tag[8]; };
static std::vector<Reg>    _staticRegs;
static std::vector<Reg>    _dynRegs;
static bool                _tablesBuilt = false;
static u32                 _dynTotal    = 0;       // sum of dynamic region lens (payload size)

// The STATIC PREFIX (header + tables + VRAM + PVR + 16MB RAM), built once, then
// zstd'd into a ZCST envelope cached for every connecting client.
static std::vector<uint8_t> _prefixZcst;           // ready-to-send compressed bytes
static std::atomic<bool>    _prefixReady{false};
static std::mutex           _prefixMutex;          // guards build of _prefixZcst

// ---------------------------------------------------------------------------
// ON-CHANGE DYNAMIC GFX (the static-GFX-gap fix, re_kb finding:replica_live_static_gfx_fix).
// The static prefix ships only the bodies' GFX that were resident when the prefix was built
// (ONCE). A char whose art loads AFTER that — e.g. a client that connected pre-art-load — has
// its GFX2 cell records FROZEN/stale in the client RAM image, so the transpiled walker
// (loc_8c0344d4) reads garbage cell records and emits a grid. FIX: every frame, walk the slot
// table, and for each active BODY node ship its GFX1(+0x15C)/GFX2(+0x160) regions at their REAL
// extent (GFX1 ~1.1MB, GFX2 ~64KB — read from the offset table; see gfxExtentFromBase) as a
// VARIABLE dynamic tail — but ONLY when that (base,content-signature) has not been shipped before
// (covers both the never-shipped case and a re-load that changed the bytes). The signature is a
// cheap fold over the FULL region so a static (already-fresh) body costs nothing after its first
// send. Wire cost: a body's full GFX1+GFX2 = ~1.3MB raw -> compressed ON (re)LOAD only, ~0 steady.
struct GfxBase { u32 base; u32 sig; };                 // base addr (page-aligned) + content sig
static std::vector<GfxBase> _gfxShipped;               // bases+sigs already sent to all clients
static std::atomic<bool>    _gfxResendAll{false};      // set on new-connect: re-ship all GFX once
static inline u32 rd32(u32 g);                         // fwd-decl (defined below) — used by gfxSig
static inline u8  rd8 (u32 g);

// gfxSig — content signature over a shipped GFX region. The signature drives the on-change
// re-ship: if the sig changed since last shipped, the region (and its self-modified dispatch
// head) is re-sent. THE STORM-SCRAMBLE ROOT-CAUSE FIX (re_kb 32
// finding:replica_storm_scramble_is_static_gfx2_self_modify_plus_torn_capture):
//
//   The engine SELF-MODIFIES the GFX2 cell-record DISPATCH TABLE in place per animation
//   sub-frame — it rewrites GFX2[(sid&0x7FFF)*4] (one u32) to point at the current pose's
//   cell records (Storm body sid 0x42: disc 0x1b04 -> live 0x1b44, the same region byte
//   trueBase+0x108 / pageBase-rel 0x788). The OLD sparse-64 fold sampled only every 64th
//   byte (rd32 at b=0,64,128,...) and PROVABLY never read 0x788 (1928 = 64*30.125, falls
//   between samples) — so a single-u32 dispatch mutation never changed the sig and GFX2 was
//   frozen at the disc default 0x1b04 -> the client walker (loc_8c0344d4: r11 = GFX2 +
//   GFX2[(sid&0x7FFF)*4]) read the STALE entry -> wrong part list / sels -> scramble.
//
//   FIX: for GFX2, DENSELY fold the WHOLE dispatch-table HEAD (the front n*4 bytes, where
//   entry[0]==n*4 and n is the selector count — ~1174 entries => ~4.7KB) so EVERY u32
//   dispatch entry contributes to the sig. Any per-sub-frame self-modify now flips the sig
//   and re-ships the (small, ~4.7KB) GFX2 region. The body LZSS pixels never self-modify, so
//   the rest of GFX2 / all of GFX1 keep the cheap sparse-64 fold.
static u32 gfxSig(u32 base, u32 len, bool isGfx2 = false)
{
	u32 h = 2166136261u;
	if (isGfx2) {
		// DENSE dispatch-head fold: GFX2 true base = node+0x160 (NOT necessarily == base, which is
		// page-aligned-DOWN). Read entry[0] at the TRUE base to get n; fold every u32 of the head.
		// The dispatch head can sit 0x8C0 into the page (page-aligned base is < true base); we don't
		// know the true base here, so locate it: the descriptor uses trueBase = original gfxPtr, and
		// (trueBase - base) is the in-page offset. Recover it by scanning forward from base for the
		// first valid table-head (entry0 == n*4, n in 1..20000) within the first 0x1000 bytes.
		u32 trueBase = base, n = 0;
		for (u32 d = 0; d < 0x1000u; d += 4) {
			u32 e0 = rd32(base + d), nn = e0 >> 2;
			if (nn != 0 && nn <= 20000u && e0 == (nn << 2)) { trueBase = base + d; n = nn; break; }
		}
		if (n) {
			u32 headLen = n * 4u;                     // the dispatch table head (every selector entry)
			for (u32 b = 0; b < headLen; b += 4) {    // DENSE: every u32 dispatch entry (catches the
				h ^= rd32(trueBase + b); h *= 16777619u;  // per-sub-frame self-modify, e.g. 0x108)
			}
		}
		// Plus the sparse-64 fold across the rest (LZSS-stored cell records change only on re-load).
	}
	for (u32 b = 0; b < len; b += 64) {               // sparse sample (every 64B) across the FULL
		h ^= rd32(base + b); h *= 16777619u;          // region — catches a content change / re-load
	}
	return h;
}
static bool gfxAlreadyShipped(u32 base, u32 sig)
{
	for (auto& g : _gfxShipped) if (g.base == base) return g.sig == sig;
	return false;
}

// ---------------------------------------------------------------------------
// GFX REGION EXTENT — ship each GFX1/GFX2 region at its REAL size, not a blanket 0x20000.
// (root-cause fix, re_kb finding:replica_live_gfx_extent. The fixed 128KB truncated the asset:
// GFX1 reaches ~1.1MB on the live wire — maxq_86.mcrr GFX1 true base 0x0C810000 max-offset 0x115760.)
//
// FORMAT (byte-exact, extract_gfx1_atlas.py / rip_gfx2_assembly.py; both segments share it):
//   front = u32-LE OFFSET TABLE; entry[0] = n*4 so n = entry[0]>>2 = #selectors; entry[i] = byte
//   offset (from the TRUE base) of selector i's block. The table head sits at the TRUE base
//   (node+0x160 / node+0x15C), which is NOT necessarily page-aligned (GFX2 true base was 0x..8C0,
//   0x8C0 into its page) — so the extent MUST be read at `base`, never at `base & ~0xFFF`.
//   GFX2 block @ off: [u16 count][count*8B records] -> block size = 2 + count*8 (EXACTLY headered).
//   GFX1 block @ off: [lw][lh][sw][sh] + LZSS stream -> compressed length NOT in the header, so we
//   bound the LAST block by the largest consecutive-offset GAP observed across the roster (0x234D)
//   with headroom -> GFX1_LAST_SLACK. Roster facts: GFX2 max last-tail 0x136 / max gap 0x4C2;
//   GFX1 max last-tail 0x1E3 / max gap 0x234D.
//
// Returns the EXTENT measured from `trueBase` (bytes from trueBase to region end), or 0 if the
// table head doesn't validate (caller falls back to the old 0x20000 so we never under-ship blind).
static const u32 GFX1_LAST_SLACK = 0x4000u;     // > worst compressed block (0x234D) + tail
static u32 gfxExtentFromBase(u32 trueBase, bool isGfx2)
{
	u32 e0 = rd32(trueBase);
	u32 n  = e0 >> 2;
	// table-head sanity: entry0 == n*4 and a plausible selector count.
	if (n == 0 || n > 20000u || e0 != (n << 2)) return 0;
	u32 maxoff = 0;
	for (u32 i = 0; i < n; i++) {
		u32 o = rd32(trueBase + i * 4);
		if (o > 0x600000u) return 0;             // offset out of any sane region -> bail to fallback
		if (o > maxoff) maxoff = o;
	}
	if (isGfx2) {
		u32 cnt = (u32)((rd8(trueBase + maxoff + 1) << 8) | rd8(trueBase + maxoff)); // u16-LE count
		return maxoff + 2u + cnt * 8u;           // exact: last cell-record block
	}
	return maxoff + GFX1_LAST_SLACK;             // GFX1: bound the final compressed part
}

// Page-aligned ship descriptor for a GFX region whose engine pointer is `gfxPtr` (node+0x15C/0x160).
// base = page-aligned-down (the splat target the client/render_frame address against); len covers
// from that page base through the true region end, page-rounded up. Falls back to 0x20000 only if
// the header doesn't validate (keeps the old behavior for an unexpected layout instead of a 0-len).
static void gfxShipDescriptor(u32 gfxPtr, bool isGfx2, u32& outBase, u32& outLen)
{
	u32 trueBase = gfxPtr;
	u32 pageBase = trueBase & ~0xFFFu;
	u32 ext = gfxExtentFromBase(trueBase, isGfx2);
	outBase = pageBase;
	if (ext == 0) { outLen = 0x20000u; return; }            // fallback (never blind-truncate to 0)
	u32 fromPage = (trueBase - pageBase) + ext;             // bytes from page base to region end
	outLen = (fromPage + 0xFFFu) & ~0xFFFu;                 // page-round up
}
static void gfxMarkShipped(u32 base, u32 sig)
{
	for (auto& g : _gfxShipped) if (g.base == base) { g.sig = sig; return; }
	_gfxShipped.push_back({ base, sig });
}

// ---------------------------------------------------------------------------
// ON-CHANGE PVR PALETTE (the palette-gap fix, re_kb finding:replica_live_palette_gap).
// The static prefix ships pvr_regs[] (incl PALETTE_RAM @ pvr_regs+0x1000, 1024 ARGB entries,
// 64 banks of 16 — the skin-bank layout) EXACTLY ONCE (buildPrefixLocked). The guest writes each
// active char s 16-color palette bank into PALETTE_RAM via pvr_WriteReg at CHARACTER-LOAD time
// (sb_mem.cpp). Any bank written AFTER the prefix was cached — a tag-in, or a client that connected
// before that art loaded — is FROZEN at the snapshot value (zero -> all-transparent -> the body
// renders BLACK). This is the GFX static-gap s twin (finding:replica_live_static_gfx_gap) in the
// palette domain. CONFIRMED: bank 32 (P1C2, entries 512-527, pvr off 0x1800) renders black when the
// P1C2 palette is written post-prefix; populated banks are correct only because _satlive.mcrr s
// prefix happened to be built after all 6 chars  palettes loaded (timing-dependent, not range).
//
// FIX (server-only, no client palette source today reads per-frame): ship the LIVE 32KB pvr_regs
// block as an ON-CHANGE dynamic tail (tag implicit by position) whenever its content signature
// changes vs the last shipped value. Steady-state cost = 0 (the block is constant once palettes are
// loaded). The client (replay.html liveApplyFrame) refreshes this.pvr from this tail before
// pane.render -> updatePalette(this.pvr) re-uploads the fresh banks. Wire cost: 32KB raw -> ~a few
// KB zstd, ONLY on a palette write (char load / tag-in / skin override), ~0 steady.
static u32  _pvrPalSig      = 0;            // last-shipped pvr_regs content signature
static bool _pvrPalEverSent = false;        // have we shipped a palette tail at least once?
static std::atomic<bool> _pvrPalResend{false}; // set on new-connect: re-ship the palette once

// Signature over the palette-relevant pvr_regs span: PAL_RAM_CTRL (0x108) + the full 4KB
// PALETTE_RAM (0x1000..0x1FFF). Those are the only bytes updatePalette() consumes; folding just
// them keeps the sig cheap and means a non-palette pvr reg write does not trigger a needless reship.
static u32 pvrPalSig()
{
	u32 h = 2166136261u;
	h ^= *(const u32*)&pvr_regs[0x108]; h *= 16777619u;     // PAL_RAM_CTRL
	for (u32 o = 0x1000; o < 0x2000; o += 4) {             // PALETTE_RAM (1024 entries)
		h ^= *(const u32*)&pvr_regs[o]; h *= 16777619u;
	}
	return h;
}

// Double-buffered DYNAMIC staging + single-slot publish (drop-old).
static std::vector<uint8_t> _dynBuf[2];
static int                  _dynWhich = 0;
static std::mutex           _pubMutex;
static std::condition_variable _pubCv;
static const uint8_t*       _pubPtr  = nullptr;     // points into _dynBuf[*]
static size_t               _pubLen  = 0;
static bool                 _pubHasFrame = false;
static bool                 _pubQuit = false;

// Compressors: one for the prefix (level 3, one-shot), one for the per-frame
// dynamic payload (level 1, fast). Both live on / are used only by code paths
// that serialize through their own mutex (MirrorCompressor has an internal mtx).
static MirrorCompressor     _prefixComp;
static MirrorCompressor     _frameComp;
static bool                 _compInit = false;

// MCRR / FRMx magics (LE on the wire) — see the format doc.
static constexpr u32 MCRR_MAGIC = 0x5252434Du;     // "MCRR"
static constexpr u32 FRMX_MAGIC = 0x784D5246u;     // "FRMx"
static constexpr u32 HUDQ_MAGIC = 0x48554451u;     // "HUDQ" (HUD-TA tail magic, LE)

// In-match flag + video-frame counter (same gate the Oracle uses).
static constexpr u32 IN_MATCH_ADDR = 0x8C289624;
static constexpr u32 VFRAME_ADDR   = 0x8C3496B0;

static inline u32 rd32(u32 g) { return addrspace::read32(g); }
static inline u8  rd8 (u32 g) { return addrspace::read8(g); }
static inline bool isRam(u32 g) { return (((g >> 24) & 0x7F) == 0x0C) && g != 0; }

// ===========================================================================
// Region-table construction (SH4 thread, once). Mirrors the experiment branch's
// MAPLECAST_REPLICA_RECORD AddD/AddS lists and the read-set in re_kb
// finding:render_replica_readset / docs/RENDER-REPLICA-RECORDING-FORMAT.md.
// ===========================================================================

static void pushReg(std::vector<Reg>& v, u32 a, u32 l, const char* w)
{
	Reg r; r.addr = a; r.len = l; memset(r.tag, 0, 8);
	strncpy(r.tag, w, 7);
	v.push_back(r);
}

static void buildTables()
{
	if (_tablesBuilt) return;

	// ---- DYNAMIC: shipped every frame (whole regions, multi-character-safe) ----
	auto D = [&](u32 a, u32 l, const char* w) { pushReg(_dynRegs, a, l, w); };
	D(0x8C2895E0, 0x10,        "slot_cnt");      // slot-table count array (16 layers)
	D(0x8C287DE0, 16u*0x180u,  "slot_ptr");      // slot-table ptr arrays
	// GLOBAL MATCH STATE (page 649) — 32B window over the 0x8C289620 block so the
	// client gets stage_id (0x8C289638) for the Phase-3 STAGE background pass (pick
	// which cached STGxx to render), plus in_match(0x624)/round(0x62B)/timer(0x630).
	// stage_anim_timer (0x8C1F9D80) is already shipped in the "arena" region below.
	// PHASE 3 HUD: the window also covers the HUD globals at the SAME addresses the production
	// GSTA wire reads (core/network/maplecast_gamestate.cpp): p1/p2_meter_fill u16 @0x646/0x648,
	// p1/p2_meter_level u8 @0x64A/0x64B, p1/p2_combo u16 @0x670/0x672. To reach combo (last byte
	// 0x673) the window must span 0x289620..0x289673 = 0x54 bytes; rounded to 0x60 (ends 0x28967F,
	// still inside page 649). Per-char health(+0x420)/red_health(+0x424)/char_id(+0x001) already
	// ride the "char_str" region below. (re_kb finding:replica_live_hud.)
	D(0x8C289620, 0x60,        "gstate");        // in_match/round/timer/stage_id + meter/combo (HUD)
	// BATTLE STATE (work.asm:9 "8c2895F0 Battle State"): the round-flow state machine — round
	// intro / fight / KO / win-screen / continue transitions. Oracle-confirmed live: 0x8C2895F0
	// reads 0x04 (in-round) during a live match, and the slot-count array just below (0x8C2895E0)
	// cycles 0x03->0x04 across the transition. Window 0x8C2895C0 + 0x60 = 0x2895C0..0x28961F
	// covers Battle State 0x2895F0 with headroom (and butts up against the gstate region at
	// 0x289620). Lets the client gate HUD/intro/KO overlays on the real round phase instead of
	// guessing from health/timer. +0x60 bytes/frame, zstd-trivial. (re_kb finding:replica_live_render_field_gaps.)
	D(0x8C2895C0, 0x60,        "battle");        // Battle State 0x8C2895F0 (round-flow phases)
	// PER-SIDE WIN COUNT is char+0x540 num_wins (pl_mem.asm:301: num_wins 0x540 / num_lose 0x541 /
	// num_draw 0x542 / handicap_level 0x543, all byte). 0x540 < stride 0x5A4, so it ALREADY rides
	// the char_str region below — no extra wire. Client draws win stars from char[slot]+0x540.
	// (Oracle-confirmed live: P1C1+0x540=0 wins, +0x543=2 handicap — the 0x02 there is handicap,
	// NOT a win.) (re_kb finding:replica_live_render_field_gaps.)
	D(0x8C268340, 6u*0x5A4u,   "char_str");      // P1C1..P2C3 char structs (incl +0x540 num_wins)
	// SATELLITE OBJECT POOL (capes/projectiles/drones/effects/extra-limbs) — the
	// out-of-char-struct BODY nodes the slot-walk loc_8c0308c2 also renders via
	// render_object_full (loc_8c03093c) + the body walker loc_8c0344d4. Without these
	// their node+0xDC..+0xF0 anchor/scale + node+0x15C/0x160 GFX read STALE -> missing
	// or misplaced sprites the moment an assist/projectile/super fires (re_kb
	// finding:render_frame_positions_validated open_risk; confirmed firing live: frame
	// 708746 nodes 0x8C26AFC4/0x8C26B194/0x8C275684 = pool idx 3/4/58, all on the
	// 0x8C26AA54 + N*0x1D0 grid, N in [0,256)). Pool base/stride/count cited to
	// bank04.asm:11601-11756 loc_8c044dce: base const loc_8c044ec8=0x8C26AA54,
	// stride loc_8c044ea4=0x1D0, span loc_8c044ed8=0x1D000 => 256 nodes. Ship the WHOLE
	// pool: render_object_full reads scattered fields across +0x24..+0x184 per node, so
	// a tight per-node window is fragile — the full 0x1D000 contiguous region is the
	// correct, still-bounded ship (the GFX those nodes point at already lives in the
	// once-shipped 16MB static RAM image).
	D(0x8C26AA54, 0x1D000,     "objpool");       // 256 * 0x1D0 satellite object pool
	D(0x8C1F9D80, 0x20,        "arena");         // arena-control globals
	D(0x8C1F9F9C, 0x1800,      "tiledesc");      // per-frame tile-descriptor scratch
	                                             // (live descriptor table spans 5135B —
	                                             //  0x200 truncated it; 0x1800 covers it)
	D(0x8C2D6AD8, 0xC0,        "cam_mat");       // camera matrices M2/M1
	D(0x8C26A510, 0x40,        "camZ");          // camera-Z scale block
	D(0x8C26823C, 0x04,        "ggp_ptr");       // GameGlobalPointer
	// ggp_acc: the *(GameGlobalPointer) global-accum struct. EXTENDED 0x40 -> 0x60 to reach
	// the "Game mode" block at 0x8C26828C (work.asm:20-24: char-unlocks 0x270 / color-unlocks
	// 0x278 / Game mode 0x28C / stage-unlocks 0x291 / text-name-flag 0x298). The Game-mode byte
	// 0x8C26828C is the TIME-MODE / match-mode selector — needed so the client renders the HUD
	// timer as "infinity" in an infinite-time match instead of the held-99 heuristic (Oracle-
	// confirmed live: in_match=1 yet timer 0x8C289630 frozen at 0x63=99, both chars HP=144,
	// = an infinite-time/idle match). 0x40 ended at 0x28627F (short of 0x28C); 0x60 reaches
	// 0x29F. +0x20 bytes/frame, zstd-trivial (stable). (re_kb finding:replica_live_render_field_gaps.)
	D(0x8C268240, 0x60,        "ggp_acc");       // *(GGP) global-accum struct + Game mode @0x28C
	D(0x8C26A974, 0x100,       "rparam");        // per-char render-param table
	D(0x8C2DAD30, 0x40,        "tab_ptr");       // rectab/idxtab pointer pair window
	D(0x8C2AA4C0, 0x10,        "rmode");         // global render-mode word
	{   u32 idxtab = rd32(0x8C2DAD3C), rectab = rd32(0x8C2DAD4C);
		// These two pointers are resolved at table-build time. They are stable for
		// the match (arena base), so capturing them here is correct for the stream.
		if (isRam(idxtab)) D(idxtab, 0x2000, "idxtab");
		// rectab: max record index hit 461/1024 with 2 bodies; 0x8000 truncated the
		// table on busier frames. 0x10000 gives headroom for a 3rd/4th body.
		if (isRam(rectab)) D(rectab, 0x10000, "rectab");
	}

	// PHASE 5 — PURE-STATE TEXTURES: the body sprite TEXTURE band is NO LONGER shipped.
	// The previous "bodytex" shortcut (D(0x410000, 0x50000) = ~320KB/frame of decoded VRAM
	// pixels) was off-thesis: it streamed pixels instead of state. The CLIENT now reconstructs
	// each active body part's sprite from the shipped-once compressed GFX (GFX1/GFX2 in the
	// static prefix + the 16MB RAM image) + the per-frame sprite_id (in the char structs we
	// already ship), decoding via the validated LZSS (bank03 loc_8c0354c0) and writing the
	// VERBATIM (twiddled-storage) output to its TCW VRAM address — see web/render-replica/
	// body_decoder.mjs (ensureBodyTextures). Byte-exact vs engine VRAM (decodeA == VRAM,
	// proven on _ryu_capture: Cable sid 0xd4 4/4 + 110 rectab-TCW parts 0 mismatch). This
	// restores the GSTA-size wire (state only, ~tens of KB/frame, no decoded-pixel band).
	// The tiledesc (0x1800) + rectab (0x10000) enlargements above are KEPT (legit state that
	// was truncated; nothing to do with the removed texture band).

	_dynTotal = 0;
	for (auto& r : _dynRegs) _dynTotal += r.len;

	// ---- STATIC: GFX1/GFX2 per active body (deduped), shipped once ----
	auto S = [&](u32 a, u32 l, const char* w) {
		for (auto& e : _staticRegs) if (e.addr == a) return;   // dedup
		pushReg(_staticRegs, a, l, w);
	};
	for (int L = 0; L < 16; L++) {
		u32 cnt = rd8(0x8C2895E0 + L); if (cnt == 0 || cnt > 0x60) continue;
		u32 base = 0x8C287DE0 + L * 0x180;
		for (u32 i = 0; i < cnt; i++) {
			u32 node = rd32(base + i * 4); if (!isRam(node)) continue;
			if (rd8(node + 0x3) != 0) continue;                // body only (cat==0)
			u32 GFX2 = rd32(node + 0x160), GFX1 = rd32(node + 0x15C);
			if (isRam(GFX2)) { u32 b, l; gfxShipDescriptor(GFX2, true,  b, l); S(b, l, "GFX2"); }
			if (isRam(GFX1)) { u32 b, l; gfxShipDescriptor(GFX1, false, b, l); S(b, l, "GFX1"); }
		}
	}

	_tablesBuilt = true;
}

// ===========================================================================
// STATIC PREFIX build (SH4 thread, once). Produces the uncompressed MCRR prefix
// then zstd's it into _prefixZcst (ZCST envelope). Per the format doc the static
// payload is VRAM (8MB) + PVR regs (32KB) + each static region's bytes; for the
// live MVP the "16MB area-3 RAM backdrop" is shipped as an ADDITIONAL static
// region so the client can splat the whole RAM image once and overlay the
// per-frame dynamic regions on top (the dynamic regions are subsets of that RAM,
// re-applied each frame). nFrames=0 (streamed, not file).
// ===========================================================================

static void buildPrefixLocked()
{
	// Assemble the uncompressed prefix.
	std::vector<uint8_t> p;
	const u32 vramBytes = (u32)VRAM_SIZE;
	const u32 pvrBytes  = (u32)pvr_RegSize;

	// The 16MB RAM backdrop is exposed in the STATIC region table so the client's
	// generic MCRR loader splats it like any other static region (addr & 0xFFFFFF).
	// It is the LAST static region; it does not affect the dynamic stream.
	std::vector<Reg> staticTbl = _staticRegs;
	pushReg(staticTbl, 0x8C000000u, (u32)RAM_SIZE, "ram16");

	// ---- header (32B) ----
	auto put32 = [&](u32 v) { uint8_t b[4]; memcpy(b, &v, 4); p.insert(p.end(), b, b + 4); };
	put32(MCRR_MAGIC);            // magic "MCRR"
	put32(1u);                    // version
	put32((u32)staticTbl.size()); // nStatic (incl ram16)
	put32((u32)_dynRegs.size());  // nDynamic
	put32(0u);                    // nFrames = 0 (streamed)
	put32(vramBytes);             // vramBytes
	put32(pvrBytes);              // pvrBytes
	put32(0u);                    // reserved

	// ---- STATIC region table : nStatic × { addr u32, len u32, tag[8] } ----
	for (auto& r : staticTbl) {
		put32(r.addr); put32(r.len);
		p.insert(p.end(), r.tag, r.tag + 8);
	}
	// ---- DYNAMIC region table : nDynamic × { addr u32, len u32, tag[8] } ----
	for (auto& r : _dynRegs) {
		put32(r.addr); put32(r.len);
		p.insert(p.end(), r.tag, r.tag + 8);
	}

	// ---- STATIC payload: VRAM, PVR regs, then each static region's bytes ----
	// VRAM + PVR come from the resident arrays directly (the render path owns them
	// on this thread). The GFX1/GFX2 regions + the 16MB RAM are read alias-safe via
	// addrspace so a P0/P1/P2 alias resolves identically.
	p.insert(p.end(), &vram[0], &vram[0] + vramBytes);
	p.insert(p.end(), pvr_regs, pvr_regs + pvrBytes);
	for (auto& r : staticTbl) {
		size_t off = p.size();
		p.resize(off + r.len);
		// The 16MB RAM backdrop and the GFX1/GFX2 regions are all clean main-RAM (area-3)
		// addresses; copy from mem_b directly (addr & 0xFFFFFF — alias-identical to addrspace,
		// and far faster than a per-byte loop now that GFX regions are real-size ~1.2MB each).
		// Any non-main-RAM static region (none today) falls back to alias-safe reads.
		if ((r.addr & 0xFF000000u) == 0x8C000000u || (r.addr & 0xFF000000u) == 0x0C000000u) {
			memcpy(&p[off], &mem_b[r.addr & 0x00FFFFFFu], r.len);
		} else {
			for (u32 b = 0; b < r.len; b++) p[off + b] = rd8(r.addr + b);
		}
	}

	// ---- zstd into ZCST envelope (level 3, one-shot) ----
	size_t compSize = 0; uint64_t cus = 0;
	const uint8_t* comp = _prefixComp.compress(p.data(), (u32)p.size(), compSize, cus, 3);
	_prefixZcst.assign(comp, comp + compSize);
	_prefixReady.store(true, std::memory_order_release);

	fprintf(stderr,
		"[REPLICA-LIVE] static prefix built: %d static (incl 16MB RAM) + %d dynamic regions, "
		"uncompressed %zu B -> ZCST %zu B (%.1fx)\n",
		(int)staticTbl.size(), (int)_dynRegs.size(), p.size(), compSize,
		p.size() ? (double)p.size() / (double)compSize : 0.0);
}

// ===========================================================================
// WS thread: accept handlers + sender loop
// ===========================================================================

static void sendPrefixTo(RlConnHdl hdl)
{
	if (!_prefixReady.load(std::memory_order_acquire)) return;
	try {
		_ws.send(hdl, _prefixZcst.data(), _prefixZcst.size(),
		         websocketpp::frame::opcode::binary);
	} catch (const std::exception& e) {
		fprintf(stderr, "[REPLICA-LIVE] prefix send failed: %s\n", e.what());
	} catch (...) {}
}

static void onOpen(RlConnHdl hdl)
{
	{
		std::lock_guard<std::mutex> lk(_connMutex);
		_conns.insert(hdl);
	}
	_clientCount.fetch_add(1, std::memory_order_relaxed);
	// A NEW client gets the cached static prefix, which only carries the bodies' GFX that were
	// resident WHEN THE PREFIX WAS BUILT. Any body whose art loaded later was shipped to earlier
	// clients via the on-change GFX delta and marked in _gfxShipped — so this new client would
	// otherwise never receive that GFX (grid). FIX: clear _gfxShipped so the next captured frame
	// re-ships every active body's fresh GFX to ALL clients (existing clients get a benign
	// duplicate; the new client gets the GFX it's missing). One relaxed store under the pub mutex
	// is enough — the SH4 thread reads/writes _gfxShipped only inside captureFrame.
	_gfxResendAll.store(true, std::memory_order_relaxed);
	// Likewise re-ship the PVR palette on the next captured frame: the new client's seeded pvr is the
	// prefix snapshot, which froze any bank written after the prefix was built (the palette-gap fix).
	_pvrPalResend.store(true, std::memory_order_relaxed);
	// Send the static prefix if it's already built. If not (no in-match frame has
	// armed the build yet), the sender loop sends it to all conns the moment it
	// becomes ready (see drainAndSend), so a client that connects pre-match still
	// gets the prefix before its first FRAME RECORD.
	sendPrefixTo(hdl);
	fprintf(stderr, "[REPLICA-LIVE] client connected (%d total)\n",
		_clientCount.load(std::memory_order_relaxed));
}

static void onClose(RlConnHdl hdl)
{
	{
		std::lock_guard<std::mutex> lk(_connMutex);
		_conns.erase(hdl);
	}
	int n = _clientCount.fetch_sub(1, std::memory_order_relaxed) - 1;
	if (n < 0) { _clientCount.store(0, std::memory_order_relaxed); n = 0; }
	fprintf(stderr, "[REPLICA-LIVE] client disconnected (%d remain)\n", n);
}

// Sender loop: waits for a published dynamic frame, compresses it, broadcasts.
static void senderLoop()
{
	bool prefixBroadcast = false;   // have we pushed the prefix to all current conns?
	std::vector<uint8_t> mine;      // WS-thread-private copy of the published frame

	for (;;) {
		size_t len;
		{
			std::unique_lock<std::mutex> lk(_pubMutex);
			_pubCv.wait(lk, [] { return _pubHasFrame || _pubQuit; });
			if (_pubQuit) return;
			// COPY the published bytes into a private buffer WHILE HOLDING the lock.
			// This decouples the WS thread from the SH4 double-buffer: once copied,
			// the SH4 thread is free to overwrite either staging buffer (drop-old)
			// without racing our compress/send below. The lock is held only for this
			// ~58KB memcpy — never for the socket write.
			len = _pubLen;
			mine.assign(_pubPtr, _pubPtr + len);
			_pubHasFrame = false;     // consume
		}
		const uint8_t* ptr = mine.data();

		// If the prefix just became ready (first armed frame), broadcast it to any
		// already-open connections that connected before it existed.
		if (!prefixBroadcast && _prefixReady.load(std::memory_order_acquire)) {
			std::set<RlConnHdl, std::owner_less<RlConnHdl>> snapshot;
			{ std::lock_guard<std::mutex> lk(_connMutex); snapshot = _conns; }
			for (auto& h : snapshot) sendPrefixTo(h);
			prefixBroadcast = true;
		}

		// Build the FRAME RECORD inner payload: "FRMx" + vframe + taSize(=0) + dyn bytes.
		// vframe is embedded by the SH4 thread as the first 4 bytes after a 12B header
		// it already laid down (see captureFrame): the staging buffer IS the inner
		// payload, ready to compress as-is.
		size_t compSize = 0; uint64_t cus = 0;
		const uint8_t* comp = _frameComp.compress(ptr, (u32)len, compSize, cus, 1);

		std::set<RlConnHdl, std::owner_less<RlConnHdl>> snapshot;
		{ std::lock_guard<std::mutex> lk(_connMutex); snapshot = _conns; }
		for (auto& h : snapshot) {
			try {
				_ws.send(h, comp, compSize, websocketpp::frame::opcode::binary);
			} catch (...) { /* conn may be closing; onClose will reap it */ }
		}
	}
}

// ===========================================================================
// SH4-thread per-frame capture
// ===========================================================================

// Collect, this frame, the active bodies' GFX1/GFX2 page-aligned bases whose (base,content)
// has NOT already been shipped (covers the never-shipped char + a mid-stream re-load). Returns
// the list to append as a variable GFX tail. Does NOT mark them shipped yet — that happens only
// after this frame WINS the publish swap (so a drop-old discard re-evaluates them next frame).
struct GfxToShip { u32 base; u32 len; u32 sig; };
static void collectFreshGfx(std::vector<GfxToShip>& out)
{
	for (int L = 0; L < 16; L++) {
		u32 cnt = rd8(0x8C2895E0 + L); if (cnt == 0 || cnt > 0x60) continue;
		u32 base = 0x8C287DE0 + L * 0x180;
		for (u32 i = 0; i < cnt; i++) {
			u32 node = rd32(base + i * 4); if (!isRam(node)) continue;
			// Ship GFX for BODIES (cat==0) AND SATELLITES/EFFECTS (cat 1..4). The old
			// `cat==0 only` filter was a latent BLANK-satellite bug (re_kb
			// finding:gsta_satellite_gfx_gap, MEASURED 2026-06-15): a cat 1..4 node whose
			// GFX1/GFX2 is NOT shared with an on-screen body — an assist's projectile, a
			// summoned drone, an Effect-Poly effect — would never have its art shipped, so
			// render_frame's satellite walker decodes from stale/zero RAM => the texture is
			// blank. In the common case the satellite shares the owning body's GFX (Storm's
			// cape: sat gfx1==body gfx1==c420040), so the on-change sig dedup below makes
			// this a ZERO-cost no-op there; it only adds bytes when a satellite/effect brings
			// genuinely NEW art the body filter missed. The render path (render_frame
			// render_object_full_satellite) already walks these nodes — it just needs the art.
			{ int cat = (int)(int8_t)rd8(node + 0x3); if (cat < 0 || cat >= 5) continue; }
			u32 gfx[2]   = { rd32(node + 0x160), rd32(node + 0x15C) };  // GFX2, GFX1
			bool isG2[2] = { true, false };
			for (u32 k = 0; k < 2; k++) {
				if (!isRam(gfx[k])) continue;
				u32 pbase, plen;
				gfxShipDescriptor(gfx[k], isG2[k], pbase, plen); // REAL extent, page-aligned base+len
				// GFX2: DENSE dispatch-head fold so a per-sub-frame SELF-MODIFY of GFX2[sid*4]
				// (the Storm-scramble root cause, re_kb 32) flips the sig and re-ships the region.
				u32 sig = gfxSig(pbase, plen, isG2[k]);          // sig over the FULL region (+dense head)
				if (gfxAlreadyShipped(pbase, sig)) continue;     // already fresh on the client
				bool dup = false;
				for (auto& g : out) if (g.base == pbase) { dup = true; break; }
				if (!dup) out.push_back({ pbase, plen, sig });
			}
		}
	}
}

static void captureFrame(u32 vframe)
{
	// A new client connected: drop our shipped-GFX memory so this frame re-ships every active
	// body's GFX (the cached prefix only has the build-time bodies). Existing clients get a
	// benign duplicate; the new client gets the GFX it was missing.
	if (_gfxResendAll.exchange(false, std::memory_order_relaxed)) _gfxShipped.clear();

	// A new client connected: also re-ship the palette this frame (its seeded pvr is the prefix
	// snapshot; a bank written after that snapshot would otherwise be frozen/black on the new client).
	if (_pvrPalResend.exchange(false, std::memory_order_relaxed)) _pvrPalEverSent = false;

	// Determine this frame's fresh-GFX delta (on-change dynamic GFX, the static-gap fix).
	std::vector<GfxToShip> freshGfx;
	collectFreshGfx(freshGfx);

	// Determine this frame's palette delta (on-change PVR palette, the palette-gap fix). Ship the
	// full 32KB pvr_regs block when its palette signature changed (or never shipped / new connect).
	u32  curPalSig  = pvrPalSig();
	bool shipPal    = (!_pvrPalEverSent) || (curPalSig != _pvrPalSig);
	u32  pvrPalLen  = shipPal ? (u32)pvr_RegSize : 0u;

	// Pick the staging buffer NOT currently published (double-buffer). Inner payload =
	//   12B FRAME RECORD header + fixed dynamic bytes + VARIABLE GFX tail + PALETTE tail:
	//     u32 nGfx ; nGfx × { u32 base ; u32 len ; len bytes }            (GFX tail, real-size fix)
	//     u32 pvrPalLen ; pvrPalLen bytes of pvr_regs                     (PALETTE tail, palette-gap fix)
	// Both length words are ALWAYS present (0 in the steady state). Each GFX region ships at its REAL
	// extent (GFX1 ~1.1MB, GFX2 ~64KB) instead of a fixed 128KB that truncated the cell records. The
	// palette block is the full 32KB pvr_regs, shipped only when the palette signature changed.
	const size_t hdr   = 12;
	const size_t tailHdr = 4;                                  // u32 nGfx
	size_t gfxBytes = 0;
	for (auto& g : freshGfx) gfxBytes += 8u + (size_t)g.len;   // {u32 base, u32 len, len bytes}
	// PALETTE TAIL (strict append AFTER the GFX tail): u32 pvrPalLen ; pvrPalLen bytes of pvr_regs.
	// pvrPalLen is ALWAYS present (0 in steady state). An older client that stops parsing after the
	// GFX tail simply ignores these trailing bytes; the updated client refreshes this.pvr from them.
	const size_t palHdr  = 4;                                  // u32 pvrPalLen

	// HUDQ TAIL (third variable tail, strict append AFTER the palette tail): the engine's
	// REAL HUD/composite quads captured this frame from the surviving TA pass (maplecast_oracle_hook
	// collectHudQuads). u32 magic + u32 nHud + nHud × 96-byte HudQuad. The render-replica client
	// draws these instead of the hand-coded HUD reconstruction → pixel-perfect. Present ONLY when
	// MAPLECAST_HUD_TA is armed AND the surviving pass yielded HUD quads (else absent → older clients
	// and HUD-off runs are byte-identical to before). Magic-tagged so the client can skip it safely.
	int nHud = 0;
	const maplecast_oracle_hook::HudQuad* hudQuads =
		maplecast_oracle_hook::mc_hudTaEnabled ? maplecast_oracle_hook::mc_oracle_hudQuads(&nHud) : nullptr;
	if (!hudQuads) nHud = 0;
	static_assert(sizeof(maplecast_oracle_hook::HudQuad) == 96, "HudQuad must be 96 bytes (wire interface)");
	const size_t hudHdr   = nHud ? 8u : 0u;                   // u32 magic + u32 nHud (only when present)
	const size_t hudBytes = (size_t)nHud * sizeof(maplecast_oracle_hook::HudQuad);

	const size_t total = hdr + _dynTotal + tailHdr + gfxBytes + palHdr + (size_t)pvrPalLen + hudHdr + hudBytes;

	int which = _dynWhich ^ 1;          // write the other buffer
	std::vector<uint8_t>& buf = _dynBuf[which];
	if (buf.size() != total) buf.resize(total);

	// ---- FRAME RECORD header: "FRMx"(u32) + vframe(u32) + taSize(u32=0) ----
	u32 h0 = FRMX_MAGIC, h1 = vframe, h2 = 0u;
	memcpy(&buf[0], &h0, 4);
	memcpy(&buf[4], &h1, 4);
	memcpy(&buf[8], &h2, 4);

	// ---- dynamic regions in table order, raw bytes ----
	size_t off = hdr;
	for (auto& r : _dynRegs) {
		// PHASE 5: all dynamic regions are guest STATE now (no VRAM texture band). The two
		// big tables (idxtab/rectab) and the slot ptr arrays + char structs all live in main
		// RAM; copy from mem_b directly when the addr is a clean 0x8C... main-RAM address
		// (fast), else fall back to alias-safe reads.
		if ((r.addr & 0xFF000000u) == 0x8C000000u) {
			memcpy(&buf[off], &mem_b[r.addr & 0x00FFFFFFu], r.len);
		} else {
			for (u32 b = 0; b < r.len; b++) buf[off + b] = rd8(r.addr + b);
		}
		off += r.len;
	}

	// ---- VARIABLE GFX tail: u32 nGfx, then per fresh body GFX region { base, len, len bytes } ----
	u32 nGfx = (u32)freshGfx.size();
	memcpy(&buf[off], &nGfx, 4); off += 4;
	for (auto& g : freshGfx) {
		memcpy(&buf[off], &g.base, 4); off += 4;               // page-aligned guest base (0x8C..)
		memcpy(&buf[off], &g.len,  4); off += 4;               // REAL region length (per-region)
		// GFX is main-RAM; copy from mem_b directly (alias-safe identical to addrspace here).
		memcpy(&buf[off], &mem_b[g.base & 0x00FFFFFFu], g.len);
		off += g.len;
	}

	// ---- PALETTE TAIL: u32 pvrPalLen, then pvrPalLen bytes of the LIVE pvr_regs (palette-gap fix) ----
	// pvr_regs is the resident PVR register/palette array owned by the render path on this thread;
	// the full 32KB block carries PAL_RAM_CTRL(0x108) + PALETTE_RAM(0x1000..0x1FFF) the client's
	// updatePalette() needs. Shipped only when the palette signature changed (else pvrPalLen=0).
	memcpy(&buf[off], &pvrPalLen, 4); off += 4;
	if (pvrPalLen) { memcpy(&buf[off], pvr_regs, pvrPalLen); off += pvrPalLen; }

	// ---- HUDQ TAIL: u32 magic, u32 nHud, then nHud × 96-byte HudQuad (the engine's REAL
	// HUD quads this frame). Strictly AFTER the palette tail. Absent when nHud==0 so the
	// steady HUD-off path is byte-identical. ----
	if (nHud) {
		memcpy(&buf[off], &HUDQ_MAGIC, 4); off += 4;
		u32 nh = (u32)nHud; memcpy(&buf[off], &nh, 4); off += 4;
		memcpy(&buf[off], hudQuads, hudBytes); off += hudBytes;
	}

	// ---- publish to the WS thread (drop-old: overwrite any undrained frame) ----
	{
		std::lock_guard<std::mutex> lk(_pubMutex);
		_pubPtr = buf.data();
		_pubLen = total;
		_pubHasFrame = true;            // if a previous frame was pending, it's dropped
		_dynWhich = which;              // this buffer is now the "published" one
	}
	_pubCv.notify_one();

	// Mark the GFX shipped ONLY now that this frame won the publish swap. A drop-old discard of a
	// PREVIOUS frame is harmless: this frame re-collected the same fresh bases (still unshipped)
	// and re-shipped them. (Marking after publish means at worst we re-ship one extra frame if the
	// very NEXT frame drops this one before the WS thread drains it — a benign duplicate.)
	if (nGfx) for (auto& g : freshGfx) gfxMarkShipped(g.base, g.sig);

	// Mark the palette shipped (same after-publish discipline as GFX): a drop-old discard of a
	// previous frame re-evaluated shipPal against the unchanged _pvrPalSig, so re-shipping is benign.
	if (pvrPalLen) { _pvrPalSig = curPalSig; _pvrPalEverSent = true; }
}

// ===========================================================================
// Public API
// ===========================================================================

void onRenderFrame(void* /*ctxv*/)
{
	// HOT PATH GATE (free when off / idle): not armed, or no client connected.
	if (!_armed) return;
	if (_clientCount.load(std::memory_order_relaxed) == 0) return;

	// In-match gate — same as the Oracle. Outside a match the render read-set is
	// not meaningful for the fighter renderer; skip the capture entirely.
	if (rd8(IN_MATCH_ADDR) == 0) return;

	// Build region tables + the static prefix lazily, on the first armed in-match
	// frame (the slot table + GFX pointers + RAM are populated by now).
	if (!_tablesBuilt) buildTables();
	if (!_prefixReady.load(std::memory_order_acquire)) {
		std::lock_guard<std::mutex> lk(_prefixMutex);
		if (!_prefixReady.load(std::memory_order_acquire)) {
			buildPrefixLocked();
			// CAPTURE ATOMICITY (re_kb 32: the torn-capture / one-frame cat-3 artifact). The static
			// prefix GFX2 snapshot above was read at THIS vframe; force this same frame's captureFrame
			// to re-ship every active body's GFX2/GFX1 as a fresh dynamic tail so the GFX2 dispatch
			// table the walker reads is INTERNALLY CONSISTENT with the dynamic char/slot state of the
			// SAME vframe. (_gfxShipped is already empty on first build, but make the contract explicit
			// so the dispatch head can never lag the dynamic state by a frame.)
			_gfxShipped.clear();
		}
	}

	const u32 vframe = rd32(VFRAME_ADDR);
	captureFrame(vframe);
}

void init()
{
	const char* env = getenv("MAPLECAST_REPLICA_LIVE");
	if (!env || env[0] == '0' || env[0] == '\0') {
		// UNSET / "0" ⇒ stay completely inert. No thread, no buffers, no overhead.
		_armed = false;
		return;
	}
	_armed = true;

	if (const char* pe = getenv("MAPLECAST_REPLICA_LIVE_PORT")) {
		int p = atoi(pe); if (p > 0 && p < 65536) _port = p;
	}

	if (!_compInit) {
		// Prefix: VRAM(8MB)+PVR(32KB)+RAM(16MB)+ the build-time bodies' REAL-SIZE GFX (GFX1 up to
		// ~1.2MB + GFX2 ~0.15MB per body, up to 2 bodies resident at prefix build) ≈ 24+2.7 ≈ 27MB.
		// 32MB gives headroom (the real-size GFX fix grew the static GFX from 0x20000 to ~1.3MB/body).
		_prefixComp.init((size_t)32 * 1024 * 1024);
		// Per-frame dynamic payload (Phase 5: STATE ONLY, no texture band) ≈ slot tables +
		// char structs + tiledesc(0x1800) + idxtab(0x2000) + rectab(0x10000) + camera/globals
		// ≈ 90-110KB raw worst case; compresses to GSTA-size. PLUS the REAL-SIZE on-change GFX tail:
		// a resend-all frame (new connect / multi-char load) ships every active body's full GFX —
		// up to ~4 bodies × (GFX1 ~1.2MB + GFX2 ~0.15MB) ≈ 5.4MB raw. 8MB init covers that worst case
		// with headroom; steady state is nGfx=0 (a 4B tail). ONE-TIME per character/per connect.
		_frameComp.init((size_t)8 * 1024 * 1024);
		_compInit = true;
	}

	try {
		_ws.clear_access_channels(websocketpp::log::alevel::all);
		_ws.clear_error_channels(websocketpp::log::elevel::all);
		_ws.init_asio();
		_ws.set_reuse_addr(true);
		_ws.set_open_handler(&onOpen);
		_ws.set_close_handler(&onClose);
		// No message handler: this is a one-way stream (server → client). Inbound
		// frames from the client are ignored.

		// LOOPBACK ONLY — the stream carries ROM-derived VRAM/RAM; it must never be
		// publicly reachable. nginx terminates TLS and proxies a wss path here.
		websocketpp::lib::asio::ip::tcp::endpoint loopback(
			websocketpp::lib::asio::ip::address_v4::loopback(),
			static_cast<uint16_t>(_port));
		_ws.listen(loopback);
		_ws.start_accept();

		_active.store(true);
		_wsThread = std::thread([&]() {
			std::thread sender(&senderLoop);
			try { _ws.run(); }
			catch (const std::exception& e) { fprintf(stderr, "[REPLICA-LIVE] run() threw: %s\n", e.what()); }
			catch (...) { fprintf(stderr, "[REPLICA-LIVE] run() threw unknown\n"); }
			// asio run() returned (server stopped) — wake & join the sender.
			{ std::lock_guard<std::mutex> lk(_pubMutex); _pubQuit = true; }
			_pubCv.notify_all();
			if (sender.joinable()) sender.join();
		});

		fprintf(stderr,
			"[REPLICA-LIVE] armed: streaming MCRR render read-set on ws://127.0.0.1:%d "
			"(loopback only, READ-ONLY)\n", _port);
	} catch (const std::exception& e) {
		fprintf(stderr, "[REPLICA-LIVE] init failed: %s\n", e.what());
		_active.store(false);
		_armed = false;
	}
}

void shutdown()
{
	if (!_active.exchange(false)) return;
	try { _ws.stop(); } catch (...) {}
	if (_wsThread.joinable()) _wsThread.join();
	{ std::lock_guard<std::mutex> lk(_connMutex); _conns.clear(); }
	_clientCount.store(0, std::memory_order_relaxed);
}

bool enabled() { return _armed; }

} // namespace maplecast_replica_live
