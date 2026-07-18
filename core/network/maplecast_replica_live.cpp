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
#include "statewire_v2.h"                // state-wire v2 keyframe/delta codec (opt-in)

#include "hw/sh4/sh4_mem.h"              // addrspace::read*, mem_b, RAM_SIZE
#include "network/maplecast_mirror.h"    // A2 STATEVF: suppressActive() to log only the shipped leg
#include "hw/pvr/pvr_mem.h"              // vram, VRAM_SIZE
#include "hw/pvr/pvr_regs.h"             // pvr_regs[], pvr_RegSize
#include "types.h"                       // RAM_SIZE / VRAM_SIZE macros

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <atomic>
#include <condition_variable>
#include <algorithm>
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

// CHARACTER-PASS TABLE SNAPSHOT (re_kb/50 — the idxtab effect-range staleness fix).
// idxtab/rectab effect entries (idxtab[972..1074]) are written by the per-tile submit
// (bank12 loc_8c124a30) DURING the SH4 character-pass render walk, then OVERWRITTEN/reverted
// by the HUD/composite pass before the replica capture (captureFrame) runs at the HUD-pass
// STARTRENDER. So the live HUD-pass idxtab maps the effect indices to STALE body rectab
// entries -> the scale walker resolves the wrong (body) texture (MEASURED: idxtab[1005]=872
// -> rectab[872]=c1e00 body, engine rendered an effect TCW). FIX: snapshot idxtab+rectab at
// the CHARACTER pass (mc_oracle_charPassCapture, isCharacterPass) into these side buffers;
// captureFrame ships THESE for the idxtab/rectab regions instead of the live (HUD-pass) RAM.
// This is a SCOPED read-only side-snapshot of 2 regions — NOT the onRenderFrame pass-gate
// (that gated the whole capture and stalled the stream). Body entries are identical both
// passes, so shipping the char-pass snapshot is safe for bodies and fixes effects.
static std::vector<uint8_t> _idxtabSnap;            // char-pass idxtab bytes
static std::vector<uint8_t> _rectabSnap;            // char-pass rectab bytes
static std::vector<uint8_t> _tiledescSnap;          // char-pass tile-descriptor bytes (0x8C1F9F9C)
static u32                  _idxtabAddr = 0, _idxtabLen = 0;
static u32                  _rectabAddr = 0, _rectabLen = 0;
// TILEDESC char-pass snapshot (2026-07-02, finding:storm_idle_tiledesc_count_staleness). The
// per-frame tile-descriptor table 0x8C1F9F9C is FIXED-address (not pointer-resolved). Its COUNT
// bytes (byte1 per entry) at the LOW/arena-start indices the FIRST body consumes are re-seeded
// (toward cnt=1) by the HUD pass AFTER the character-pass body walk read them — same char-pass-
// transient class as the idxtab/rectab effect entries (re_kb/50). PAIRED-DUMP GROUND TRUTH
// (Storm P1C1 dc=0): CLEAN char-pass desc CNT[18,19,20]=2,2,4 -> walker 24/24 == engine; the
// HUD-pass-stale capture had CNT[18,19,20]=1,1,1 -> walker 21/25 (4 Storm limb tiles dropped ->
// "stale white/gray blocks around idle Storm"). The SIZE bytes are identical both passes; only
// the COUNT bytes are clobbered. So snapshot the WHOLE tiledesc at the char pass and ship THAT.
static const u32            TILEDESC_ADDR = 0x8C1F9F9Cu;
static const u32            TILEDESC_LEN  = 0x1800u;
static std::atomic<bool>    _tablesSnapValid{false};
static std::atomic<bool>    _tiledescSnapValid{false};   // separate: tiledesc snaps at CHAR pass, not serverPublish
static std::mutex           _tablesSnapMutex;

// SLOT-TABLE + OBJPOOL CHARACTER-PASS SNAPSHOT (2026-07-04, finding:replica_live_slot_objpool_snapshot_coherency).
// The engine's authoritative "what renders this frame" is the freshly-built display list at the
// CHARACTER-pass render-walk (loc_8c0308c2, bank03:1200): for each of 16 layers it walks idx in
// [0, slot_cnt[layer]) over slot_ptr[layer][idx], dispatching body (loc_8c03093c) or effect
// (loc_8c030af8) by node+0x03 category; each applies a SECONDARY node+0x12C!=0 visibility gate
// (loc_8c03093c:1285-1290, loc_8c030af8:1530-1535 — offsets loc_8c030aa4/loc_8c030c66 = 0x12C).
// The table is CLEARED every frame (loc_8c045208 zeroes 0x8C2895E0[0..0x10)) and REBUILT by the
// per-node registrar loc_8c04515e (bank04:12166), which is ITSELF gated on node+0x12C!=0
// (bank04:12174-12177: mov.b @(0x12c,r4); tst; bt skip) — it then increments slot_cnt[layer]
// (0x8C2895E0+node+0x24) and appends the node ptr (0x8C287DE0[...]). So a satellite that is
// removed/deactivated is simply NOT re-registered — but its objpool node+0x12C byte is NOT cleared
// that frame, so it leaks a stale-nonzero visibility bit. If the server snapshots slot_cnt/slot_ptr
// (0x8C2895E0/0x8C287DE0) and the objpool (+0x12C @ 0x8C26AA54) at DIFFERENT instants than the
// char-pass render-walk, a decremented-away node with stale +0x12C leaks (MEASURED cape f1000: 3
// dead P1-body satellites 0x8c278ce4/0x8c26afc4/0x8c26adf4, active==0 but +0x12C==0x0010FF01, ship
// in slot_cnt=4 -> client balloons the body span 45->60 tiles). FIX: snapshot all three regions AT
// THE CHARACTER PASS — the same realBody-gated STARTRENDER instant the tiledesc/gfx2 snapshots use
// (mc_oracle_charPassCapture) — and captureFrame ships THESE, so the client sees the coherent
// render-walk view (removed nodes already out of the count; +0x12C matched to that instant). Gated
// MAPLECAST_SLOT_CHARSNAP, DEFAULT ON (must stay coherent with the DEFAULT-ON tiledesc snapshot).
static const u32            SLOTCNT_ADDR = 0x8C2895E0u;   static const u32 SLOTCNT_LEN = 0x10u;
static const u32            SLOTPTR_ADDR = 0x8C287DE0u;   static const u32 SLOTPTR_LEN = 16u * 0x180u;
static const u32            OBJPOOL_ADDR = 0x8C26AA54u;   static const u32 OBJPOOL_LEN = 0x1D000u;
static std::vector<uint8_t> _slotCntSnap;                 // char-pass slot_cnt bytes (0x8C2895E0)
static std::vector<uint8_t> _slotPtrSnap;                 // char-pass slot_ptr bytes (0x8C287DE0)
static std::vector<uint8_t> _objpoolSnap;                 // char-pass objpool bytes (0x8C26AA54, carries +0x12C)
static std::atomic<bool>    _slotSnapValid{false};        // snaps at CHAR pass, shipped by captureFrame

// FIRST-BODY idxtab WINDOW char-pass snapshot (2026-07-03, finding:hud_clobbers_first_body_idxtab).
// The FIRST character body has +0xDC=0, so its tiles resolve idxtab[arena_base + 0 .. +ntiles0).
// The per-object arena cursor 0x8C1F9D98 is RESET to 0 at the START of EACH render pass, so the
// HUD pass's OWN first object ALSO writes idxtab[arena_base + 0 ..] — OVERWRITING exactly the
// first body's window by the time captureFrame ships the HUD-pass idxtab. The 2nd body (+0xDC=25)
// resolves idxtab[arena_base+25..], which the HUD pass never reaches, so it survives -> Cable
// coherent, Storm scrambled (right tiles, wrong per-tile tcw). FIX: snapshot ONLY the first body's
// idxtab window at the CHARACTER-pass STARTRENDER (mc_oracle_charPassCapture) — where body0's walk
// has WRITTEN its idxtab AND the HUD pass has NOT yet clobbered it (rend_start_render fires after
// the SH4 draw walk, before QueueRender/HUD) — and OVERLAY it onto the shipped (HUD-pass) idxtab in
// captureFrame. rectab stays STARTRENDER (finalized during the walk; taking it early = red/white
// garbage regression, 2026-07-03). Gated MAPLECAST_IDXTAB_CHARSNAP, DEFAULT OFF (A/B).
static std::vector<uint8_t> _idxWinSnap;                 // the first-body idxtab window bytes
static u32                  _idxWinOff = 0, _idxWinLen = 0;   // byte offset+len into the idxtab region
static std::atomic<bool>    _idxWinValid{false};

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

// CHAR-PASS GFX2 SNAPSHOT (Storm under-tile fix, re_kb/60 finding:storm_shipped_descriptor_tear).
// The engine SELF-MODIFIES the GFX2 cell-record DISPATCH HEAD GFX2[(sid&0x7FFF)*4] in place per
// animation sub-frame (re_kb/32). That mutation is LIVE only during the CHARACTER render pass; by
// the time captureFrame's collectFreshGfx reads GFX2 (the winning HUD-pass STARTRENDER — the same
// instant that reverts the tiledesc) the head is back at the disc/default pose, so the on-change
// sig never sees the char-pass value and the wire FREEZES GFX2 -> the client walker (render_frame
// rebuild_tile_grid: cell = GFX2 + GFX2[sid*4]) reads a stale record list -> wrong tile count
// (Storm ~24-38 vs engine ~49-53). FIX (mirrors snapshotCharPassTiledesc): snapshot each active
// body's GFX2 region AT THE CHARACTER PASS, and have collectFreshGfx/captureFrame SOURCE GFX2 bytes
// from that snapshot so the LIVE self-modified head + its referenced records ship fresh every frame
// the head changes. GFX1 (LZSS pixels) never self-modifies -> stays live-sourced / static-once.
struct Gfx2Snap { u32 base; u32 len; u32 gen; std::vector<uint8_t> bytes; };
static std::vector<Gfx2Snap> _gfx2CharSnap;            // per-active-body GFX2 @ CHAR pass (render thread only)
static u32                   _gfx2CharSnapGen = 0;     // bumped each char pass; entry gen!=cur => stale/pruned
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

// Dense content signature over a CAPTURED buffer (the CHAR-PASS GFX2 snapshot). Folds EVERY u32 so
// a single per-sub-frame dispatch-head self-modify (e.g. Storm GFX2[sid*4] 0x1b04<->0x1b44) always
// flips the sig -> the region re-ships. GFX2 is small (~4-64KB) so the dense fold is cheap.
static u32 gfxSigBufDense(const uint8_t* buf, u32 len)
{
	u32 h = 2166136261u;
	for (u32 b = 0; b + 4 <= len; b += 4) { u32 w; memcpy(&w, buf + b, 4); h ^= w; h *= 16777619u; }
	return h;
}
// Look up THIS char-pass's GFX2 snapshot for a page-aligned base (gen==current only; a stale gen
// means the char pass hasn't refreshed it this frame -> caller falls back to the live head).
static const Gfx2Snap* findGfx2Snap(u32 base)
{
	for (auto& s : _gfx2CharSnap) if (s.base == base && s.gen == _gfx2CharSnapGen) return &s;
	return nullptr;
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

// --- state-wire v2 (MAPLECAST_STATEWIRE_V2=1): keyframe/delta dirty-diff of the
// per-frame DYNAMIC payload (statewire_v2.h, proven byte-exact in Py/C++/JS). OFF
// by default -> the v1 FRMx frame publishes unchanged. Frame magic FRM2 tells the
// client to decode the variable-length dynamic block before the GFX/pal/HUD tails. ---
static constexpr u32 FRM2_MAGIC = 0x324D5246u;     // "FRM2"
static std::vector<uint8_t> _v2Out[2];             // double-buffered v2 output (like _dynBuf)
static std::vector<uint8_t> _v2Blk;                // encoded dynamic block (scratch)
static std::vector<uint8_t> _v2Key;                // last keyframe raw dynamic blob
static u32  _v2KeyId    = 0;                        // frame ctr of the last keyframe
static u32  _v2FrameCtr = 0;
static std::atomic<bool> _v2ForceKey{false};       // new connect -> next frame is a keyframe
static bool statewireV2On() {
	static const bool on = []{ const char* e = getenv("MAPLECAST_STATEWIRE_V2"); return e && e[0] == '1'; }();
	return on;
}
static u32 statewireV2KeyInterval() {
	static const u32 k = []{ const char* e = getenv("MAPLECAST_STATEWIRE_V2_KEY"); u32 v = e ? (u32)atoi(e) : 60u; return v ? v : 60u; }();
	return k;
}
static constexpr u32 HUDQ_MAGIC = 0x48554451u;     // "HUDQ" (HUD-TA tail magic, LE)
static constexpr u32 BTCW_MAGIC = 0x57435442u;     // "BTCW" (resolved-body-tcw tail magic, LE)
static constexpr u32 PL3D_MAGIC = 0x44334C50u;     // "PL3D" (3D-machine SQ-flush tail magic, LE)

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
		if (isRam(idxtab)) { D(idxtab, 0x2000, "idxtab"); _idxtabAddr = idxtab; _idxtabLen = 0x2000; }
		// rectab: max record index hit 461/1024 with 2 bodies; 0x8000 truncated the
		// table on busier frames. 0x10000 gives headroom for a 3rd/4th body.
		if (isRam(rectab)) { D(rectab, 0x10000, "rectab"); _rectabAddr = rectab; _rectabLen = 0x10000; }
	}

	// EFFECT RECTAB-TEMPLATE arenas (re_kb/50 super-freeze fix — the bit15 SCALE-walker path).
	// MVC2's super/projectile effect parts (sel bit15 set) render via loc_8c0348c8 (the SCALE
	// walker), whose per-record rectab alloc-index = 0x390 + read.b(*(node+0x180)+0x220+ctr).
	// node+0x180 points at a PER-CHARACTER effect display-list TEMPLATE in 0x0C56_5000-family RAM
	// (bank13.asm loc_8c135594 bases / loc_8c1355b4 ends: each char slot owns a 0x3000 arena,
	// 0x0C565000..0x0C568000 etc). The engine REBUILDS this template every frame during the effect
	// pass (LIVE ASMTRACE on PC 0x8C034BA4: the per-record indices 0x3CC..0x432 derive from live
	// non-zero template bytes that DIFFER from the prefix-snapshot's stale 00,01,02..). Since the
	// template only rode the once-shipped static 16MB RAM image, the GSTA client read STALE bytes ->
	// the scale walker computed wrong indices -> wrong (c1xxx) effect TCWs -> the phantom-tiled-body
	// garble. FIX (additive read-set ship; does NOT alter the body path): ship the 7 per-character
	// template arenas every frame so render_frame's scale walker reads the LIVE per-record indices.
	// 7*0x3000 = ~84KB raw, but zstd-trivial when unchanged (they only carry data during a super).
	// Bases/ends are static ROM constants (bank13.asm); shipping all 7 is multi-character-safe.
	{
		static const u32 kEffectTemplateBase[7] = {
			0x8C565000u, 0x8C955000u, 0x8C6B5000u, 0x8CAA5000u,
			0x8C805000u, 0x8CBF5000u, 0x8CD45000u
		};
		for (int i = 0; i < 7; i++)
			if (isRam(kEffectTemplateBase[i])) D(kEffectTemplateBase[i], 0x3000, "efxtmpl");
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
struct GfxToShip { u32 base; u32 len; u32 sig; const uint8_t* src; };  // src=char-pass snapshot bytes (GFX2) or nullptr (live)
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
				// GFX2 (isG2): SOURCE the bytes from the CHARACTER-PASS snapshot — the LIVE
				// self-modified dispatch head — NOT the (reverted) HUD-pass RAM read at this instant.
				// This is the Storm under-tile fix (re_kb/60): reading GFX2 live here freezes the head
				// at the disc default on every frame but the rare one that caught it mid-pass, so the
				// walker's tile count is wrong. The dense-folded snapshot sig flips on any per-sub-frame
				// head mutation -> the (small) GFX2 region re-ships with the CORRECT records. GFX1
				// (pixels) does not self-modify -> live-sourced, static-once (cheap sparse-64 fold).
				const uint8_t* src = nullptr; u32 sig;
				if (isG2[k]) {
					const Gfx2Snap* sn = findGfx2Snap(pbase);
					if (sn && sn->len == plen) { src = sn->bytes.data(); sig = gfxSigBufDense(src, plen); }
					else sig = gfxSig(pbase, plen, true);        // fallback: no char-pass snap yet -> live head
				} else {
					sig = gfxSig(pbase, plen, false);            // GFX1 pixels: live, static-once
				}
				if (gfxAlreadyShipped(pbase, sig)) continue;     // already fresh on the client
				bool dup = false;
				for (auto& g : out) if (g.base == pbase) { dup = true; break; }
				if (!dup) out.push_back({ pbase, plen, sig, src });
			}
		}
	}
}

static void captureFrame(u32 vframe)
{
	// A2 STATE-WIRE gate (MAPLECAST_STATEVF=1): the DUMP_TA offset test measured the LEGACY TA
	// display list, NOT this 7212 state wire the GSTA client consumes. Log the state the wire
	// actually ships at publish: pos_x (+0x34, game-LOGIC, unambiguously F+2 after the preview leg)
	// and screen_x (+0xE0, RENDER-DEPOSITED at STARTRENDER — may inherit MVC2's 1-frame submit defer).
	// Compare baseline vs run-ahead published sequences: +1 offset on pos_x => run-ahead delivers.
	{
		// anim@+0x142/sprite@+0x144 (u32 @0x8C268482) CHANGE every frame via the idle animation
		// (pos_x is static when idle). Log only the SHIPPED (preview/non-suppressed) leg so the
		// run-ahead sequence is 1-per-tick and comparable to baseline. vf STAMP is unreliable
		// (increments at vblank AFTER this point) — compare the anim CONTENT, not the vf.
		static const bool _svf = std::getenv("MAPLECAST_STATEVF") != nullptr;
		if (_svf && !maplecast_mirror::suppressActive()) {
			printf("[STATEVF-PUB] vf=%u anim=%08x screenx=%08x\n",
			       vframe, rd32(0x8C268482), rd32(0x8C268420));
			fflush(stdout);
		}
	}

	// A new client connected: drop our shipped-GFX memory so this frame re-ships every active
	// body's GFX (the cached prefix only has the build-time bodies). Existing clients get a
	// benign duplicate; the new client gets the GFX it was missing.
	if (_gfxResendAll.exchange(false, std::memory_order_relaxed)) { _gfxShipped.clear(); _v2ForceKey.store(true, std::memory_order_relaxed); }

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

	// ---- SHIP-RESOLVED-BODY-TCW tail: the engine's resolved per-tile body tcws (parity-flip fix) ----
	int btcwWords = 0;
	const uint32_t* btcwBuf = maplecast_oracle_hook::mc_oracle_bodyTcws(&btcwWords);
	const size_t btcwHdr   = btcwWords ? 8u : 0u;             // u32 magic "BTCW" + u32 nWords
	const size_t btcwBytes = (size_t)btcwWords * 4u;

	// ---- PL3D tail: the 3D-machine (bank12 loc_8c129cc0 POL drawer) TA parcels captured at
	// the engine's own SQ flushes this frame (re_kb/64 finding:3d_draw_emit_map — impact
	// sparks / cast flashes / 3D effects). 36-byte records {kind,slot,cls,pad,32B SQ line};
	// the client appends the lines VERBATIM to fr.ta. Absent when no flush fired. ----
	int p3dBytes = 0;
	const uint8_t* p3dBuf = maplecast_oracle_hook::mc_oracle_poly3d(&p3dBytes);
	const size_t p3dHdr   = p3dBytes ? 8u : 0u;               // u32 magic "PL3D" + u32 nBytes
	const size_t p3dTail  = (size_t)p3dBytes;

	const size_t total = hdr + _dynTotal + tailHdr + gfxBytes + palHdr + (size_t)pvrPalLen
	                   + hudHdr + hudBytes + btcwHdr + btcwBytes + p3dHdr + p3dTail;

	int which = _dynWhich ^ 1;          // write the other buffer
	std::vector<uint8_t>& buf = _dynBuf[which];
	if (buf.size() != total) buf.resize(total);

	// ---- FRAME RECORD header: "FRMx"(u32) + vframe(u32) + taSize(u32=0) ----
	u32 h0 = FRMX_MAGIC, h1 = vframe, h2 = 0u;
	memcpy(&buf[0], &h0, 4);
	memcpy(&buf[4], &h1, 4);
	memcpy(&buf[8], &h2, 4);

	// ---- dynamic regions in table order, raw bytes ----
	// CHAR-PASS TABLE OVERRIDE (re_kb/50): for the idxtab/rectab regions, ship the CHARACTER-PASS
	// side-snapshot (snapshotCharPassTables) instead of the live HUD-pass RAM — the effect-range
	// entries are only valid at the char pass. Falls back to live RAM until the first char-pass
	// snapshot exists (and for non-table regions). Lock briefly to read the snapshot coherently.
	const bool haveSnap    = _tablesSnapValid.load(std::memory_order_acquire);     // idxtab/rectab (serverPublish)
	const bool haveTdSnap  = _tiledescSnapValid.load(std::memory_order_acquire);   // tiledesc (CHAR pass)
	const bool haveIdxWin  = _idxWinValid.load(std::memory_order_acquire);         // first-body idxtab window (CHAR pass)
	const bool haveSlotSnap = _slotSnapValid.load(std::memory_order_acquire);      // slot_cnt/slot_ptr/objpool (CHAR pass)
	std::unique_lock<std::mutex> snapLk(_tablesSnapMutex, std::defer_lock);
	if (haveSnap || haveTdSnap || haveIdxWin || haveSlotSnap) snapLk.lock();
	size_t off = hdr;
	for (auto& r : _dynRegs) {
		const uint8_t* src = nullptr;
		if (haveSnap && r.addr == _idxtabAddr && r.len == _idxtabLen && _idxtabSnap.size() == r.len)
			src = _idxtabSnap.data();
		else if (haveSnap && r.addr == _rectabAddr && r.len == _rectabLen && _rectabSnap.size() == r.len)
			src = _rectabSnap.data();
		else if (haveTdSnap && r.addr == TILEDESC_ADDR && r.len == TILEDESC_LEN && _tiledescSnap.size() == r.len)
			src = _tiledescSnap.data();   // CHAR-PASS tiledesc (Storm-idle fix, torn-safe: snapped pre-HUD-rebuild)
		// CHAR-PASS DISPLAY-LIST + OBJPOOL OVERRIDE (finding:replica_live_slot_objpool_snapshot_coherency):
		// ship the render-walk-instant slot_cnt/slot_ptr/objpool so a removed satellite's stale node+0x12C
		// cannot leak past the coherent count (the phantom-cape span-balloon fix). Coherent with tiledesc.
		else if (haveSlotSnap && r.addr == SLOTCNT_ADDR && r.len == SLOTCNT_LEN && _slotCntSnap.size() == r.len)
			src = _slotCntSnap.data();
		else if (haveSlotSnap && r.addr == SLOTPTR_ADDR && r.len == SLOTPTR_LEN && _slotPtrSnap.size() == r.len)
			src = _slotPtrSnap.data();
		else if (haveSlotSnap && r.addr == OBJPOOL_ADDR && r.len == OBJPOOL_LEN && _objpoolSnap.size() == r.len)
			src = _objpoolSnap.data();
		if (src) {
			memcpy(&buf[off], src, r.len);                          // char-pass snapshot (effect-correct)
		} else if ((r.addr & 0xFF000000u) == 0x8C000000u) {
			memcpy(&buf[off], &mem_b[r.addr & 0x00FFFFFFu], r.len); // fast main-RAM path
		} else {
			for (u32 b = 0; b < r.len; b++) buf[off + b] = rd8(r.addr + b); // alias-safe fallback
		}
		// FIRST-BODY idxtab WINDOW OVERLAY (MAPLECAST_IDXTAB_CHARSNAP): overlay body0's char-pass
		// idxtab window onto the just-written (HUD-pass) idxtab region — ONLY the [_idxWinOff,
		// +_idxWinLen) bytes, so Cable's band + the effect band + rectab stay HUD-pass/STARTRENDER.
		// This is the targeted Storm fix; rectab is NOT touched (finalized during the walk).
		if (r.addr == _idxtabAddr && r.len == _idxtabLen &&
		    _idxWinValid.load(std::memory_order_acquire) &&
		    _idxWinLen > 0 && _idxWinSnap.size() == _idxWinLen &&
		    (size_t)_idxWinOff + _idxWinLen <= r.len) {
			memcpy(&buf[off + _idxWinOff], _idxWinSnap.data(), _idxWinLen);
		}
		off += r.len;
	}
	if (snapLk.owns_lock()) snapLk.unlock();

	// ---- VARIABLE GFX tail: u32 nGfx, then per fresh body GFX region { base, len, len bytes } ----
	u32 nGfx = (u32)freshGfx.size();
	memcpy(&buf[off], &nGfx, 4); off += 4;
	for (auto& g : freshGfx) {
		memcpy(&buf[off], &g.base, 4); off += 4;               // page-aligned guest base (0x8C..)
		memcpy(&buf[off], &g.len,  4); off += 4;               // REAL region length (per-region)
		// GFX bytes: for GFX2 (g.src set) copy the CHARACTER-PASS snapshot — the LIVE self-modified
		// dispatch head + referenced records — so the client walker reads the correct pose (re_kb/60
		// Storm under-tile fix). For GFX1 (g.src==nullptr) copy live main-RAM (pixels don't self-modify).
		if (g.src) memcpy(&buf[off], g.src, g.len);
		else       memcpy(&buf[off], &mem_b[g.base & 0x00FFFFFFu], g.len);
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

	// ---- BTCW TAIL: u32 magic "BTCW", u32 nWords, then nWords u32 (per body [node][ntiles][tcw...]).
	// The engine's RESOLVED per-tile body tcws — render_frame uses them verbatim for body tiles,
	// killing the arena-parity flip. Strictly AFTER the HUDQ tail; absent when btcwWords==0. ----
	if (btcwWords) {
		memcpy(&buf[off], &BTCW_MAGIC, 4); off += 4;
		u32 nw = (u32)btcwWords; memcpy(&buf[off], &nw, 4); off += 4;
		memcpy(&buf[off], btcwBuf, btcwBytes); off += btcwBytes;
	}

	// ---- PL3D TAIL: u32 magic "PL3D", u32 nBytes, then nBytes of 36-byte flush records.
	// Strictly AFTER the BTCW tail (older clients skip unknown trailing bytes — same
	// pattern as every prior tail); absent when no 3D-machine flush fired this frame. ----
	if (p3dBytes) {
		memcpy(&buf[off], &PL3D_MAGIC, 4); off += 4;
		u32 nb = (u32)p3dBytes; memcpy(&buf[off], &nb, 4); off += 4;
		memcpy(&buf[off], p3dBuf, p3dTail); off += p3dTail;
	}

	// ---- state-wire v2 (opt-in): republish with the DYNAMIC block dirty-diffed vs the
	// last keyframe. The v1 buffer above is untouched; when MAPLECAST_STATEWIRE_V2 is off
	// this is skipped and the FRMx frame publishes exactly as before. The variable-length
	// v2 block replaces the raw dynamic bytes; the GFX/pal/HUD/BTCW/PL3D tails are copied
	// verbatim after it (the client finds them via statewire_v2 decodeV2Len). ----
	const uint8_t* pubPtr = buf.data();
	size_t         pubLen = total;
	if (statewireV2On()) {
		_v2FrameCtr++;
		bool isKey = _v2ForceKey.exchange(false, std::memory_order_relaxed)
		          || _v2Key.size() != _dynTotal
		          || (_v2FrameCtr % statewireV2KeyInterval() == 1);   // ctr==1 => first frame is a key
		const uint8_t* dynSrc = &buf[hdr];                 // the v1 raw dynamic block
		if (isKey) { _v2Key.assign(dynSrc, dynSrc + _dynTotal); _v2KeyId = _v2FrameCtr; }
		statewire_v2::encode(dynSrc, _dynTotal, _v2Key.data(), _v2KeyId, isKey, _v2Blk);
		const size_t tailOff = hdr + _dynTotal;            // GFX/pal/HUD/BTCW/PL3D tails start here
		const size_t tailLen = total - tailOff;
		const size_t v2total = hdr + _v2Blk.size() + tailLen;
		std::vector<uint8_t>& out = _v2Out[which];         // double-buffered like _dynBuf
		if (out.size() != v2total) out.resize(v2total);
		memcpy(&out[0], &buf[0], hdr);                     // 12B FRMx header ...
		u32 m2 = FRM2_MAGIC; memcpy(&out[0], &m2, 4);      // ... rewritten to FRM2
		memcpy(&out[hdr], _v2Blk.data(), _v2Blk.size());   // dirty-diffed dynamic block
		memcpy(&out[hdr + _v2Blk.size()], &buf[tailOff], tailLen);   // tails verbatim
		pubPtr = out.data();
		pubLen = v2total;
	}

	// ---- publish to the WS thread (drop-old: overwrite any undrained frame) ----
	{
		std::lock_guard<std::mutex> lk(_pubMutex);
		_pubPtr = pubPtr;
		_pubLen = pubLen;
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

// fwd-decls: the walk-snap helpers + idxtab body-band constants are defined below (with the
// tiledesc/table snapshot core) but are referenced by snapshotCharPassTables (the EFFECT-band merge).
static bool walkSnapActive();
static void snapRange(u32 addr, u32 fullLen, u32 off, u32 len, std::vector<uint8_t>& dst);
static const u32 IDXTAB_BODY_ENTRIES = 912;                       // body band = idxtab indices [0, 912)
static const u32 IDXTAB_BODY_BYTES   = IDXTAB_BODY_ENTRIES * 4;   // = 0xE40 bytes

// idxtab/rectab BODY-BAND WALK-INSTANT co-snapshot — DEFAULT OFF (MAPLECAST_IDXTAB_WALKSNAP).
// REGRESSED (2026-07-03, faithful raster): moving the idxtab body band to the walk instant gives
// WRONG tcws — BOTH bodies (incl. the previously-coherent Cable) render red/white striped texture
// garbage (wrong-texture-sampling). The idxtab must stay at STARTRENDER (snapshotCharPassTables).
// The TILEDESC walk-instant fix STAYS (count correct, Cable coherent). This env is an A/B lever
// only; keep it OFF for the good state (tiledesc-walk + idxtab-STARTRENDER).
static bool idxtabWalkSnapActive()
{
	static const bool on = []{
		const char* e = getenv("MAPLECAST_IDXTAB_WALKSNAP");
		return (e && e[0] == '1');   // default OFF; explicit "1" enables (A/B only)
	}();
	return on;
}

// CHARACTER-PASS TABLE SNAPSHOT (re_kb/50). Called from mc_oracle_charPassCapture ONLY on the
// CHARACTER pass. Snapshots the live idxtab/rectab (effect entries are written by the char-pass
// submit and reverted by the HUD pass) into side buffers that captureFrame ships instead of the
// HUD-pass-stale RAM. Read-only, 2 region memcpys; free when off / no client / not in-match /
// tables not built. Body entries are identical both passes — safe for bodies, fixes effects.
void snapshotCharPassTables()
{
	if (!_armed) return;
	if (_clientCount.load(std::memory_order_relaxed) == 0) return;
	if (rd8(IN_MATCH_ADDR) == 0) return;
	if (!_tablesBuilt) return;                 // addr/len set by buildTables (first onRenderFrame)
	if (!_idxtabAddr && !_rectabAddr) return;

	std::lock_guard<std::mutex> lk(_tablesSnapMutex);
	auto snap = [](u32 addr, u32 len, std::vector<uint8_t>& dst) {
		if (!addr || !len) return;
		if (dst.size() != len) dst.resize(len);
		if ((addr & 0xFF000000u) == 0x8C000000u)
			memcpy(dst.data(), &mem_b[addr & 0x00FFFFFFu], len);     // fast main-RAM path
		else
			for (u32 b = 0; b < len; b++) dst[b] = rd8(addr + b);   // alias-safe fallback
	};
	// DEFAULT (idxtab-STARTRENDER): snapshot the WHOLE idxtab + rectab here. This is the
	// BEST-CONFIRMED state — the idxtab body band MUST stay at STARTRENDER (the walk-instant
	// body-band co-snapshot REGRESSED both bodies to striped garbage; 2026-07-03 raster). The
	// TILEDESC walk-instant fix is independent and STAYS (snapshotWalkInstantTiledesc).
	//
	// A/B ONLY (MAPLECAST_IDXTAB_WALKSNAP=1): the walk-entry hook owns the BODY band [0,912) +
	// rectab, so here we snapshot ONLY the EFFECT band [912..] to avoid clobbering it. OFF by default.
	if (idxtabWalkSnapActive()) {
		if (_idxtabAddr && _idxtabLen && _idxtabLen > IDXTAB_BODY_BYTES)
			snapRange(_idxtabAddr, _idxtabLen, IDXTAB_BODY_BYTES,
			          _idxtabLen - IDXTAB_BODY_BYTES, _idxtabSnap);   // EFFECT band only
	} else {
		snap(_idxtabAddr, _idxtabLen, _idxtabSnap);                  // FULL idxtab (STARTRENDER)
		snap(_rectabAddr, _rectabLen, _rectabSnap);                  // FULL rectab (STARTRENDER)
	}
	// NOTE: the tiledesc (0x8C1F9F9C) is NOT snapshotted here. serverPublish runs AFTER the
	// HUD pass STARTRENDER, and the tiledesc is RESET-then-REBUILT from index 0 at the START
	// of EACH render pass (loc_8c0337bc stores base into cursor 0x8C1F9D98; loc_8c129728 fills
	// it — re_kb/22 finding:tile_descriptor_runtime_built). So at serverPublish the LOW/body
	// descriptor region has ALREADY been re-seeded for the HUD pass's minimal geometry -> a
	// serverPublish tiledesc snapshot is WORSE than live RAM (it SHATTERS moving bodies:
	// duplicated emblem/face, torn halves, confetti — MEASURED regression 2026-07-02). The
	// tiledesc is instead snapshotted at the CHARACTER pass STARTRENDER via
	// snapshotCharPassTiledesc() (below), the instant the walker's descriptors are live and
	// BEFORE the HUD pass rebuilds — the same instant the DUMP captures (dump gave 24/24 +
	// 49/49 both bodies byte-exact vs engine).
	_tablesSnapValid.store(true, std::memory_order_release);
}

// TILEDESC CHARACTER-PASS SNAPSHOT (2026-07-02, finding:tiledesc_snapshot_must_be_char_pass).
// Called from mc_oracle_charPassCapture ONLY on the CHARACTER pass (realBody-gated STARTRENDER),
// which fires BEFORE QueueRender and BEFORE the HUD pass resets+rebuilds 0x8C1F9F9C. At this
// instant the per-frame tile-descriptor table holds BOTH bodies' descriptors exactly as the
// SH4 body walk (loc_8c0344d4) left them (re_kb/22: table is frame-local, rebuilt per pass from
// index 0). captureFrame ships THIS instead of the HUD-pass-stale/rebuilt RAM. Gated env
// MAPLECAST_TILEDESC_CHARSNAP (default ON); set to "0" to A/B-disable (ships live tiledesc).
// idxtab BODY/EFFECT band split. The BODY tiles resolve tcw = rectab[idxtab[alloc_index]] with
// alloc_index = node+0xDC + arena_base + k — LOW indices (bodies' +0xDC 0..~50 + arena_base 16/400,
// so < ~500). The EFFECT scale walker uses alloc_index = 0x390 (912) + template read -> idxtab
// [912..1074] (re_kb/50, effect entries idxtab[972..1074]). So idxtab[0..IDXTAB_BODY_ENTRIES) is
// the BODY band; [912..] the EFFECT band. u32 entries -> byte offset = entry*4. (IDXTAB_BODY_ENTRIES
// / IDXTAB_BODY_BYTES are defined at the top of the Public API section.)

// Core tiledesc memcpy (0x8C1F9F9C, 0x1800B) into _tiledescSnap under the shared mutex.
// Marks _tiledescSnapValid. Shared by both the (legacy) STARTRENDER path and the walk-instant
// path. Caller must hold the arm/client/in-match/built gates.
static void doSnapshotTiledesc()
{
	std::lock_guard<std::mutex> lk(_tablesSnapMutex);
	if (_tiledescSnap.size() != TILEDESC_LEN) _tiledescSnap.resize(TILEDESC_LEN);
	if ((TILEDESC_ADDR & 0xFF000000u) == 0x8C000000u)
		memcpy(_tiledescSnap.data(), &mem_b[TILEDESC_ADDR & 0x00FFFFFFu], TILEDESC_LEN);
	else
		for (u32 b = 0; b < TILEDESC_LEN; b++) _tiledescSnap[b] = rd8(TILEDESC_ADDR + b);
	_tiledescSnapValid.store(true, std::memory_order_release);
}

// SLOT-TABLE + OBJPOOL CHARACTER-PASS SNAPSHOT. Called from mc_oracle_charPassCapture ONLY on the
// realBody-gated CHARACTER pass — the same instant snapshotCharPassTiledesc/Gfx2 fire — so the
// display list (slot_cnt/slot_ptr) and the objpool (+0x12C visibility bit + node+0xDC cursor) are
// captured EXACTLY as the render-walk loc_8c0308c2 consumed them, BEFORE the HUD pass rebuilds the
// table for its minimal geometry. captureFrame ships these three snapshots instead of the live
// (HUD-pass / serverPublish-instant) RAM, so a removed satellite's stale node+0x12C can never leak
// past the coherent slot_cnt (see the module-state comment block for the disasm cites). READ-ONLY,
// three region memcpys under the shared table-snapshot mutex; free when off / no client / not
// in-match / tables not built. Gated MAPLECAST_SLOT_CHARSNAP, DEFAULT ON (must be coherent with the
// DEFAULT-ON tiledesc/gfx2 char-pass snapshots — they are one render-walk view or none).
static bool slotCharSnapActive()
{
	static const bool on = []{
		const char* e = getenv("MAPLECAST_SLOT_CHARSNAP");
		return !(e && e[0] == '0');   // default ON; "0" A/B-disables (ships live/serverPublish RAM)
	}();
	return on;
}

void snapshotCharPassSlots()
{
	if (!slotCharSnapActive()) return;
	if (!_armed) return;
	if (_clientCount.load(std::memory_order_relaxed) == 0) return;
	if (rd8(IN_MATCH_ADDR) == 0) return;
	if (!_tablesBuilt) return;

	std::lock_guard<std::mutex> lk(_tablesSnapMutex);
	auto snap = [](u32 addr, u32 len, std::vector<uint8_t>& dst) {
		if (dst.size() != len) dst.resize(len);
		if ((addr & 0xFF000000u) == 0x8C000000u)
			memcpy(dst.data(), &mem_b[addr & 0x00FFFFFFu], len);     // fast main-RAM path
		else
			for (u32 b = 0; b < len; b++) dst[b] = rd8(addr + b);   // alias-safe fallback
	};
	snap(SLOTCNT_ADDR, SLOTCNT_LEN, _slotCntSnap);   // display-list counts (render-walk instant)
	snap(SLOTPTR_ADDR, SLOTPTR_LEN, _slotPtrSnap);   // display-list node ptrs (compacted this frame)
	snap(OBJPOOL_ADDR, OBJPOOL_LEN, _objpoolSnap);   // node fields incl +0x12C visibility, +0xDC cursor
	_slotSnapValid.store(true, std::memory_order_release);
}

// FIRST-BODY idxtab WINDOW snapshot — the targeted Storm fix. Called from the CHARACTER-pass
// STARTRENDER (mc_oracle_charPassCapture, realBody-gated) where body0's idxtab is WRITTEN (walk
// done) and the HUD pass has NOT yet clobbered it. Snapshots ONLY idxtab[arena_base+0 ..
// arena_base+ntiles0) into _idxWinSnap; captureFrame overlays it onto the shipped idxtab.
//   arena_base = *(0x8C1F9D94) (16 or 400, per-frame parity — read LIVE, not hardcoded).
//   body0      = the active char body with +0xDC==0 (the FIRST arena object).
//   ntiles0    = the smallest POSITIVE active-body +0xDC (== body0's tile count, since +0xDC is a
//                prefix-sum: next body's prefix = body0's ntiles). If body0 is the only active body,
//                ntiles0 = its walk-instant tiledesc tile count is unknown here without the cell walk,
//                so we fall back to a safe cap (the effect band start relative to arena_base).
// Gated MAPLECAST_IDXTAB_CHARSNAP (default OFF). rectab untouched (stays STARTRENDER).
static bool idxtabCharSnapActive()
{
	static const bool on = []{
		const char* e = getenv("MAPLECAST_IDXTAB_CHARSNAP");
		return (e && e[0] == '1');   // default OFF; explicit "1" enables (A/B)
	}();
	return on;
}
void snapshotCharPassIdxtabBody0Window()
{
	if (!idxtabCharSnapActive()) return;
	if (!_armed) return;
	if (_clientCount.load(std::memory_order_relaxed) == 0) return;
	if (rd8(IN_MATCH_ADDR) == 0) return;
	if (!_tablesBuilt || !_idxtabAddr || !_idxtabLen) return;

	static const u32 CB[6] = { 0x8C268340u, 0x8C2688E4u, 0x8C268E88u,
	                           0x8C26942Cu, 0x8C2699D0u, 0x8C269F74u };
	// arena_base LIVE (16 or 400 per-frame parity). read32 through addrspace (alias-safe).
	u32 arena = addrspace::read32(0x8C1F9D94u);
	// HARD GUARD: the engine only ever uses arena_base 16 or 400 (the two ping-pong halves). If we
	// read anything else, the char-pass instant is wrong / the value isn't live yet -> BAIL rather
	// than compute a garbage offset that could OOB. (This is also the diagnostic the coordinator asked
	// for: a garbage arena_base means the read instant is wrong.)
	if (arena != 16u && arena != 400u) {
		static bool s_warned = false;
		if (getenv("MAPLECAST_IDXTAB_CHARSNAP") && !s_warned) {
			s_warned = true;
			fprintf(stderr, "[IDXWIN] BAIL: arena_base=0x%08X (%u) not 16/400 at char pass — read instant wrong\n",
			        arena, arena);
		}
		_idxWinValid.store(false, std::memory_order_release);
		return;
	}
	// Find body0 (+0xDC==0, active) and ntiles0 = smallest positive active +0xDC.
	bool haveBody0 = false; u32 ntiles0 = 0xFFFFFFFFu;
	for (int i = 0; i < 6; i++) {
		if (rd8(CB[i] + 0x000) == 0) continue;                 // inactive slot
		u32 dc = (u32)addrspace::read16(CB[i] + 0xDC);
		if (dc == 0) haveBody0 = true;
		else if (dc < ntiles0) ntiles0 = dc;                   // next prefix = body0 tile count
	}
	if (!haveBody0) { _idxWinValid.store(false, std::memory_order_release); return; }
	if (ntiles0 == 0xFFFFFFFFu) ntiles0 = 64;                  // only body active: safe cap (< effect band)
	if (ntiles0 == 0 || ntiles0 > 256) ntiles0 = 256;         // clamp: never 0, never touch the effect band

	// window = idxtab entries [arena+0, arena+ntiles0) -> bytes [ (arena)*4, (arena+ntiles0)*4 ).
	// arena is now guaranteed 16 or 400 and ntiles0 in [1,256], so these can't overflow.
	u32 offBytes = arena * 4u;
	u32 lenBytes = ntiles0 * 4u;
	// HARD BOUNDS: never index past _idxtabLen (the buffer captureFrame ships) nor produce len 0.
	if (offBytes >= _idxtabLen) { _idxWinValid.store(false, std::memory_order_release); return; }
	if (offBytes + lenBytes > _idxtabLen) lenBytes = _idxtabLen - offBytes;
	if (lenBytes == 0) { _idxWinValid.store(false, std::memory_order_release); return; }

	std::lock_guard<std::mutex> lk(_tablesSnapMutex);
	if (_idxWinSnap.size() != lenBytes) _idxWinSnap.resize(lenBytes);
	// SOURCE read: use the alias-safe rd8 loop unconditionally. The fast mem_b path assumed a
	// 0x8C.. base masked to 16MB, but _idxtabAddr is POINTER-RESOLVED (rd32(0x8C2DAD3C)) and may be
	// a P0/0x0C.. alias or sit anywhere in area-3 — the raw &mem_b[..&0xFFFFFF] indexing was the
	// likely fault. addrspace::read8 resolves any alias correctly.
	for (u32 b = 0; b < lenBytes; b++) _idxWinSnap[b] = rd8(_idxtabAddr + offBytes + b);
	_idxWinOff = offBytes; _idxWinLen = lenBytes;
	_idxWinValid.store(true, std::memory_order_release);

	// ONE-TIME DIAGNOSTIC (the coordinator's [IDXWIN] values).
	if (getenv("MAPLECAST_IDXTAB_CHARSNAP")) {
		static bool s_printed = false;
		if (!s_printed) {
			s_printed = true;
			fprintf(stderr, "[IDXWIN] arena_base=%u ntiles0=%u off=%u len=%u idxtabLen=%u idxtabAddr=0x%08X\n",
			        arena, ntiles0, offBytes, lenBytes, _idxtabLen, _idxtabAddr);
		}
	}
}

// CHAR-PASS GFX2 SNAPSHOT (Storm under-tile fix, re_kb/60). Called from mc_oracle_charPassCapture
// ONLY on the CHARACTER pass (realBody-gated STARTRENDER) — the instant the engine's body walker
// (loc_8c0344d4) reads the SELF-MODIFIED GFX2 dispatch head. By serverPublish (the winning HUD-pass
// captureFrame) that head is reverted to the disc/default pose, so collectFreshGfx's live read froze
// the tile count. Here we snapshot each active body's GFX2 region (page-aligned base + real extent)
// while the head is live; collectFreshGfx/captureFrame source GFX2 from this snapshot so the correct
// (fresh) records ship EVERY frame the head changes. Default ON; MAPLECAST_GFX2_CHARSNAP=0 A/B-disables
// (reverts to the frozen live read). Render-thread only (same thread as captureFrame) -> lock-free.
static bool gfx2CharSnapActive()
{
	static const bool on = []{
		const char* e = getenv("MAPLECAST_GFX2_CHARSNAP");
		return !(e && e[0] == '0');   // default ON; explicit "0" disables (A/B)
	}();
	return on;
}
void snapshotCharPassGfx2()
{
	if (!gfx2CharSnapActive()) return;
	if (!_armed) return;
	if (_clientCount.load(std::memory_order_relaxed) == 0) return;
	if (rd8(IN_MATCH_ADDR) == 0) return;
	if (!_tablesBuilt) return;

	const u32 gen = ++_gfx2CharSnapGen;
	// Walk the slot table exactly like collectFreshGfx; snapshot each active body/effect node's GFX2
	// region at THIS char-pass instant (the self-modified dispatch head is live here).
	for (int L = 0; L < 16; L++) {
		u32 cnt = rd8(0x8C2895E0 + L); if (cnt == 0 || cnt > 0x60) continue;
		u32 base = 0x8C287DE0 + L * 0x180;
		for (u32 i = 0; i < cnt; i++) {
			u32 node = rd32(base + i * 4); if (!isRam(node)) continue;
			{ int cat = (int)(int8_t)rd8(node + 0x3); if (cat < 0 || cat >= 5) continue; }
			u32 gfx2 = rd32(node + 0x160); if (!isRam(gfx2)) continue;
			u32 pbase, plen; gfxShipDescriptor(gfx2, true, pbase, plen);
			if (!plen) continue;
			// find-or-add by page base (reuse the byte buffer to avoid per-frame realloc)
			Gfx2Snap* sn = nullptr;
			for (auto& s : _gfx2CharSnap) if (s.base == pbase) { sn = &s; break; }
			if (!sn) { _gfx2CharSnap.push_back(Gfx2Snap{ pbase, 0, 0, {} }); sn = &_gfx2CharSnap.back(); }
			if (sn->bytes.size() != plen) sn->bytes.resize(plen);
			if ((pbase & 0xFF000000u) == 0x8C000000u)
				memcpy(sn->bytes.data(), &mem_b[pbase & 0x00FFFFFFu], plen);  // fast main-RAM path
			else
				for (u32 b = 0; b < plen; b++) sn->bytes[b] = rd8(pbase + b); // alias-safe fallback
			sn->len = plen; sn->gen = gen;
		}
	}
	// Prune entries not refreshed this pass (swapped-out chars) so the vector stays bounded.
	_gfx2CharSnap.erase(std::remove_if(_gfx2CharSnap.begin(), _gfx2CharSnap.end(),
		[gen](const Gfx2Snap& s){ return s.gen != gen; }), _gfx2CharSnap.end());
}

// Snapshot a byte sub-range [off, off+len) of a table at `addr` into dst (dst is the FULL-table
// buffer; the sub-range is copied in place, other bytes untouched). Caller holds _tablesSnapMutex.
static void snapRange(u32 addr, u32 fullLen, u32 off, u32 len, std::vector<uint8_t>& dst)
{
	if (!addr || !fullLen) return;
	if (dst.size() != fullLen) dst.resize(fullLen);
	if (off >= fullLen) return;
	if (off + len > fullLen) len = fullLen - off;
	if ((addr & 0xFF000000u) == 0x8C000000u)
		memcpy(dst.data() + off, &mem_b[(addr + off) & 0x00FFFFFFu], len);
	else
		for (u32 b = 0; b < len; b++) dst[off + b] = rd8(addr + off + b);
}

// WALK-INSTANT idxtab/rectab BODY-BAND snapshot. The BODY idxtab band is char-pass-transient like
// the tiledesc COUNT bytes: the HUD pass rebuilds the tile arena from index 0 for its own (minimal)
// geometry, shifting the low idxtab entries the FIRST body's tiles resolve against — so a STARTRENDER
// idxtab snapshot maps the walk-instant tiledesc's alloc indices to the WRONG rectab entries
// (MEASURED: Storm first body scrambled — right tiles, wrong per-tile tcw). We snapshot the idxtab
// BODY band [0, 912) AND the whole rectab (rectab entries the body idxtab points at are anywhere in
// rectab; the effect submit only APPENDS high rectab entries during the walk, so the low/body rectab
// entries are already final at the walk instant) at the SAME walk instant as the tiledesc. The
// EFFECT idxtab band [912..] is left for the STARTRENDER path (written DURING the walk). Caller holds
// the arm/client/in-match/built gates.
static void doSnapshotWalkInstantTables()
{
	std::lock_guard<std::mutex> lk(_tablesSnapMutex);
	// idxtab BODY band only (preserve the effect band for the STARTRENDER snapshot).
	if (_idxtabAddr && _idxtabLen)
		snapRange(_idxtabAddr, _idxtabLen, 0, IDXTAB_BODY_BYTES, _idxtabSnap);
	// rectab: the whole table. Body idxtab entries can point anywhere; the effect submit only
	// APPENDS higher rectab slots during the walk, and those aren't referenced by body idxtab.
	if (_rectabAddr && _rectabLen)
		snapRange(_rectabAddr, _rectabLen, 0, _rectabLen, _rectabSnap);
	_tablesSnapValid.store(true, std::memory_order_release);
}

// Is the walk-instant tiledesc snapshot active? When it is (default), the STARTRENDER tiledesc
// path MUST NOT run — STARTRENDER fires LATER in the frame than the walker entry and its tiledesc
// is INFLATED (re-seeded), so it would CLOBBER the correct walk-entry snapshot. So the two paths
// are mutually exclusive: walk-snap ON => STARTRENDER tiledesc snapshot is a no-op.
static bool walkSnapActive()
{
	static const bool on = []{
		const char* e = getenv("MAPLECAST_TILEDESC_WALKSNAP");
		return !(e && e[0] == '0');   // default ON
	}();
	return on;
}

void snapshotCharPassTiledesc()
{
	if (walkSnapActive()) return;      // walk-instant path owns the tiledesc; STARTRENDER stale
	static const bool s_enabled = []{
		const char* e = getenv("MAPLECAST_TILEDESC_CHARSNAP");
		return !(e && e[0] == '0');   // default ON; "0" disables for A/B
	}();
	if (!s_enabled) return;
	if (!_armed) return;
	if (_clientCount.load(std::memory_order_relaxed) == 0) return;
	if (rd8(IN_MATCH_ADDR) == 0) return;
	if (!_tablesBuilt) return;
	doSnapshotTiledesc();
}

// WALK-INSTANT TILEDESC SNAPSHOT (2026-07-02, finding:tiledesc_walk_instant_snapshot). Called
// from the oracle hook at the BODY-WALKER ENTRY (loc_8c0344d4, PC 0x8C0344D4), on the FIRST
// char-body walk of the video frame. GROUND-TRUTH REASON (TDTILE capture, Storm vf46489): the
// per-record tiledesc byte1 the walk READS sums to the +0xDC budget (24 tiles/7 records), i.e.
// it is CORRECT at the walk. By STARTRENDER (where snapshotCharPassTiledesc fires) the SAME
// records read INFLATED byte1 (sum 59) — the table is re-seeded/rebuilt for the HUD pass
// (loc_8c0337bc/loc_8c129728, re_kb/22). The tiledesc ENTRIES are READ-ONLY during the walk
// (r13 only advances +4/tile, no writes), so the walker-entry state IS the authoritative
// pre-consumption b1. This overrides the STARTRENDER tiledesc: whichever fires, captureFrame
// ships _tiledescSnap; the walk-entry value is the correct one and it is written LAST relative
// to STARTRENDER in program order per frame. idxtab/rectab are NOT snapshotted here — their
// EFFECT entries are WRITTEN by the per-tile submit DURING the walk (re_kb/50, line 80), so
// they remain correct only at/after STARTRENDER (snapshotCharPassTables, EFFECT band). Gated
// MAPLECAST_TILEDESC_WALKSNAP (default ON); "0" reverts to the STARTRENDER-only behavior.
//
// idxtab/rectab BODY BAND: co-snapshotted here ONLY under MAPLECAST_IDXTAB_WALKSNAP=1 (DEFAULT
// OFF). That experiment REGRESSED both bodies to striped texture garbage (2026-07-03 raster) — the
// idxtab must stay at STARTRENDER (snapshotCharPassTables, full table). So by default this function
// snapshots ONLY the tiledesc. The idxtab A/B lever is retained but off.
void snapshotWalkInstantTiledesc()
{
	if (!walkSnapActive()) return;   // "0" A/B-disables (STARTRENDER path only)
	if (!_armed) return;
	if (_clientCount.load(std::memory_order_relaxed) == 0) return;
	if (rd8(IN_MATCH_ADDR) == 0) return;
	if (!_tablesBuilt) return;
	doSnapshotTiledesc();
	if (idxtabWalkSnapActive())
		doSnapshotWalkInstantTables();   // A/B ONLY (default OFF): idxtab body band [0,912) + rectab

	// ONE-SHOT VERIFY (gated MAPLECAST_WALKSNAP_VERIFY): after the FIRST successful walk-instant
	// snapshot, print Storm's (node 0x8C268340) first-record tiledesc byte1 values + the per-record
	// (b1+1) sum FROM THE SNAPSHOT. It MUST show small b1 (walk-instant, sum ~24-38), NOT the
	// STARTRENDER-inflated ~57. r13 base = node+0xDC (prefix) into the tiledesc @0x8C1F9F9C.
	if (getenv("MAPLECAST_WALKSNAP_VERIFY") != nullptr) {
		static bool s_printed = false;
		if (!s_printed) {
			s_printed = true;
			const u32 NODE = 0x8C268340u;
			u32 gfx2 = rd32(NODE + 0x160);
			u32 sid  = (u32)addrspace::read16(NODE + 0x144) & 0x7FFFu;
			u32 dc   = (u32)addrspace::read16(NODE + 0xDC);
			if ((gfx2 & 0xFF000000u) && gfx2 >= 0x8C000000u && gfx2 < 0x8D000000u) {
				u32 cell = gfx2 + rd32(gfx2 + sid * 4);
				u32 rc   = (u32)addrspace::read16(cell);
				u32 r13 = dc, sum = 0; char buf[256]; int off = 0;
				for (u32 r = 0; r < rc && r < 12; r++) {
					// read b1 FROM THE SNAPSHOT (not live RAM). _tiledescSnap[0] == TILEDESC_ADDR,
					// so the entry offset is r13*4 (no base add). byte1 is at +1.
					u32 idx = r13 * 4 + 1;
					u32 b1  = (idx < _tiledescSnap.size()) ? _tiledescSnap[idx] : 0xFFu;
					off += snprintf(buf + off, sizeof(buf) - off, "%u ", b1 + 1);
					sum += b1 + 1; r13 += b1 + 1;
				}
				fprintf(stderr, "[WALKSNAP-VERIFY] Storm sid=%u dc=%u recCount=%u first-record (b1+1): %s "
				                "=> sum(first %u)=%u  (walk-instant expects ~24-38, NOT ~57)\n",
				        sid, dc, rc, buf, (rc < 12 ? rc : 12), sum);
				// idxtab BODY-band consistency: dump the first 12 body idxtab entries as SNAPSHOTTED
				// at the walk (should be self-consistent with the tiledesc). The coordinator compares
				// this against a STARTRENDER dump; if they DIFFER, the HUD-pass idxtab was scrambling
				// the first body — which this walk-instant body-band snapshot now fixes.
				if (_idxtabAddr && _idxtabSnap.size() >= 48) {
					char ib[256]; int io = 0;
					for (u32 e = 0; e < 12; e++) {
						u32 v = (u32)_idxtabSnap[e*4] | ((u32)_idxtabSnap[e*4+1] << 8)
						      | ((u32)_idxtabSnap[e*4+2] << 16) | ((u32)_idxtabSnap[e*4+3] << 24);
						io += snprintf(ib + io, sizeof(ib) - io, "%u ", v);
					}
					fprintf(stderr, "[WALKSNAP-VERIFY] idxtab body[0..11] @walk: %s\n", ib);
				}
			}
		}
	}
}

void onRenderFrame(void* ctxv)
{
	// HOT PATH GATE (free when off / idle): not armed, or no client connected.
	if (!_armed) return;
	// A2 RUN-AHEAD: capture ONLY leg3 (the published N+1 frame) for the native client's bodies.
	// The hidden (N) + lookahead (N+1-build) legs must not reach the client. Context-stamped +
	// race-free — mirrors serverPublish's /ws gate (mirror.cpp:2263). Correctness also needs
	// MAPLECAST_RA_SHORT_LEG3 (default ON) so an N+2-building SR can't clobber the drop-old publish.
	if (maplecast_mirror::ctxIsHiddenLeg(ctxv)) return;
	// STATE-MERGE: when the body feed is folded into the main ZCS2/ZCST wire (MAPLECAST_STATE_MERGE),
	// serverPublish reads currentStatePayload() to append the STAT section — so capture MUST run for
	// the main-wire client even with NO /replica-live (:7212) client connected. Without this override,
	// dropping the :7212 socket (the whole point of the fold) would stop the body capture entirely.
	static const bool _stateMergeCap = [](){ const char* e = std::getenv("MAPLECAST_STATE_MERGE");
		return e && e[0] && e[0] != '0'; }();
	if (_clientCount.load(std::memory_order_relaxed) == 0 && !_stateMergeCap) return;

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
	// A2 dedup (belt): publish each guest vframe at most once, so a second non-hidden SR can't
	// re-ship the same frame via drop-old. Transparent when run-ahead is off (vframe advances +1).
	static u32 _lastPubVframe = 0xFFFFFFFFu;
	if (vframe == _lastPubVframe) return;
	_lastPubVframe = vframe;
	captureFrame(vframe);
}

// STATE-MERGE accessor — see the header. Returns this frame's just-built FRMx body-state payload
// (set at the end of captureFrame) so serverPublish can append it as a "STAT" section to the frame
// both wires already carry. Render-thread only; the buffer is stable for this frame (double-buffer
// swaps next frame). Brief lock to read the published pointer coherently vs the WS sender thread.
bool currentStatePayload(const uint8_t*& payload, size_t& len)
{
	std::lock_guard<std::mutex> lk(_pubMutex);
	if (!_pubPtr || _pubLen == 0) { payload = nullptr; len = 0; return false; }
	payload = _pubPtr;
	len     = _pubLen;
	return true;
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
		// TCP_NODELAY (disable Nagle) on every accepted socket: each 60fps frame is a small
		// message that must leave immediately. Without this, Nagle batches frames into bursts
		// -> the client sees uneven inter-frame gaps (jagged motion). Pairs with the client
		// jitter buffer: a smoother source lets that buffer (and its latency) stay small.
		_ws.set_socket_init_handler(
			[](websocketpp::connection_hdl, websocketpp::lib::asio::ip::tcp::socket& s) {
				websocketpp::lib::asio::error_code ec;
				s.set_option(websocketpp::lib::asio::ip::tcp::no_delay(true), ec);
			});
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

bool hasClients() { return _armed && _clientCount.load(std::memory_order_relaxed) > 0; }

} // namespace maplecast_replica_live
