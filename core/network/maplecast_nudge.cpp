/*
	MapleCast Nudge — 58 bytes/frame position correction.

	Server writes to shared memory:
	  frame_counter (u32)
	  6 characters × { pos_x(f32), pos_y(f32), health(u8) } = 6 × 9 = 54 bytes
	  Total: 58 bytes per frame

	Client reads and writes positions directly to Dreamcast RAM.
	Client's SH4 CPU runs normally — it just gets nudged on position.
*/
#include "types.h"
#include "maplecast_nudge.h"
#include "maplecast_gamestate.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/maple/maple_cfg.h"
#include "input/gamepad_device.h"
#include "hw/mem/mem_watch.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

namespace maplecast_nudge
{

static const char* SHM_NAME = "/maplecast_nudge";
static const size_t SHM_SIZE = 4096;  // tiny — just 58 bytes + header

static bool _isServer = false;
static bool _isClient = false;
static uint8_t* _shmPtr = nullptr;
static int _shmFd = -1;


// Shared memory layout
struct NudgeData {
	volatile uint32_t sequence;     // increments each write
	// Inputs
	uint32_t kcode[4];              // button state per port (active-low)
	uint16_t lt[4];                 // left trigger per port
	uint16_t rt[4];                 // right trigger per port
	// Full 253-byte game state — covers ALL screens
	uint8_t gamestate[256];         // serialized via maplecast_gamestate
	uint32_t gs_size;               // actual bytes used
};


static bool openShm(bool create)
{
	if (create) shm_unlink(SHM_NAME);
	int fd = shm_open(SHM_NAME, create ? (O_CREAT | O_RDWR) : O_RDWR, 0666);
	if (fd < 0) { printf("[NUDGE] shm_open failed\n"); return false; }
	if (create) ftruncate(fd, SHM_SIZE);
	_shmPtr = (uint8_t*)mmap(nullptr, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (_shmPtr == MAP_FAILED) { _shmPtr = nullptr; close(fd); return false; }
	if (create) memset(_shmPtr, 0, SHM_SIZE);
	return true;
}

void initServer()
{
	if (!openShm(true)) return;
	_isServer = true;
	printf("[NUDGE] === SERVER === streaming 58 bytes/frame (positions + health)\n");
}

void initClient()
{
	if (!openShm(false)) return;
	_isClient = true;

	// Wait for server to be ready (has written at least 1 frame)
	NudgeData* data = (NudgeData*)_shmPtr;
	printf("[NUDGE] Waiting for server...\n");
	for (int i = 0; i < 500; i++) {
		if (data->sequence > 0) break;
		usleep(10000);
	}

	// Unprotect memory so our writes don't hit page protection faults
	memwatch::unprotect();

	printf("[NUDGE] === CLIENT === receiving inputs + 253B state corrections\n");
	printf("[NUDGE] Memory unprotected — writes will not trigger faults\n");
}

bool isServer() { return _isServer; }
bool isClient() { return _isClient; }

// Server: read full game state + inputs, write to shared memory
void serverTick()
{
	if (!_isServer || !_shmPtr) return;

	NudgeData* data = (NudgeData*)_shmPtr;

	// Capture inputs
	for (int p = 0; p < 4; p++)
	{
		data->kcode[p] = kcode[p];
		data->lt[p] = lt[p];
		data->rt[p] = rt[p];
	}

	// Read and serialize full 253-byte game state
	maplecast_gamestate::GameState gs;
	maplecast_gamestate::readGameState(gs);
	data->gs_size = maplecast_gamestate::serialize(gs, data->gamestate, sizeof(data->gamestate));

	__sync_synchronize();
	data->sequence++;
}

// Client: read inputs + game state from shared memory, apply corrections
static uint32_t _lastSeq = 0;
static uint32_t _totalFrames = 0;
static uint32_t _corrections = 0;

void clientTick()
{
	if (!_isClient || !_shmPtr) return;

	NudgeData* data = (NudgeData*)_shmPtr;

	uint32_t seq = data->sequence;
	if (seq == _lastSeq) return;
	_lastSeq = seq;

	__sync_synchronize();

	_totalFrames++;

	// 1. Inject server inputs into client
	for (int p = 0; p < 4; p++)
	{
		kcode[p] = data->kcode[p];
		lt[p] = data->lt[p];
		rt[p] = data->rt[p];
	}

	// 2. Deserialize server game state
	if (data->gs_size < maplecast_gamestate::WIRE_SIZE) return;  // not ready yet

	maplecast_gamestate::GameState serverState;
	maplecast_gamestate::deserialize(data->gamestate, data->gs_size, serverState);

	// 3. Only correct if server is in a valid state
	if (serverState.frame_counter == 0) return;  // no valid data yet

	// 4. Write server state to client RAM
	// Only write specific fields — don't touch frame_counter (SH4 manages it)
	// Write positions, health, animation, meters — visual state
	for (int i = 0; i < 6; i++)
	{
		uint32_t base = 0;
		switch(i) {
			case 0: base = 0x8C268340; break;
			case 1: base = 0x8C2688E4; break;
			case 2: base = 0x8C268E88; break;
			case 3: base = 0x8C26942C; break;
			case 4: base = 0x8C2699D0; break;
			case 5: base = 0x8C269F74; break;
		}
		const auto& c = serverState.chars[i];

		// Positions (most important for sync)
		uint32_t raw;
		memcpy(&raw, &c.pos_x, 4); addrspace::write32(base + 0x034, raw);
		memcpy(&raw, &c.pos_y, 4); addrspace::write32(base + 0x038, raw);
		memcpy(&raw, &c.screen_x, 4); addrspace::write32(base + 0x0E0, raw);
		memcpy(&raw, &c.screen_y, 4); addrspace::write32(base + 0x0E4, raw);
		memcpy(&raw, &c.vel_x, 4); addrspace::write32(base + 0x05C, raw);
		memcpy(&raw, &c.vel_y, 4); addrspace::write32(base + 0x060, raw);

		// State
		addrspace::write8(base + 0x000, c.active);
		addrspace::write8(base + 0x420, c.health);
		addrspace::write8(base + 0x424, c.red_health);
		addrspace::write16(base + 0x1D0, c.animation_state);
		addrspace::write16(base + 0x144, c.sprite_id);
		addrspace::write16(base + 0x142, c.anim_timer);
		addrspace::write8(base + 0x110, c.facing_right);
	}

	// Global state
	addrspace::write8(0x8C289624, serverState.in_match);
	addrspace::write8(0x8C289630, serverState.game_timer);

	// Camera
	uint32_t raw;
	memcpy(&raw, &serverState.camera_x, 4); addrspace::write32(0x8C1F9CD8, raw);
	memcpy(&raw, &serverState.camera_y, 4); addrspace::write32(0x8C1F9CDC, raw);

	// Meters
	addrspace::write16(0x8C289646, serverState.p1_meter_fill);
	addrspace::write16(0x8C289648, serverState.p2_meter_fill);
	addrspace::write8(0x8C28964A, serverState.p1_meter_level);
	addrspace::write8(0x8C28964B, serverState.p2_meter_level);

	_corrections++;

	// Log every 5 seconds
	if (_totalFrames % 300 == 0)
	{
		printf("[NUDGE] %u frames | %u corrections | server_frame=%u | in_match=%d\n",
			_totalFrames, _corrections, serverState.frame_counter, serverState.in_match);
	}
}

}  // namespace maplecast_nudge
