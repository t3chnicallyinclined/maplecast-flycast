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
// table, and for each active BODY node ship its GFX1(+0x15C)/GFX2(+0x160) page-aligned 0x20000
// regions as a VARIABLE dynamic tail — but ONLY when that (base,content-signature) has not been
// shipped before (covers both the never-shipped case and a re-load that changed the bytes). The
// signature is a cheap fold over the region so a static (already-fresh) body costs nothing after
// its first send. Wire cost: ~121KB zstd per (re)loaded body, ~0 steady-state.
struct GfxBase { u32 base; u32 sig; };                 // base addr (page-aligned) + content sig
static std::vector<GfxBase> _gfxShipped;               // bases+sigs already sent to all clients
static std::atomic<bool>    _gfxResendAll{false};      // set on new-connect: re-ship all GFX once
static u32 gfxSig(u32 base)                            // fold the 0x20000 region to a u32 sig
{
	u32 h = 2166136261u;
	for (u32 b = 0; b < 0x20000u; b += 64) {          // sparse sample (every 64B) — enough to
		h ^= rd32(base + b); h *= 16777619u;          // catch a content change without a full scan
	}
	return h;
}
static bool gfxAlreadyShipped(u32 base, u32 sig)
{
	for (auto& g : _gfxShipped) if (g.base == base) return g.sig == sig;
	return false;
}
static void gfxMarkShipped(u32 base, u32 sig)
{
	for (auto& g : _gfxShipped) if (g.base == base) { g.sig = sig; return; }
	_gfxShipped.push_back({ base, sig });
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
	D(0x8C268340, 6u*0x5A4u,   "char_str");      // P1C1..P2C3 char structs
	D(0x8C1F9D80, 0x20,        "arena");         // arena-control globals
	D(0x8C1F9F9C, 0x1800,      "tiledesc");      // per-frame tile-descriptor scratch
	                                             // (live descriptor table spans 5135B —
	                                             //  0x200 truncated it; 0x1800 covers it)
	D(0x8C2D6AD8, 0xC0,        "cam_mat");       // camera matrices M2/M1
	D(0x8C26A510, 0x40,        "camZ");          // camera-Z scale block
	D(0x8C26823C, 0x04,        "ggp_ptr");       // GameGlobalPointer
	D(0x8C268240, 0x40,        "ggp_acc");       // *(GGP) global-accum struct
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
			if (isRam(GFX2)) S(GFX2 & ~0xFFFu, 0x20000, "GFX2");
			if (isRam(GFX1)) S(GFX1 & ~0xFFFu, 0x20000, "GFX1");
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
		// Fast path for the 16MB RAM backdrop: copy from mem_b directly (the region
		// addr 0x8C000000 maps to mem_b[0]). For the GFX regions use addrspace reads
		// (alias-safe; modest size 0x20000 each).
		if (r.addr == 0x8C000000u) {
			memcpy(&p[off], &mem_b[0], r.len);
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
struct GfxToShip { u32 base; u32 sig; };
static void collectFreshGfx(std::vector<GfxToShip>& out)
{
	for (int L = 0; L < 16; L++) {
		u32 cnt = rd8(0x8C2895E0 + L); if (cnt == 0 || cnt > 0x60) continue;
		u32 base = 0x8C287DE0 + L * 0x180;
		for (u32 i = 0; i < cnt; i++) {
			u32 node = rd32(base + i * 4); if (!isRam(node)) continue;
			if (rd8(node + 0x3) != 0) continue;                 // body only (cat==0)
			u32 gfx[2] = { rd32(node + 0x160), rd32(node + 0x15C) };  // GFX2, GFX1
			for (u32 k = 0; k < 2; k++) {
				if (!isRam(gfx[k])) continue;
				u32 pbase = gfx[k] & ~0xFFFu;
				u32 sig = gfxSig(pbase);
				if (gfxAlreadyShipped(pbase, sig)) continue;     // already fresh on the client
				bool dup = false;
				for (auto& g : out) if (g.base == pbase) { dup = true; break; }
				if (!dup) out.push_back({ pbase, sig });
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

	// Determine this frame's fresh-GFX delta (on-change dynamic GFX, the static-gap fix).
	std::vector<GfxToShip> freshGfx;
	collectFreshGfx(freshGfx);
	const u32 GFX_REGION = 0x20000u;

	// Pick the staging buffer NOT currently published (double-buffer). Inner payload =
	//   12B FRAME RECORD header + fixed dynamic bytes + VARIABLE GFX tail, where the tail is:
	//     u32 nGfx ; then nGfx × { u32 base ; GFX_REGION bytes }.
	// nGfx is ALWAYS present (0 in the steady state — a 4B tail). The client reads it after the
	// fixed dynamic regions (it knows _dynTotal from the region table) and splats each block.
	const size_t hdr   = 12;
	const size_t tailHdr = 4;                                  // u32 nGfx
	const size_t gfxBytes = freshGfx.size() * (4 + (size_t)GFX_REGION);
	const size_t total = hdr + _dynTotal + tailHdr + gfxBytes;

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

	// ---- VARIABLE GFX tail: u32 nGfx, then per fresh body GFX region { base, 0x20000 bytes } ----
	u32 nGfx = (u32)freshGfx.size();
	memcpy(&buf[off], &nGfx, 4); off += 4;
	for (auto& g : freshGfx) {
		memcpy(&buf[off], &g.base, 4); off += 4;               // page-aligned guest base (0x8C..)
		// GFX is main-RAM; copy from mem_b directly (alias-safe identical to addrspace here).
		memcpy(&buf[off], &mem_b[g.base & 0x00FFFFFFu], GFX_REGION);
		off += GFX_REGION;
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
		if (!_prefixReady.load(std::memory_order_acquire)) buildPrefixLocked();
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
		// Prefix: up to VRAM(8MB)+PVR(32KB)+RAM(16MB)+~256KB GFX ≈ 24.3MB worst case.
		_prefixComp.init((size_t)28 * 1024 * 1024);
		// Per-frame dynamic payload (Phase 5: STATE ONLY, no texture band) ≈ slot tables +
		// char structs + tiledesc(0x1800) + idxtab(0x2000) + rectab(0x10000) + camera/globals
		// ≈ 90-110KB raw worst case; compresses to GSTA-size. PLUS the on-change GFX tail: up to
		// ~4 bodies × 2 regions × 0x20000 = 1MB on a resend-all frame (new connect / multi-char
		// load), ~0 in steady state. 6MB init covers the worst case with headroom.
		_frameComp.init((size_t)6 * 1024 * 1024);
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
