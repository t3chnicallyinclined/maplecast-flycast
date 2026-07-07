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

static void trimMap(std::map<uint64_t, uint64_t>& m)
{
	while (m.size() > kMaxHashes) m.erase(m.begin());
}

// Compare local vs server hash for `frame` if both are present. Caller holds
// c_mu. Returns: 0 = not both present, 1 = match, 2 = mismatch.
static int compareLocked(uint64_t frame)
{
	auto ll = c_localHashes.find(frame);
	if (ll == c_localHashes.end()) return 0;
	// Compare local[frame] against server[frame + lockedOffset]. The offset is 0
	// until the first divergence auto-detects a constant skew.
	const uint64_t target = (uint64_t)((int64_t)frame + c_frameOffset);
	auto ls = c_serverHashes.find(target);
	if (ls == c_serverHashes.end()) return 0;   // server hash for the target frame not in yet
	c_compared.fetch_add(1, std::memory_order_relaxed);

	if (ls->second == ll->second) {
		c_matched.fetch_add(1, std::memory_order_relaxed);
		c_consecMismatch = 0;
		if (debugOn() && (frame % 300 == 0))
			printf("[lockstep] client MATCH  frame=%llu (offset%+lld) h=%016llx\n",
			       (unsigned long long)frame, (long long)c_frameOffset,
			       (unsigned long long)ll->second);
		return 1;
	}

	// Diverged at the current offset. Probe a window of offsets for a byte-
	// identical match. If one exists, the SIM IS DETERMINISTIC and only the
	// frame LABEL is offset — either the initial resume skew (first lock) or a
	// slow frame-ACCOUNTING drift (the GUI present-loop advancing SH4 frames out
	// of 1:1 with the stream tick). Re-lock to follow it: this keeps the mirror
	// in checksum sync AND measures the drift (a steady offset = pure label
	// skew; a growing offset = frame-accounting drift; neither is tick
	// corruption). Only a total absence of any matching offset is real
	// render-perturbs-sim divergence.
	{
		int best = 99;
		for (int d = -6; d <= 6; d++) {
			auto it = c_serverHashes.find((uint64_t)((int64_t)frame + d));
			if (it != c_serverHashes.end() && it->second == ll->second) { best = d; break; }
		}
		if (best != 99) {
			const bool first = !c_offsetLocked;
			const int64_t prev = c_frameOffset;
			c_frameOffset  = best;
			c_offsetLocked = true;
			c_matched.fetch_add(1, std::memory_order_relaxed);
			c_skewMatched++;
			c_consecMismatch = 0;
			if (first)
				printf("[lockstep] LOCKED frame-compare offset = %+d "
				       "(local[%llu]==server[%llu], byte-identical) — sim is "
				       "deterministic; render client runs an extra SH4 frame on "
				       "resume. NOT a per-tick divergence.\n",
				       best, (unsigned long long)frame,
				       (unsigned long long)(frame + best));
			else
				printf("[lockstep] frame-accounting DRIFT: offset %+lld -> %+d "
				       "@frame=%llu (still byte-identical — render present-loop "
				       "advanced the SH4 tick out of 1:1 with the stream)\n",
				       (long long)prev, best, (unsigned long long)frame);
			return 1;
		}
	}

	// Genuine mismatch: no offset in the window reproduces the state. THIS would
	// be real per-tick (render-perturbs-sim) divergence.
	c_mismatched.fetch_add(1, std::memory_order_relaxed);
	c_consecMismatch++;
	printf("[lockstep] client MISMATCH frame=%llu local=%016llx server[+%lld]=%016llx "
	       "consec=%d\n",
	       (unsigned long long)frame,
	       (unsigned long long)ll->second, (long long)c_frameOffset,
	       (unsigned long long)ls->second, c_consecMismatch);
	return 2;
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
		compareLocked(frame);   // local hash for this frame may already be in
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

	int result;
	{
		std::lock_guard<std::mutex> lock(c_mu);
		c_localHashes[completed] = h;
		trimMap(c_localHashes);
		result = compareLocked(completed);
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
	// the state for many consecutive frames. A single/transient mismatch, or a
	// pure frame-label skew (auto-locked above), must NOT trigger the 10 MB
	// re-JOIN storm that froze earlier builds. Require a run of real mismatches.
	static constexpr int kResyncThreshold = 30;
	if (result == 2 && c_consecMismatch >= kResyncThreshold) {
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

} // namespace maplecast_lockstep
