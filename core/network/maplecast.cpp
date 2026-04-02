/*
	MapleCast — Flycast IS the server.

	One UDP port. Auto-assign P1/P2 by connection order.
	First unique sender = P1. Second unique sender = P2.
	4-byte W3 packet: {LT, RT, buttons_hi, buttons_lo}

	That's it. Flycast runs the game. One state. Zero desync.
*/
#include "maplecast.h"
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
// First unique sender = P1, second = P2
static struct sockaddr_in _playerAddr[2];
static bool _playerAssigned[2] = { false, false };
static int _playerCount = 0;

// Latest received W3 state per player
static u8 _w3[2][4] = {
	{ 0, 0, 0xFF, 0xFF },  // P1: all buttons released
	{ 0, 0, 0xFF, 0xFF },  // P2: all buttons released
};

// Compare two sockaddr_in (ip + port)
static bool addrMatch(const struct sockaddr_in& a, const struct sockaddr_in& b)
{
	return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

// Parse W3 into MapleInputState
static void w3ToInput(const u8 w3[4], MapleInputState& state)
{
	u16 buttons = ((u16)w3[2] << 8) | w3[3];  // active-low: 0=pressed

	state.kcode = buttons | 0xFFFF0000;
	state.halfAxes[PJTI_L] = (u16)w3[0] << 8;
	state.halfAxes[PJTI_R] = (u16)w3[1] << 8;

	state.fullAxes[PJAI_X1] = 0;
	state.fullAxes[PJAI_Y1] = 0;
	state.fullAxes[PJAI_X2] = 0;
	state.fullAxes[PJAI_Y2] = 0;
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

	// Non-blocking
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

	_active = true;
	printf("[maplecast] server mode — listening on port %d\n", port);
	printf("[maplecast] first sender = P1, second sender = P2\n");
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

		// Identify which player this packet is from
		int player = -1;
		for (int i = 0; i < 2; i++)
		{
			if (_playerAssigned[i] && addrMatch(_playerAddr[i], from))
			{
				player = i;
				break;
			}
		}

		// New sender — auto-assign next available slot
		if (player < 0 && _playerCount < 2)
		{
			player = _playerCount;
			_playerAddr[player] = from;
			_playerAssigned[player] = true;
			_playerCount++;

			char ipStr[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &from.sin_addr, ipStr, sizeof(ipStr));
			printf("[maplecast] P%d assigned: %s:%d\n",
				player + 1, ipStr, ntohs(from.sin_port));
		}

		if (player >= 0 && player < 2)
		{
			_w3[player][0] = buf[0];
			_w3[player][1] = buf[1];
			_w3[player][2] = buf[2];
			_w3[player][3] = buf[3];
		}
	}

	// Write into mapleInputState
	w3ToInput(_w3[0], inputState[0]);
	w3ToInput(_w3[1], inputState[1]);
}

bool active()
{
	return _active;
}

}  // namespace maplecast
