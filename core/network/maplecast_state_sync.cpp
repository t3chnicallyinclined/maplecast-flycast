/*
	================================================================
	SHELVED 2026-04-09 — superseded by GGPO peer mode
	================================================================
	Companion to maplecast_player.cpp (see SHELVED block at the top
	of that file for full rationale). This is the bespoke TCP state-
	sync that hands a one-shot dc_serialize savestate to a connecting
	player client. It is being replaced by GGPO's existing
	save_game_state / load_game_state callbacks in
	core/network/ggpo.cpp, which already use dc_serialize/
	dc_deserialize and are integrated with GGPO's rollback ring.

	Still compiled and called from maplecast_player::init() and
	maplecast_mirror::initServer(). Held as a fallback until GGPO
	peer mode is proven end-to-end. Do not add features here.
	================================================================

	MapleCast State Sync — server + client implementations.

	See maplecast_state_sync.h for the wire format and rationale. Both
	endpoints live in this file because they share the STAT envelope
	parser and because neither is big enough to deserve its own TU.

	Server
	  - Listens on TCP port 7102
	  - accept thread: accepts clients, pushes them onto a shared list
	  - send thread: per-client blocking send of queued STAT blobs
	  - onServerFramePublished(frame): called from the emu thread (inside
	    maplecast_mirror::serverPublish, right after the frame counter is
	    bumped). Every STATE_SYNC_INTERVAL frames it calls
	    buildFullSaveState + MirrorCompressor::compress and queues the
	    blob to every connected client.
	  - On accept, the send thread immediately queues a STAT blob with
	    the STAT_FLAG_INITIAL bit set so late-joining clients catch up.

	Client
	  - Connects to <host>:7102 in a background thread (retries on failure)
	  - Receive thread parses STAT envelopes into a single "pending slot"
	    — if a newer state arrives before the old one is applied, it
	    replaces it (latest-wins — we only ever want the freshest)
	  - clientApplyPending() is called from the emu thread (inside
	    maplecast_player::frameGate) before tape inputs are applied. If
	    there's a pending state and the client hasn't done the initial
	    sync yet OR the pending state's serverFrame is >= our local
	    frame counter, it dc_deserialize's the payload and reseeds the
	    local frame counter.

	Threading / safety
	  - All state-mutating work (buildFullSaveState on server,
	    dc_deserialize on client) happens synchronously on the emu
	    thread. Network threads only marshal bytes.
	  - buildFullSaveState must NOT race with the SH4 — it's called from
	    inside serverPublish which itself runs on the emu thread between
	    runInternal() calls, so this is naturally safe.
*/
#include "types.h"
#include "maplecast_state_sync.h"
#include "maplecast_mirror.h"
#include "maplecast_compress.h"
#include "maplecast_player.h"
#include "serialize.h"
#include "emulator.h"

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <chrono>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>

namespace maplecast_state_sync
{

static inline int64_t nowUs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

// Read exactly `n` bytes into `buf` from `fd`. Returns true on success,
// false on EOF / error. Loops over partial reads — TCP is a byte stream.
static bool readExact(int fd, void* buf, size_t n)
{
	uint8_t* p = (uint8_t*)buf;
	while (n > 0) {
		ssize_t r = recv(fd, p, n, 0);
		if (r == 0) return false;
		if (r < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		p += r;
		n -= (size_t)r;
	}
	return true;
}

// Write exactly `n` bytes from `buf` to `fd`.
static bool writeExact(int fd, const void* buf, size_t n)
{
	const uint8_t* p = (const uint8_t*)buf;
	while (n > 0) {
		ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
		if (w < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		p += w;
		n -= (size_t)w;
	}
	return true;
}

// =====================================================================
//                               SERVER
// =====================================================================

namespace server {

struct OutboundBlob {
	uint64_t              serverFrame;
	uint8_t               flags;
	std::vector<uint8_t>  payload;   // already compressed (or raw if comp failed)
};

struct ClientConn {
	int                             fd;
	std::thread                     sendThread;
	std::mutex                      qmu;
	std::condition_variable         qcv;
	std::deque<OutboundBlob>        q;
	std::atomic<bool>               alive{true};
	// Phase 3 v2: per-client initial-sync gate. Set to true on accept,
	// cleared on first STAT send. onServerFramePublished only builds a
	// state when at least one client has this flag set, so steady-state
	// sessions cost zero buildFullSaveState calls.
	std::atomic<bool>               needsInitialSync{true};
};

static std::atomic<bool>                       s_running{false};
static int                                     s_listenFd = -1;
static std::thread                             s_acceptThread;
static std::mutex                              s_clientsMu;
static std::vector<ClientConn*>                s_clients;

// Telemetry
static std::atomic<uint64_t> s_statesBuilt{0};
static std::atomic<uint64_t> s_statesSent{0};
static std::atomic<uint64_t> s_totalBytesSent{0};
static std::atomic<uint64_t> s_totalBytesBeforeComp{0};
static std::atomic<uint64_t> s_lastBuildUs{0};
static std::atomic<uint64_t> s_lastCompressUs{0};
static std::atomic<uint64_t> s_lastCompressedSize{0};
static std::atomic<uint64_t> s_lastRawSize{0};
// Persistent compressor. buildFullSaveState typically returns 2-5 MB
// on MVC2; 16 MB input budget is plenty of headroom.
static MirrorCompressor s_comp;
static bool             s_compInit = false;

static void sendLoop(ClientConn* c)
{
	char threadName[64];
	snprintf(threadName, sizeof(threadName), "[state-sync] send fd=%d", c->fd);
	printf("%s started\n", threadName);

	while (c->alive.load(std::memory_order_relaxed) && s_running.load(std::memory_order_relaxed))
	{
		OutboundBlob blob;
		{
			std::unique_lock<std::mutex> lock(c->qmu);
			c->qcv.wait(lock, [&]{
				return !c->q.empty() || !c->alive.load(std::memory_order_relaxed)
				                     || !s_running.load(std::memory_order_relaxed);
			});
			if (!c->alive.load(std::memory_order_relaxed) || !s_running.load(std::memory_order_relaxed))
				break;
			blob = std::move(c->q.front());
			c->q.pop_front();
		}

		StatHeader hdr{};
		hdr.magic       = STAT_MAGIC;
		hdr.version     = STAT_VERSION;
		hdr.flags       = blob.flags;
		hdr.reserved0   = 0;
		hdr.serverFrame = blob.serverFrame;
		hdr.payloadLen  = (uint32_t)blob.payload.size();
		hdr.reserved1   = 0;

		if (!writeExact(c->fd, &hdr, sizeof(hdr))) break;
		if (!writeExact(c->fd, blob.payload.data(), blob.payload.size())) break;

		s_statesSent.fetch_add(1, std::memory_order_relaxed);
		s_totalBytesSent.fetch_add(sizeof(hdr) + blob.payload.size(),
		                           std::memory_order_relaxed);
	}

	printf("%s stopped\n", threadName);
	c->alive.store(false, std::memory_order_relaxed);
	::shutdown(c->fd, SHUT_RDWR);
	close(c->fd);
	c->fd = -1;
}

// Build+compress+enqueue a fresh state for every connected client.
// Called on the emu thread from onServerFramePublished. The snapshot is
// taken ONCE and the resulting payload is shared across all clients'
// queues via vector copy (small — typically ~1 MB compressed).
static void broadcastFreshState(uint64_t frame, bool initialFlag)
{
	// 1. Build
	auto t0 = std::chrono::high_resolution_clock::now();
	size_t rawSize = 0;
	uint8_t* raw = maplecast_mirror::buildFullSaveState(rawSize);
	if (!raw || rawSize == 0) {
		printf("[state-sync] buildFullSaveState failed\n");
		return;
	}
	auto t1 = std::chrono::high_resolution_clock::now();
	uint64_t buildUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

	// 2. Compress via the shared MirrorCompressor. Lazy-init to match
	// the largest raw size we've seen so ZSTD_compressBound doesn't
	// reject oversized inputs.
	if (!s_compInit) {
		s_comp.init(std::max<size_t>(rawSize * 2, 16 * 1024 * 1024));
		s_compInit = true;
	}
	size_t compSize = 0;
	uint64_t compressUs = 0;
	const uint8_t* compPtr = s_comp.compress(raw, (uint32_t)rawSize,
	                                         compSize, compressUs, /*level=*/1);

	// 3. Decide compressed vs raw. MirrorCompressor falls back to raw
	// if zstd errored — in that case compPtr == raw and the ZCST magic
	// isn't present.
	bool isCompressed = (compPtr != raw);

	OutboundBlob blob;
	blob.serverFrame = frame;
	blob.flags       = (isCompressed ? STAT_FLAG_COMPRESSED : 0)
	                 | (initialFlag  ? STAT_FLAG_INITIAL    : 0);
	blob.payload.assign(compPtr, compPtr + compSize);

	free(raw);

	s_statesBuilt.fetch_add(1, std::memory_order_relaxed);
	s_lastBuildUs.store(buildUs, std::memory_order_relaxed);
	s_lastCompressUs.store(compressUs, std::memory_order_relaxed);
	s_lastCompressedSize.store(compSize, std::memory_order_relaxed);
	s_lastRawSize.store(rawSize, std::memory_order_relaxed);
	s_totalBytesBeforeComp.fetch_add(rawSize, std::memory_order_relaxed);

	// 4. Fan out — ONLY to clients that still need an initial sync.
	// Clients that already received their one-shot are skipped, and
	// their `needsInitialSync` flag is cleared atomically here so the
	// next onServerFramePublished call doesn't re-build for them.
	std::lock_guard<std::mutex> lock(s_clientsMu);
	for (ClientConn* c : s_clients) {
		if (!c->alive.load(std::memory_order_relaxed)) continue;
		if (!c->needsInitialSync.load(std::memory_order_relaxed)) continue;
		{
			std::lock_guard<std::mutex> qlock(c->qmu);
			c->q.push_back(blob);
		}
		c->qcv.notify_one();
		c->needsInitialSync.store(false, std::memory_order_relaxed);
	}
}

static void acceptLoop()
{
	printf("[state-sync] accept thread started on port %d\n", STATE_PORT);
	while (s_running.load(std::memory_order_relaxed))
	{
		sockaddr_in cli{};
		socklen_t clen = sizeof(cli);
		int cfd = accept(s_listenFd, (sockaddr*)&cli, &clen);
		if (cfd < 0) {
			if (errno == EINTR) continue;
			if (!s_running.load(std::memory_order_relaxed)) break;
			// EBADF on close during shutdown — bail.
			if (errno == EBADF || errno == EINVAL) break;
			printf("[state-sync] accept error: %s\n", strerror(errno));
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		int one = 1;
		setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

		char ipstr[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &cli.sin_addr, ipstr, sizeof(ipstr));
		printf("[state-sync] client connected: %s:%u (fd=%d)\n",
		       ipstr, ntohs(cli.sin_port), cfd);

		ClientConn* c = new ClientConn{};
		c->fd = cfd;
		// needsInitialSync defaults to true in the struct, so the next
		// onServerFramePublished call (on the emu thread) will build a
		// state and ship it to this client exactly once.
		{
			std::lock_guard<std::mutex> lock(s_clientsMu);
			s_clients.push_back(c);
		}
		c->sendThread = std::thread(sendLoop, c);
	}
	printf("[state-sync] accept thread stopped\n");
}

} // namespace server

bool serverStart()
{
	using namespace server;
	if (s_running.load()) return true;

	s_listenFd = socket(AF_INET, SOCK_STREAM, 0);
	if (s_listenFd < 0) {
		printf("[state-sync] socket failed: %s\n", strerror(errno));
		return false;
	}
	int one = 1;
	setsockopt(s_listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	sockaddr_in addr{};
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port        = htons(STATE_PORT);
	if (bind(s_listenFd, (sockaddr*)&addr, sizeof(addr)) < 0) {
		printf("[state-sync] bind port %d failed: %s\n", STATE_PORT, strerror(errno));
		close(s_listenFd);
		s_listenFd = -1;
		return false;
	}
	if (listen(s_listenFd, 8) < 0) {
		printf("[state-sync] listen failed: %s\n", strerror(errno));
		close(s_listenFd);
		s_listenFd = -1;
		return false;
	}

	s_running.store(true);
	s_acceptThread = std::thread(server::acceptLoop);
	printf("[state-sync] === SERVER READY === port %d\n", STATE_PORT);
	return true;
}

void serverStop()
{
	using namespace server;
	if (!s_running.load()) return;
	s_running.store(false);

	if (s_listenFd >= 0) {
		::shutdown(s_listenFd, SHUT_RDWR);
		close(s_listenFd);
		s_listenFd = -1;
	}
	if (s_acceptThread.joinable()) s_acceptThread.join();

	std::lock_guard<std::mutex> lock(s_clientsMu);
	for (ClientConn* c : s_clients) {
		c->alive.store(false, std::memory_order_relaxed);
		c->qcv.notify_all();
		if (c->sendThread.joinable()) c->sendThread.join();
		delete c;
	}
	s_clients.clear();

	printf("[state-sync] server stopped\n");
}

void onServerFramePublished(uint64_t frame)
{
	if (!server::s_running.load(std::memory_order_relaxed)) return;

	// Phase 3 v2: only build a state when at least one connected client
	// is still waiting for its initial sync. Steady-state sessions
	// (everybody synced) cost ZERO buildFullSaveState calls per frame —
	// the input tape is the only thing keeping clients in sync. This
	// completely eliminates the periodic-heartbeat thrashing that
	// starved the emu thread in the previous design.
	bool anyNeedsSync = false;
	{
		std::lock_guard<std::mutex> lock(server::s_clientsMu);
		for (const auto* c : server::s_clients) {
			if (c->alive.load(std::memory_order_relaxed)
			 && c->needsInitialSync.load(std::memory_order_relaxed)) {
				anyNeedsSync = true;
				break;
			}
		}
	}
	if (!anyNeedsSync) return;

	server::broadcastFreshState(frame, /*initialFlag=*/true);
}

ServerStats getServerStats()
{
	using namespace server;
	ServerStats out{};
	out.running  = s_running.load(std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(s_clientsMu);
		out.clientCount = (uint32_t)s_clients.size();
	}
	out.statesBuilt          = s_statesBuilt.load(std::memory_order_relaxed);
	out.statesSent           = s_statesSent.load(std::memory_order_relaxed);
	out.totalBytesSent       = s_totalBytesSent.load(std::memory_order_relaxed);
	out.totalBytesBeforeComp = s_totalBytesBeforeComp.load(std::memory_order_relaxed);
	out.lastBuildUs          = s_lastBuildUs.load(std::memory_order_relaxed);
	out.lastCompressUs       = s_lastCompressUs.load(std::memory_order_relaxed);
	out.lastCompressedSize   = s_lastCompressedSize.load(std::memory_order_relaxed);
	out.lastRawSize          = s_lastRawSize.load(std::memory_order_relaxed);
	return out;
}

// =====================================================================
//                               CLIENT
// =====================================================================

namespace client {

struct PendingState {
	uint64_t              serverFrame;
	uint8_t               flags;
	std::vector<uint8_t>  payload;   // as-received, may be ZCST-compressed
};

static std::atomic<bool>   c_running{false};
static std::atomic<bool>   c_connected{false};
static std::string         c_host;
static std::thread         c_thread;
static int                 c_fd = -1;

static std::mutex          c_pendingMu;
static PendingState        c_pending;
static bool                c_hasPending = false;
static bool                c_everSynced = false;

// Telemetry
static std::atomic<uint64_t> c_statesReceived{0};
static std::atomic<uint64_t> c_statesApplied{0};
static std::atomic<uint64_t> c_bytesReceived{0};
static std::atomic<uint64_t> c_lastApplyUs{0};
static std::atomic<uint64_t> c_lastDecompressUs{0};
static std::atomic<uint64_t> c_lastAppliedFrame{0};
static std::atomic<int64_t>  c_lastReceiveUs{0};

// Shared decompressor on the apply path. dc_deserialize runs on the
// emu thread, and so does clientApplyPending, so this is single-threaded.
static MirrorDecompressor c_decomp;
static bool               c_decompInit = false;

static int openTcpBlocking(const std::string& host, int port)
{
	struct addrinfo hints{};
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	struct addrinfo* res = nullptr;
	int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
	if (rc != 0 || !res) {
		printf("[state-sync] client getaddrinfo('%s') failed: %s\n",
		       host.c_str(), gai_strerror(rc));
		return -1;
	}
	sockaddr_in sa;
	memcpy(&sa, res->ai_addr, sizeof(sa));
	sa.sin_port = htons((uint16_t)port);
	freeaddrinfo(res);

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	if (connect(fd, (sockaddr*)&sa, sizeof(sa)) < 0) {
		close(fd);
		return -1;
	}
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return fd;
}

static void rxLoop()
{
	printf("[state-sync] client rx thread started, target %s:%d\n",
	       c_host.c_str(), STATE_PORT);

	while (c_running.load(std::memory_order_relaxed))
	{
		// Reconnect loop. Back off 500ms between attempts.
		c_fd = openTcpBlocking(c_host, STATE_PORT);
		if (c_fd < 0) {
			c_connected.store(false, std::memory_order_relaxed);
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			continue;
		}
		c_connected.store(true, std::memory_order_relaxed);
		printf("[state-sync] client connected to %s:%d (fd=%d)\n",
		       c_host.c_str(), STATE_PORT, c_fd);

		while (c_running.load(std::memory_order_relaxed)) {
			StatHeader hdr;
			if (!readExact(c_fd, &hdr, sizeof(hdr))) break;
			if (hdr.magic != STAT_MAGIC) {
				printf("[state-sync] client: bad STAT magic %08x, reconnecting\n",
				       hdr.magic);
				break;
			}
			if (hdr.version != STAT_VERSION) {
				printf("[state-sync] client: version mismatch (%u != %u)\n",
				       hdr.version, STAT_VERSION);
				break;
			}
			if (hdr.payloadLen > 64 * 1024 * 1024) {
				printf("[state-sync] client: refusing payload of %u bytes\n",
				       hdr.payloadLen);
				break;
			}

			PendingState ps;
			ps.serverFrame = hdr.serverFrame;
			ps.flags       = hdr.flags;
			ps.payload.resize(hdr.payloadLen);
			if (!readExact(c_fd, ps.payload.data(), hdr.payloadLen)) break;

			c_bytesReceived.fetch_add(sizeof(hdr) + hdr.payloadLen,
			                          std::memory_order_relaxed);
			c_statesReceived.fetch_add(1, std::memory_order_relaxed);
			c_lastReceiveUs.store(nowUs(), std::memory_order_relaxed);

			printf("[state-sync] client received STAT frame=%llu flags=%02x "
			       "payload=%u bytes\n",
			       (unsigned long long)hdr.serverFrame, hdr.flags, hdr.payloadLen);

			{
				std::lock_guard<std::mutex> lock(c_pendingMu);
				// Latest-wins. If the old pending was never applied, drop it.
				c_pending    = std::move(ps);
				c_hasPending = true;
			}
		}

		if (c_fd >= 0) {
			::shutdown(c_fd, SHUT_RDWR);
			close(c_fd);
			c_fd = -1;
		}
		c_connected.store(false, std::memory_order_relaxed);
		printf("[state-sync] client disconnected, will retry\n");
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}
	printf("[state-sync] client rx thread stopped\n");
}

} // namespace client

bool clientStart(const char* host)
{
	using namespace client;
	if (c_running.load()) return true;
	if (!host || !*host) return false;
	c_host    = host;
	c_running.store(true);
	c_thread  = std::thread(client::rxLoop);
	return true;
}

void clientStop()
{
	using namespace client;
	if (!c_running.load()) return;
	c_running.store(false);
	if (c_fd >= 0) ::shutdown(c_fd, SHUT_RDWR);
	if (c_thread.joinable()) c_thread.join();
	printf("[state-sync] client stopped\n");
}

bool clientApplyPending()
{
	using namespace client;
	if (!c_running.load(std::memory_order_relaxed)) return false;

	PendingState ps;
	{
		std::lock_guard<std::mutex> lock(c_pendingMu);
		if (!c_hasPending) return false;
		ps = std::move(c_pending);
		c_hasPending = false;
	}

	// Decompress (or passthrough if the flag says it wasn't compressed).
	// MirrorDecompressor auto-detects the ZCST header and passes-through
	// otherwise — we don't actually need to check the flag here, but we
	// assert consistency in debug.
	if (!c_decompInit) {
		c_decomp.init(32 * 1024 * 1024);   // max DC+Naomi save state comfortably
		c_decompInit = true;
	}

	auto td0 = std::chrono::high_resolution_clock::now();
	size_t rawSize = 0;
	const uint8_t* rawPtr = c_decomp.decompress(ps.payload.data(),
	                                            ps.payload.size(), rawSize);
	auto td1 = std::chrono::high_resolution_clock::now();
	c_lastDecompressUs.store(
	    std::chrono::duration_cast<std::chrono::microseconds>(td1 - td0).count(),
	    std::memory_order_relaxed);

	if (rawSize == 0 || rawPtr == nullptr) {
		printf("[state-sync] client: decompress failed, skipping state\n");
		return false;
	}

	// Apply via Emulator::loadstate(), NOT bare dc_deserialize.
	//
	// Why this matters: dc_deserialize ONLY rewrites the in-memory state.
	// It doesn't tell the JIT, the MMU, the texture cache, or memwatch
	// that the world just changed under them. The first apply works
	// because everything is still cold; subsequent applies (or even SH4
	// execution after the first apply) trip on stale JIT translation
	// blocks pointing into now-bogus code, stale MMU TLB entries, or
	// stale memwatch page protections — manifesting as random
	// "SH4 exception when blocked" crashes seconds later.
	//
	// Emulator::loadstate (core/emulator.cpp:938) is the canonical
	// wrapper that brackets dc_deserialize with all the necessary
	// invalidations: custom_texture reinit, AICA ARM recompiler flush,
	// MMU flush + reload, SHREC bm_Reset, memwatch reset, SH4 executor
	// cache reset, LoadState event broadcast. It's exactly what GGPO's
	// load_game_state callback does, but exposed as a public Emulator
	// method so callers outside GGPO can use it.
	//
	// rollback=false MUST match the server's buildFullSaveState — see
	// maplecast_mirror.cpp:buildFullSaveState for the explanation.
	auto ta0 = std::chrono::high_resolution_clock::now();
	try {
		Deserializer deser(rawPtr, rawSize, /*rollback=*/false);
		emu.loadstate(deser);
	} catch (const std::exception& e) {
		printf("[state-sync] client: emu.loadstate threw: %s\n", e.what());
		return false;
	}
	auto ta1 = std::chrono::high_resolution_clock::now();
	c_lastApplyUs.store(
	    std::chrono::duration_cast<std::chrono::microseconds>(ta1 - ta0).count(),
	    std::memory_order_relaxed);

	// Reseed the player-client local frame counter so we step in lockstep
	// with the server from this snapshot forward.
	maplecast_player::seedLocalFrame(ps.serverFrame);

	c_everSynced = true;
	c_statesApplied.fetch_add(1, std::memory_order_relaxed);
	c_lastAppliedFrame.store(ps.serverFrame, std::memory_order_relaxed);
	printf("[state-sync] client applied state @ server frame %llu "
	       "(raw=%zu bytes, apply=%lluus)\n",
	       (unsigned long long)ps.serverFrame, rawSize,
	       (unsigned long long)c_lastApplyUs.load());
	return true;
}

ClientStats getClientStats()
{
	using namespace client;
	ClientStats out{};
	out.running           = c_running.load(std::memory_order_relaxed);
	out.connected         = c_connected.load(std::memory_order_relaxed);
	out.statesReceived    = c_statesReceived.load(std::memory_order_relaxed);
	out.statesApplied     = c_statesApplied.load(std::memory_order_relaxed);
	out.bytesReceived     = c_bytesReceived.load(std::memory_order_relaxed);
	out.lastApplyUs       = c_lastApplyUs.load(std::memory_order_relaxed);
	out.lastDecompressUs  = c_lastDecompressUs.load(std::memory_order_relaxed);
	out.lastAppliedFrame  = c_lastAppliedFrame.load(std::memory_order_relaxed);
	out.lastReceiveUs     = c_lastReceiveUs.load(std::memory_order_relaxed);
	return out;
}

}  // namespace maplecast_state_sync
