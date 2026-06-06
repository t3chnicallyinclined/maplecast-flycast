/*
	MapleCast State-Replica Client — server-authoritative state INJECTION.
	See maplecast_state_replica.h for the concept and the injection-timing
	rationale (the crux: inject at frame top, before runInternal()).
*/
#include "types.h"
#include "maplecast_state_replica.h"
#include "maplecast_gamestate.h"
#include "maplecast_mirror.h"
#include "cfg/option.h"

#include <atomic>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

// The authoritative input globals the game reads at vblank. In FREEZE mode we
// pin these to neutral (active-low: all bits set) so the local SH4 cannot move
// the characters — only the injected GSTA does.
extern u32 kcode[4];
extern u16 rt[4], lt[4];

namespace maplecast_state_replica
{

static std::atomic<bool>     _active{false};
static std::atomic<bool>     _gotFirst{false};
static bool                  _freeze = false;
static std::string           _host;
static int                   _port = 7201;
static std::atomic<uint32_t> _lastFrame{0};
static std::atomic<uint64_t> _injected{0};
static std::atomic<uint64_t> _stalled{0};
static std::atomic<int>      _lastObjsSeen{0};      // objects the server shipped (OBJF)
static std::atomic<int>      _lastObjsWritten{0};   // objects we found a local node for

static bool parseSpec(const char* spec)
{
	if (!spec || !*spec) return false;
	std::string s = spec;
	size_t colon = s.find_last_of(':');
	if (colon != std::string::npos) {
		_host = s.substr(0, colon);
		int p = std::atoi(s.c_str() + colon + 1);
		if (p > 0 && p <= 65535) _port = p;
	} else {
		_host = s;
	}
	return !_host.empty();
}

bool init()
{
	if (_active.load()) return true;
	const char* spec = std::getenv("MAPLECAST_STATE_REPLICA");
	if (!spec || !*spec) return false;
	if (!parseSpec(spec)) {
		printf("[state-replica] bad MAPLECAST_STATE_REPLICA spec '%s'\n", spec);
		return false;
	}
	// FREEZE is the ONLY supported path. The client must NOT simulate any
	// visible thing — it renders the injected state PURELY. Input-replay /
	// local-sim correction was rejected: any dropped input drifts whatever the
	// inject doesn't cover. So we force freeze on; the env var is kept only as
	// documentation of intent (and to allow a future opt-out experiment).
	_freeze = (std::getenv("MAPLECAST_STATE_REPLICA_NO_FREEZE") == nullptr);

	// The local SH4 is authoritative for nothing here — mute its audio so two
	// instances side-by-side don't fight over the speakers. The server's audio
	// (if wanted) comes from its own stream.
	settings.aica.muteAudio = true;

	// Reuse the mirror WS client purely as the GSTA transport. wsClientRun()
	// already deserializes 'GSTA' packets into _clientGameState, exposed via
	// maplecast_mirror::getClientGameState(). We do NOT consume the TA video —
	// the local SH4 builds its own TA list. (The WS still carries the heavy TA
	// frames; for a bandwidth-pure test the server should be told to ship GSTA
	// only, but for the fastest visual proof reusing this stream is fine.)
	printf("[state-replica] === STATE-REPLICA MODE ===\n");
	printf("[state-replica] GSTA source: %s:%d  freeze=%d\n", _host.c_str(), _port, (int)_freeze);
	printf("[state-replica] inject point: frame top, before runInternal()\n");
	maplecast_mirror::startMirrorStream(_host.c_str(), _port);

	_active.store(true);
	return true;
}

bool frameInject()
{
	if (!_active.load(std::memory_order_relaxed)) return true;

	// FREEZE: pin local inputs to neutral every frame so the local SH4 cannot
	// advance the characters on its own — the injected GSTA is the only mover.
	// active-low: a released button is a 1 bit; neutral = all 1s.
	if (_freeze) {
		for (int i = 0; i < 4; i++) { kcode[i] = 0xFFFFFFFFu; lt[i] = 0; rt[i] = 0; }
	}

	maplecast_gamestate::GameState gs;
	bool haveState = maplecast_mirror::getClientGameState(gs);
	if (!haveState) {
		// No authoritative state yet. Do NOT block — let the frame render the
		// savestate-baseline pose so the operator at least sees the game. (The
		// emu loop must keep advancing for the renderer to present.)
		_stalled.fetch_add(1, std::memory_order_relaxed);
		static uint64_t _waitN = 0;
		if ((++_waitN % 60) == 1)
			printf("[state-replica] waiting for first GSTA (rendering savestate frame) stalls=%llu\n",
			       (unsigned long long)_stalled.load());
		return true;   // advance + render anyway
	}

	// 1) Inject the chars + globals + stage-anim timer. The game's draw code
	//    (run by runInternal() right after this returns) reads these when it
	//    builds the TA list for this frame.
	maplecast_gamestate::writeGameState(gs);

	// 2) Inject the OBJECT POOL (cape / effects / projectiles / supers). The
	//    server ships the full record via OBJF; writeObjects overwrites the
	//    matching already-linked local nodes so the game's pool render walker
	//    draws the server's truth. Objects the local SH4 never spawned (under
	//    FREEZE: input-driven projectiles/supers) have no node and stay missing
	//    — that gap is the measured "needs node synthesis" region.
	maplecast_gamestate::ObjectState objs[48];
	int nobj = maplecast_mirror::getClientObjects(objs, 48);
	int wrote = 0;
	if (nobj > 0) wrote = maplecast_gamestate::writeObjects(objs, nobj);
	_lastObjsSeen.store(nobj, std::memory_order_relaxed);
	_lastObjsWritten.store(wrote, std::memory_order_relaxed);

	uint64_t injected = _injected.fetch_add(1, std::memory_order_relaxed) + 1;
	_lastFrame.store(gs.frame_counter, std::memory_order_relaxed);

	if (!_gotFirst.exchange(true)) {
		printf("[state-replica] FIRST GSTA injected — frame_counter=%u stage_anim=%u objs=%d\n",
		       gs.frame_counter, gs.stage_anim_timer, nobj);
		fflush(stdout);
	}

	// Once-per-second heartbeat (wall clock, so it prints even at low FPS).
	// frame = server frame_counter; injected = frames we've applied; objs gap =
	// the node-synthesis region (server objects with no local node yet).
	{
		struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
		int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
		static int64_t lastMs = 0;
		if (now - lastMs >= 1000) {
			lastMs = now;
			printf("[state-replica] HEARTBEAT frame=%u injected=%llu objs seen=%d written=%d gap=%d\n",
			       gs.frame_counter, (unsigned long long)injected, nobj, wrote, nobj - wrote);
			fflush(stdout);
		}
	}
	return true;
}

void shutdown()
{
	if (!_active.load()) return;
	_active.store(false);
	settings.aica.muteAudio = false;
	printf("[state-replica] shutdown (injected=%llu stalled=%llu)\n",
	       (unsigned long long)_injected.load(),
	       (unsigned long long)_stalled.load());
}

bool active() { return _active.load(std::memory_order_relaxed); }

Stats getStats()
{
	Stats s{};
	s.active = _active.load(std::memory_order_relaxed);
	s.gotFirstState = _gotFirst.load(std::memory_order_relaxed);
	s.freeze = _freeze;
	s.lastFrameCounter = _lastFrame.load(std::memory_order_relaxed);
	s.framesInjected = _injected.load(std::memory_order_relaxed);
	s.framesStalled = _stalled.load(std::memory_order_relaxed);
	s.lastObjsSeen = _lastObjsSeen.load(std::memory_order_relaxed);
	s.lastObjsWritten = _lastObjsWritten.load(std::memory_order_relaxed);
	return s;
}

}
