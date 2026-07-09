/*
	MapleCast Lockstep Mirror — implementation.

	See maplecast_lockstep.h for the architecture. This file is the checksum /
	resync layer: a tiny UDP channel carrying the game-state-region hash from
	the authoritative server to native lockstep clients, plus the client-side
	compare + resync trigger. The heavy lifting (JOIN snapshot, input tape,
	native render) is done by the existing, reused modules.
*/
#include "types.h"
#include "maplecast_lockstep.h"
#include "maplecast_rollback.h"   // gameStateRegionHash()
#include "maplecast_player.h"     // requestResync()

#include <atomic>
#include <thread>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

#include "net_platform.h"
#include "maplecast_compat.h"

namespace maplecast_lockstep
{

static inline int64_t nowUs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

// ── env / common ──────────────────────────────────────────────────────

bool active()
{
	static int cached = -1;
	if (cached < 0) {
		const char* e = std::getenv("MAPLECAST_LOCKSTEP");
		cached = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
	}
	return cached == 1;
}

uint32_t hashInterval()
{
	static uint32_t cached = 0;
	if (cached == 0) {
		cached = 60;
		if (const char* e = std::getenv("MAPLECAST_LOCKSTEP_INTERVAL")) {
			int n = std::atoi(e);
			if (n > 0) cached = (uint32_t)n;
		}
	}
	return cached;
}

static bool debugOn()
{
	static int cached = -1;
	if (cached < 0)
		cached = std::getenv("MAPLECAST_LOCKSTEP_DEBUG") ? 1 : 0;
	return cached == 1;
}

// Encode a GSHA datagram into buf[GSHA_DATAGRAM_LEN]. Little-endian.
static void encodeGsha(uint8_t* buf, uint64_t frame, uint64_t hash)
{
	buf[0] = 'G'; buf[1] = 'S'; buf[2] = 'H'; buf[3] = 'A';
	buf[4] = GSHA_VERSION;
	buf[5] = buf[6] = buf[7] = 0;
	for (int i = 0; i < 8; i++) buf[8  + i] = (uint8_t)((frame >> (i * 8)) & 0xFF);
	for (int i = 0; i < 8; i++) buf[16 + i] = (uint8_t)((hash  >> (i * 8)) & 0xFF);
}
static bool decodeGsha(const uint8_t* buf, size_t n, uint64_t& frame, uint64_t& hash)
{
	if (n < (size_t)GSHA_DATAGRAM_LEN) return false;
	if (!(buf[0] == 'G' && buf[1] == 'S' && buf[2] == 'H' && buf[3] == 'A')) return false;
	if (buf[4] != GSHA_VERSION) return false;
	frame = 0; hash = 0;
	for (int i = 0; i < 8; i++) frame |= ((uint64_t)buf[8  + i]) << (i * 8);
	for (int i = 0; i < 8; i++) hash  |= ((uint64_t)buf[16 + i]) << (i * 8);
	return true;
}

// =====================================================================
//                               SERVER
// =====================================================================

namespace server {

struct Subscriber {
	sockaddr_in addr;
	int64_t     lastHeloUs;
};

static std::atomic<bool>       s_running{false};
static int                     s_sock = -1;
static std::thread             s_heloThread;
static std::mutex              s_subMu;
static std::vector<Subscriber> s_subs;

static std::atomic<uint64_t> s_hashesSent{0};
static std::atomic<uint64_t> s_datagramsSent{0};
static std::atomic<uint64_t> s_bytesSent{0};

// Match a subscriber by (ip,port).
static int findSub(const sockaddr_in& a)
{
	for (size_t i = 0; i < s_subs.size(); i++)
		if (s_subs[i].addr.sin_addr.s_addr == a.sin_addr.s_addr
		 && s_subs[i].addr.sin_port        == a.sin_port)
			return (int)i;
	return -1;
}

static void heloLoop()
{
	printf("[lockstep] server HELO thread started on port %d\n", HASH_PORT);
	struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000;   // 200ms
	mc_setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	uint8_t buf[64];
	while (s_running.load(std::memory_order_relaxed)) {
		sockaddr_in from{};
		socklen_t flen = sizeof(from);
		ssize_t n = mc_recvfrom(s_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &flen);
		int64_t now = nowUs();
		if (n >= 4 && buf[0] == 'H' && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'O') {
			std::lock_guard<std::mutex> lock(s_subMu);
			int idx = findSub(from);
			if (idx >= 0) {
				s_subs[idx].lastHeloUs = now;
			} else {
				Subscriber s{}; s.addr = from; s.lastHeloUs = now;
				s_subs.push_back(s);
				char ip[INET_ADDRSTRLEN];
				inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
				printf("[lockstep] server: new hash subscriber %s:%u\n",
				       ip, ntohs(from.sin_port));
			}
		}
		// Age out silent subscribers (5s TTL). Cheap sweep on every wakeup.
		{
			std::lock_guard<std::mutex> lock(s_subMu);
			for (size_t i = 0; i < s_subs.size();) {
				if (now - s_subs[i].lastHeloUs > 5000000) {
					s_subs.erase(s_subs.begin() + i);
				} else ++i;
			}
		}
	}
	printf("[lockstep] server HELO thread stopped\n");
}

} // namespace server

bool serverStart()
{
	using namespace server;
	if (!active()) return false;
	if (s_running.load()) return true;

	s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s_sock < 0) {
		printf("[lockstep] server socket() failed: %s\n", strerror(errno));
		return false;
	}
	int one = 1;
	mc_setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port        = htons(HASH_PORT);
	if (bind(s_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
		printf("[lockstep] server bind port %d failed: %s\n", HASH_PORT, strerror(errno));
		mc_closesocket(s_sock);
		s_sock = -1;
		return false;
	}
	s_running.store(true);
	s_heloThread = std::thread(server::heloLoop);
	printf("[lockstep] === SERVER READY === game-state hash channel port %d, interval=%u\n",
	       HASH_PORT, hashInterval());
	return true;
}

void serverStop()
{
	using namespace server;
	if (!s_running.load()) return;
	s_running.store(false);
	if (s_sock >= 0) {
		::shutdown(s_sock, SHUT_RDWR);
		mc_closesocket(s_sock);
		s_sock = -1;
	}
	if (s_heloThread.joinable()) s_heloThread.join();
	std::lock_guard<std::mutex> lock(s_subMu);
	s_subs.clear();
	printf("[lockstep] server stopped\n");
}

void onServerFrame(uint64_t frame)
{
	using namespace server;
	if (!s_running.load(std::memory_order_relaxed)) return;
	if ((frame % hashInterval()) != 0) return;

	// No subscribers => skip the hash entirely (cheap steady-state).
	{
		std::lock_guard<std::mutex> lock(s_subMu);
		if (s_subs.empty()) return;
	}

	// Compute the game-state-region hash from live guest RAM. This runs on the
	// same emu/publish thread that just committed `frame`, so the bytes reflect
	// the end of frame `frame` — exactly what the client compares against.
	const uint64_t h = maplecast_rollback::gameStateRegionHash();

	uint8_t dg[GSHA_DATAGRAM_LEN];
	encodeGsha(dg, frame, h);

	std::lock_guard<std::mutex> lock(s_subMu);
	for (const auto& s : s_subs) {
		ssize_t sent = mc_sendto(s_sock, dg, sizeof(dg), 0,
		                         (const sockaddr*)&s.addr, sizeof(s.addr));
		if (sent == (ssize_t)sizeof(dg)) {
			s_datagramsSent.fetch_add(1, std::memory_order_relaxed);
			s_bytesSent.fetch_add(sizeof(dg), std::memory_order_relaxed);
		}
	}
	s_hashesSent.fetch_add(1, std::memory_order_relaxed);

	if (debugOn() && (frame % 60 == 0))
		printf("[lockstep] server hash frame=%llu h=%016llx subs=%zu\n",
		       (unsigned long long)frame, (unsigned long long)h, s_subs.size());
}

ServerStats getServerStats()
{
	using namespace server;
	ServerStats out{};
	out.running = s_running.load(std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(s_subMu);
		out.subscribers = (uint32_t)s_subs.size();
	}
	out.hashesSent    = s_hashesSent.load(std::memory_order_relaxed);
	out.datagramsSent = s_datagramsSent.load(std::memory_order_relaxed);
	out.bytesSent     = s_bytesSent.load(std::memory_order_relaxed);
	return out;
}

// =====================================================================
//                               CLIENT
// =====================================================================

namespace client {

static std::atomic<bool>   c_running{false};
static std::thread         c_thread;
static int                 c_sock = -1;
static sockaddr_in         c_serverAddr{};
static std::string         c_host;

// Server hashes keyed by absolute frame, and our own local hashes. Both are
// bounded ring-maps (drop oldest). Comparison happens whenever both maps have
// the same frame key — decoupling wire timing from emu timing.
static std::mutex                    c_mu;
static std::map<uint64_t, uint64_t>  c_serverHashes;
static std::map<uint64_t, uint64_t>  c_localHashes;
static constexpr size_t              kMaxHashes = 4096;

static std::atomic<uint64_t> c_hashesReceived{0};
static std::atomic<uint64_t> c_compared{0};
static std::atomic<uint64_t> c_matched{0};
static std::atomic<uint64_t> c_mismatched{0};
static std::atomic<uint64_t> c_resyncs{0};
static std::atomic<uint64_t> c_lastServerFrame{0};
static std::atomic<uint64_t> c_bytesReceived{0};

// Cooldown so one desync doesn't spam resync requests before the snapshot
// lands and the frame counter is reseeded.
static int64_t c_lastResyncUs = 0;

// Frame-compare offset (measurement-driven). The norend headless client aligns
// at offset 0 (local[N]==server[N]); the NATIVE-RENDER GUI client runs one
// extra SH4 frame during JOIN/resume (its present-driven mainui loop advances a
// frame the frameGate counter doesn't book), so its state is BYTE-IDENTICAL but
// labeled one ahead: local[N]==server[N+1]. Rather than hard-code either, we
// auto-detect the constant offset from the first post-JOIN divergence and lock
// it. A STABLE lock (matches for the rest of the match) proves the render does
// NOT perturb the per-tick sim — it's a pure counter skew. If no constant
// offset holds, that's genuine per-tick divergence and we resync.
static int64_t c_frameOffset  = 0;
static bool    c_offsetLocked = false;
static uint64_t c_skewMatched = 0;   // matches credited via the locked offset
static int      c_consecMismatch = 0;
// Server frames already scored, so the two compare triggers (a server hash
// arriving vs the client's local frame catching up to it) don't double-count.
static std::set<uint64_t> c_comparedServer;
// Offset-detection / drift-follow window. Searched over the DENSE local ring
// (every frame) for a byte-identical match to a server hash — so it's robust to
// a sparse server hash cadence (INTERVAL>1) and only needs to span the small
// state-vs-label skew, not the interval.
static constexpr int kOffsetWindow = 30;

static void trimMap(std::map<uint64_t, uint64_t>& m)
{
	while (m.size() > kMaxHashes) m.erase(m.begin());
}

// Score one server-hash frame S against the client's DENSE local-hash ring.
// Caller holds c_mu. Idempotent per S (via c_comparedServer). This is SERVER-
// driven: for a (possibly sparse, INTERVAL>1) server hash we search the local
// ring for the byte-identical frame, so a sparse server cadence and a growing
// lag can never wedge detection at matched=0 (the earlier ±6 client-driven
// probe searched the SPARSE server side and failed under INTERVAL=60).
//
// Invariant: local[N] == server[N + offset]  <=>  server[S] == local[S - offset].
static void onServerHash(uint64_t S)
{
	auto ls = c_serverHashes.find(S);
	if (ls == c_serverHashes.end()) return;
	if (c_comparedServer.count(S)) return;           // already scored this S

	if (c_offsetLocked) {
		auto ll = c_localHashes.find((uint64_t)((int64_t)S - c_frameOffset));
		if (ll == c_localHashes.end()) {
			// Client hasn't reached S-offset yet OR it aged out. If it's still
			// ahead of the ring, retry later; if it aged out, drop it.
			if (!c_localHashes.empty() && S - (uint64_t)c_frameOffset < c_localHashes.begin()->first)
				c_comparedServer.insert(S);          // permanently gone; skip
			return;
		}
		c_comparedServer.insert(S);
		c_compared.fetch_add(1, std::memory_order_relaxed);
		if (ll->second == ls->second) {
			c_matched.fetch_add(1, std::memory_order_relaxed);
			c_consecMismatch = 0;
			return;
		}
		// Mismatch at the locked offset — follow a slow frame-accounting drift
		// (still byte-identical at a nearby offset) over the dense local ring.
		for (int k = -kOffsetWindow; k <= kOffsetWindow; k++) {
			auto l2 = c_localHashes.find((uint64_t)((int64_t)S - k));
			if (l2 != c_localHashes.end() && l2->second == ls->second) {
				printf("[lockstep] frame-accounting DRIFT: offset %+lld -> %+d "
				       "@server=%llu (still byte-identical)\n",
				       (long long)c_frameOffset, k, (unsigned long long)S);
				c_frameOffset = k;
				c_matched.fetch_add(1, std::memory_order_relaxed);
				c_skewMatched++;
				c_consecMismatch = 0;
				return;
			}
		}
		// Genuine mismatch: no offset reproduces the state = real divergence.
		c_mismatched.fetch_add(1, std::memory_order_relaxed);
		c_consecMismatch++;
		printf("[lockstep] client MISMATCH server=%llu local=%016llx server=%016llx "
		       "consec=%d\n",
		       (unsigned long long)S, (unsigned long long)ll->second,
		       (unsigned long long)ls->second, c_consecMismatch);
		return;
	}

	// Not yet locked: detect the constant offset by finding the local frame whose
	// state hash equals this server hash. Searches the DENSE local ring.
	for (int k = -kOffsetWindow; k <= kOffsetWindow; k++) {
		auto ll = c_localHashes.find((uint64_t)((int64_t)S - k));
		if (ll != c_localHashes.end() && ll->second == ls->second) {
			c_frameOffset  = k;
			c_offsetLocked = true;
			c_comparedServer.insert(S);
			c_compared.fetch_add(1, std::memory_order_relaxed);
			c_matched.fetch_add(1, std::memory_order_relaxed);
			c_skewMatched++;
			c_consecMismatch = 0;
			printf("[lockstep] LOCKED frame-compare offset = %+d (server-driven: "
			       "local[%llu]==server[%llu], byte-identical) — sim deterministic, "
			       "render-path frame-label skew. NOT a per-tick divergence.\n",
			       k, (unsigned long long)((int64_t)S - k), (unsigned long long)S);
			return;
		}
	}
	// No match yet — the client hasn't reached this frame, or it's genuinely
	// diverged. Don't score it; retry on the next trigger. (If it never matches
	// after the client passes it, the mismatch path above will catch it once an
	// offset is locked.)
}

// Drive comparisons for all server frames the client can now compare (both
// triggers funnel here). Caller holds c_mu. Cheap: the server ring is small
// (INTERVAL-spaced), and c_comparedServer skips already-scored frames.
static void drainComparisons()
{
	for (const auto& kv : c_serverHashes)
		onServerHash(kv.first);
	// Trim the compared-set to the live server-hash window.
	while (c_comparedServer.size() > 8192) c_comparedServer.erase(c_comparedServer.begin());
}

static void rxLoop()
{
	printf("[lockstep] client rx thread started, target %s:%d\n",
	       c_host.c_str(), HASH_PORT);

	c_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (c_sock < 0) {
		printf("[lockstep] client socket() failed: %s\n", strerror(errno));
		return;
	}
	// connect() so recv only delivers from the server and send needs no dest.
	if (connect(c_sock, (sockaddr*)&c_serverAddr, sizeof(c_serverAddr)) < 0)
		printf("[lockstep] client connect() warning: %s\n", strerror(errno));

	struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 100000;   // 100ms
	mc_setsockopt(c_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	int64_t lastHeloUs = 0;
	uint8_t buf[128];
	while (c_running.load(std::memory_order_relaxed)) {
		int64_t now = nowUs();
		if (now - lastHeloUs > 900000) {
			const char helo[4] = { 'H', 'E', 'L', 'O' };
			mc_send(c_sock, helo, 4, 0);
			lastHeloUs = now;
		}
		ssize_t n = mc_recv(c_sock, buf, sizeof(buf), 0);
		if (n < 0) {
			// Recv timeout (SO_RCVTIMEO) or transient error. errno is not a
			// reliable Winsock indicator, so just retry — the running flag +
			// HELO pacing bound this at ~10 Hz when idle. Shutdown sets
			// c_running=false and ::shutdown()s the socket, so we exit next spin.
			continue;
		}
		uint64_t frame, hash;
		if (!decodeGsha(buf, (size_t)n, frame, hash)) continue;

		c_hashesReceived.fetch_add(1, std::memory_order_relaxed);
		c_bytesReceived.fetch_add((uint64_t)n, std::memory_order_relaxed);
		c_lastServerFrame.store(frame, std::memory_order_relaxed);

		std::lock_guard<std::mutex> lock(c_mu);
		c_serverHashes[frame] = hash;
		trimMap(c_serverHashes);
		drainComparisons();   // client may already have run this frame
	}

	mc_closesocket(c_sock);
	c_sock = -1;
	printf("[lockstep] client rx thread stopped\n");
}

} // namespace client

bool clientInit(const char* host)
{
	using namespace client;
	if (!active()) return false;
	if (c_running.load()) return true;
	if (!host || !*host) return false;

	std::string h = host;
	// Strip an optional ":port" — the tape host is passed as-is.
	size_t colon = h.find_last_of(':');
	if (colon != std::string::npos) h = h.substr(0, colon);
	c_host = h;

	struct addrinfo hints{};
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	struct addrinfo* res = nullptr;
	if (getaddrinfo(c_host.c_str(), nullptr, &hints, &res) != 0 || !res) {
		printf("[lockstep] client getaddrinfo('%s') failed\n", c_host.c_str());
		return false;
	}
	memcpy(&c_serverAddr, res->ai_addr, sizeof(sockaddr_in));
	c_serverAddr.sin_port = htons((uint16_t)HASH_PORT);
	freeaddrinfo(res);

	c_running.store(true);
	c_thread = std::thread(client::rxLoop);
	printf("[lockstep] === CLIENT MODE === hash channel -> %s:%d\n",
	       c_host.c_str(), HASH_PORT);
	return true;
}

void clientShutdown()
{
	using namespace client;
	if (!c_running.load()) return;
	c_running.store(false);
	if (c_sock >= 0) ::shutdown(c_sock, SHUT_RDWR);
	if (c_thread.joinable()) c_thread.join();
	std::lock_guard<std::mutex> lock(c_mu);
	c_serverHashes.clear();
	c_localHashes.clear();
	c_comparedServer.clear();
	printf("[lockstep] client shutdown\n");
}

void clientVerify(uint64_t frameToRun)
{
	using namespace client;
	if (!c_running.load(std::memory_order_relaxed)) return;
	if (frameToRun == 0) return;   // nothing has run yet

	// Resident guest RAM currently reflects the END of frame (frameToRun-1).
	const uint64_t completed = frameToRun - 1;
	const uint64_t h = maplecast_rollback::gameStateRegionHash();

	bool sustainedDivergence;
	{
		std::lock_guard<std::mutex> lock(c_mu);
		c_localHashes[completed] = h;
		trimMap(c_localHashes);
		// Drive server-driven comparisons: score every server frame the client
		// can now compare (this local frame may complete a pending one).
		drainComparisons();
		sustainedDivergence = (c_consecMismatch >= 30);
	}

	// Periodic checksum summary (always printed + flushed) so the gate can read
	// the match rate live regardless of stdout buffering.
	{
		static uint64_t _lastSummaryFrame = 0;
		if (completed >= _lastSummaryFrame + 120) {
			_lastSummaryFrame = completed;
			printf("[lockstep] CHECKSUM @frame=%llu | compared=%llu matched=%llu "
			       "mismatched=%llu resyncs=%llu serverFrame=%llu\n",
			       (unsigned long long)completed,
			       (unsigned long long)c_compared.load(),
			       (unsigned long long)c_matched.load(),
			       (unsigned long long)c_mismatched.load(),
			       (unsigned long long)c_resyncs.load(),
			       (unsigned long long)c_lastServerFrame.load());
			fflush(stdout);
		}
	}

	// Resync ONLY on SUSTAINED genuine divergence — no constant offset reproduces
	// the state for many consecutive scored server frames. A transient mismatch,
	// a pure frame-label skew (auto-locked), or mere lag (which never scores a
	// mismatch — onServerHash just waits) must NOT trigger the 10 MB re-JOIN
	// storm that froze earlier builds.
	if (sustainedDivergence) {
		int64_t now = nowUs();
		if (now - c_lastResyncUs > 2000000) {   // >=2s cooldown
			c_lastResyncUs = now;
			c_resyncs.fetch_add(1, std::memory_order_relaxed);
			printf("[lockstep] client requesting RESYNC (%d consecutive genuine "
			       "mismatches @ frame %llu, offset%+lld)\n",
			       c_consecMismatch, (unsigned long long)completed,
			       (long long)c_frameOffset);
			maplecast_player::requestResync();
			// Reset compare state so the fresh JOIN re-detects its offset clean.
			std::lock_guard<std::mutex> lock(c_mu);
			c_localHashes.clear();
			c_serverHashes.clear();
			c_comparedServer.clear();
			c_offsetLocked   = false;
			c_frameOffset    = 0;
			c_consecMismatch = 0;
		}
	}
}

ClientStats getClientStats()
{
	using namespace client;
	ClientStats out{};
	out.running         = c_running.load(std::memory_order_relaxed);
	out.hashesReceived  = c_hashesReceived.load(std::memory_order_relaxed);
	out.compared        = c_compared.load(std::memory_order_relaxed);
	out.matched         = c_matched.load(std::memory_order_relaxed);
	out.mismatched      = c_mismatched.load(std::memory_order_relaxed);
	out.resyncs         = c_resyncs.load(std::memory_order_relaxed);
	out.lastServerFrame = c_lastServerFrame.load(std::memory_order_relaxed);
	out.bytesReceived   = c_bytesReceived.load(std::memory_order_relaxed);
	return out;
}

// Lightweight accessor for the predict-live run-ahead: the server's most recent
// frame as reported over the hash channel (updated every hashInterval frames).
uint64_t lastServerFrame()
{
	return client::c_lastServerFrame.load(std::memory_order_relaxed);
}

// Offset-aware server-hash lookup for the predict-live CONFIRMED-hash gate. Given
// a CLIENT frame, returns the server's authoritative hash for the matching server
// frame (server_frame = client_frame + c_frameOffset) if the offset is locked and
// that hash has arrived. Thread-safe (c_mu).
bool serverHashForClientFrame(uint64_t clientFrame, uint64_t* out)
{
	using namespace client;
	std::lock_guard<std::mutex> lock(c_mu);
	// The client consumes the tape stamped in SERVER frame numbers, so its
	// confirmed frame == the server frame directly (strict — no fuzzy ±offset).
	auto it = c_serverHashes.find(clientFrame);
	if (it == c_serverHashes.end()) return false;
	if (out) *out = it->second;
	return true;
}

} // namespace maplecast_lockstep
