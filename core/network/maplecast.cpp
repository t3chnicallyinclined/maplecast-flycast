/*
	MapleCast — Flycast IS the server.

	One UDP port. Auto-assign P1/P2 by connection order.
	4-byte W3 packet: {LT, RT, buttons_hi, buttons_lo}
	Comprehensive telemetry for latency analysis.
*/
#include "maplecast.h"
#include "maplecast_telemetry.h"
#include "hw/maple/maple_cfg.h"
#include "input/gamepad.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#define INVALID_SOCK INVALID_SOCKET
#define SOCK_ERROR SOCKET_ERROR
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#define INVALID_SOCK -1
#define SOCK_ERROR -1
#define closesocket close
typedef int SOCKET;
#endif

#include <cstring>
#include <cstdio>

namespace maplecast
{

static bool _active = false;
static SOCKET _sock = INVALID_SOCK;

// Auto-assigned players by source address
static struct sockaddr_in _playerAddr[2];
static bool _playerAssigned[2] = { false, false };
static int _playerCount = 0;

// Latest W3 per player
static u8 _w3[2][4] = {
	{ 0, 0, 0xFF, 0xFF },
	{ 0, 0, 0xFF, 0xFF },
};

// Telemetry counters
static uint32_t _packetCount[2] = {0, 0};
static uint32_t _unknownPackets = 0;
static uint32_t _getInputCount = 0;
static int64_t _lastInputTimeUs = 0;

// Per-second rate tracking for stats
static uint32_t _pktPerSec[2] = {0, 0};
static uint32_t _chgPerSec[2] = {0, 0};
static uint32_t _pktAccum[2] = {0, 0};
static uint32_t _chgAccum[2] = {0, 0};
static uint32_t _lastRateFrame = 0;
static uint32_t _packetsThisFrame[2] = {0, 0};
static uint32_t _stateChanges[2] = {0, 0};
static u8 _prevW3[2][4] = {{0,0,0xFF,0xFF},{0,0,0xFF,0xFF}};

static bool addrMatch(const struct sockaddr_in& a, const struct sockaddr_in& b)
{
	return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

static void w3ToInput(const u8 w3[4], MapleInputState& state)
{
	u16 buttons = ((u16)w3[2] << 8) | w3[3];
	state.kcode = buttons | 0xFFFF0000;
	state.halfAxes[PJTI_L] = (u16)w3[0] << 8;
	state.halfAxes[PJTI_R] = (u16)w3[1] << 8;
	state.fullAxes[PJAI_X1] = 0;
	state.fullAxes[PJAI_Y1] = 0;
	state.fullAxes[PJAI_X2] = 0;
	state.fullAxes[PJAI_Y2] = 0;
}

static const char* addrStr(const struct sockaddr_in& addr)
{
	static char buf[2][64];
	static int idx = 0;
	idx = (idx + 1) % 2;
	char ip[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
	snprintf(buf[idx], sizeof(buf[idx]), "%s:%d", ip, ntohs(addr.sin_port));
	return buf[idx];
}

bool init(int port)
{
#ifdef _WIN32
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

	_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (_sock == INVALID_SOCK)
	{
		printf("[maplecast] failed to create socket\n");
		return false;
	}

#ifdef _WIN32
	u_long mode = 1;
	ioctlsocket(_sock, FIONBIO, &mode);
#else
	int flags = fcntl(_sock, F_GETFL, 0);
	fcntl(_sock, F_SETFL, flags | O_NONBLOCK);
#endif

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons((u16)port);

	if (bind(_sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCK_ERROR)
	{
		printf("[maplecast] failed to bind port %d\n", port);
		closesocket(_sock);
		_sock = INVALID_SOCK;
		return false;
	}

	_playerAssigned[0] = false;
	_playerAssigned[1] = false;
	_playerCount = 0;
	_unknownPackets = 0;

	_active = true;
	printf("[maplecast] === SERVER MODE ===\n");
	printf("[maplecast] listening on UDP port %d\n", port);
	printf("[maplecast] first sender = P1, second sender = P2\n");
	printf("[maplecast] waiting for players...\n");
	return true;
}

void shutdown()
{
	if (_sock != INVALID_SOCK) { closesocket(_sock); _sock = INVALID_SOCK; }
	_active = false;
	_playerCount = 0;
	printf("[maplecast] shutdown\n");
}

void getInput(MapleInputState inputState[4])
{
	_packetsThisFrame[0] = 0;
	_packetsThisFrame[1] = 0;

	// Drain all pending UDP packets
	u8 buf[64];
	struct sockaddr_in from;
	socklen_t fromLen;

	while (true)
	{
		fromLen = sizeof(from);
		int n = recvfrom(_sock, (char*)buf, sizeof(buf), 0,
			(struct sockaddr*)&from, &fromLen);
		if (n < 4) break;

		// Identify player
		int player = -1;
		for (int i = 0; i < 2; i++)
		{
			if (_playerAssigned[i] && addrMatch(_playerAddr[i], from))
			{
				player = i;
				break;
			}
		}

		// New sender — auto-assign
		if (player < 0)
		{
			if (_playerCount < 2)
			{
				player = _playerCount;
				_playerAddr[player] = from;
				_playerAssigned[player] = true;
				_playerCount++;

				printf("[maplecast] *** P%d ASSIGNED: %s ***\n", player + 1, addrStr(from));
				maplecast_telemetry::send("[maplecast] P%d ASSIGNED: %s", player + 1, addrStr(from));

				if (_playerCount == 2)
				{
					printf("[maplecast] *** BOTH PLAYERS CONNECTED ***\n");
					printf("[maplecast]   P1: %s\n", addrStr(_playerAddr[0]));
					printf("[maplecast]   P2: %s\n", addrStr(_playerAddr[1]));
					maplecast_telemetry::send("[maplecast] BOTH PLAYERS CONNECTED");
				}
			}
			else
			{
				// Slots full — check if this is a reconnect (same IP, different port)
				// This happens when pc_gamepad_sender.py restarts
				for (int i = 0; i < 2; i++)
				{
					if (_playerAddr[i].sin_addr.s_addr == from.sin_addr.s_addr)
					{
						// Same IP, different port — reassign
						printf("[maplecast] P%d RECONNECTED: %s (was %s)\n",
							i + 1, addrStr(from), addrStr(_playerAddr[i]));
						maplecast_telemetry::send("[maplecast] P%d RECONNECTED: %s", i + 1, addrStr(from));
						_playerAddr[i] = from;
						player = i;
						break;
					}
				}

				if (player < 0)
				{
					_unknownPackets++;
					if (_unknownPackets <= 5)
					{
						printf("[maplecast] REJECTED packet from %s (slots full)\n", addrStr(from));
					}
				}
			}
		}

		if (player >= 0 && player < 2)
		{
			// Track state changes
			if (memcmp(_w3[player], buf, 4) != 0)
			{
				_stateChanges[player]++;
				_chgAccum[player]++;
			}

			memcpy(_prevW3[player], _w3[player], 4);
			_w3[player][0] = buf[0];
			_w3[player][1] = buf[1];
			_w3[player][2] = buf[2];
			_w3[player][3] = buf[3];
			_packetCount[player]++;
			_packetsThisFrame[player]++;
			_pktAccum[player]++;
		}
	}

	// Write into mapleInputState
	w3ToInput(_w3[0], inputState[0]);
	w3ToInput(_w3[1], inputState[1]);

	// Timestamp this input read for latency telemetry
	LARGE_INTEGER _qpc, _qpf;
	QueryPerformanceFrequency(&_qpf);
	QueryPerformanceCounter(&_qpc);
	_lastInputTimeUs = _qpc.QuadPart * 1000000LL / _qpf.QuadPart;

	_getInputCount++;

	// Update per-second rates every 60 frames
	if (_getInputCount - _lastRateFrame >= 60)
	{
		for (int i = 0; i < 2; i++)
		{
			_pktPerSec[i] = _pktAccum[i] * 60 / (_getInputCount - _lastRateFrame);
			_chgPerSec[i] = _chgAccum[i] * 60 / (_getInputCount - _lastRateFrame);
			_pktAccum[i] = 0;
			_chgAccum[i] = 0;
		}
		_lastRateFrame = _getInputCount;
	}

	// Telemetry every 300 frames (5 seconds at 60fps)
	if (_getInputCount % 300 == 0)
	{
		printf("[maplecast] frame:%u | P1: %u pkts, %u changes, btns=0x%02X%02X | P2: %u pkts, %u changes, btns=0x%02X%02X | rejected:%u\n",
			_getInputCount,
			_packetCount[0], _stateChanges[0], _w3[0][2], _w3[0][3],
			_packetCount[1], _stateChanges[1], _w3[1][2], _w3[1][3],
			_unknownPackets);

		maplecast_telemetry::send(
			"[maplecast] frame:%u | P1:%u pkts/%u chg/0x%02X%02X | P2:%u pkts/%u chg/0x%02X%02X | rej:%u | connected:%d",
			_getInputCount,
			_packetCount[0], _stateChanges[0], _w3[0][2], _w3[0][3],
			_packetCount[1], _stateChanges[1], _w3[1][2], _w3[1][3],
			_unknownPackets, _playerCount);

		// Reset per-interval counters
		_packetCount[0] = 0;
		_packetCount[1] = 0;
		_stateChanges[0] = 0;
		_stateChanges[1] = 0;
		_unknownPackets = 0;
	}
}

bool active()
{
	return _active;
}

int64_t lastInputTimeUs()
{
	return _lastInputTimeUs;
}

void getPlayerStats(PlayerStats& p1, PlayerStats& p2)
{
	for (int i = 0; i < 2; i++)
	{
		PlayerStats& s = (i == 0) ? p1 : p2;
		s.packetsPerSec = _pktPerSec[i];
		s.changesPerSec = _chgPerSec[i];
		s.buttons = ((uint16_t)_w3[i][2] << 8) | _w3[i][3];
		s.lt = _w3[i][0];
		s.rt = _w3[i][1];
		s.connected = _playerAssigned[i];
	}
}

}  // namespace maplecast
