/*
	MapleCast — Flycast IS the server.

	Receives gamepad input from pc_gamepad_sender.py over UDP.
	P1 on port 7101, P2 on port 7102.
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
static SOCKET _sockP1 = INVALID_SOCK;
static SOCKET _sockP2 = INVALID_SOCK;

// Latest received W3 state per player (LT, RT, buttons_hi, buttons_lo)
// Updated by non-blocking recv in getInput()
static u8 _p1W3[4] = { 0, 0, 0xFF, 0xFF };  // all buttons released
static u8 _p2W3[4] = { 0, 0, 0xFF, 0xFF };

static SOCKET createUdpListener(int port)
{
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCK)
		return INVALID_SOCK;

	// Non-blocking
#ifdef _WIN32
	u_long mode = 1;
	ioctlsocket(s, FIONBIO, &mode);
#else
	int flags = fcntl(s, F_GETFL, 0);
	fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons((u16)port);

	if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCK_ERROR)
	{
		closesocket(s);
		return INVALID_SOCK;
	}

	return s;
}

// Drain all pending packets from socket, keep the latest
static void drainLatest(SOCKET s, u8 w3[4])
{
	u8 buf[64];
	struct sockaddr_in from;
	socklen_t fromLen = sizeof(from);

	// Read all pending packets, keep only the freshest
	while (true)
	{
		int n = recvfrom(s, (char*)buf, sizeof(buf), 0,
			(struct sockaddr*)&from, &fromLen);
		if (n >= 4)
		{
			// W3 format: LT, RT, buttons_hi, buttons_lo (big endian from sender)
			w3[0] = buf[0];
			w3[1] = buf[1];
			w3[2] = buf[2];
			w3[3] = buf[3];
		}
		else
		{
			break;  // No more packets
		}
	}
}

// Parse W3 into MapleInputState
static void w3ToInput(const u8 w3[4], MapleInputState& state)
{
	// W3 from pc_gamepad_sender.py: {LT, RT, buttons_hi, buttons_lo} big-endian
	u16 buttons = ((u16)w3[2] << 8) | w3[3];  // active-low: 0=pressed

	state.kcode = buttons | 0xFFFF0000;  // upper bits always 1
	state.halfAxes[PJTI_L] = (u16)w3[0] << 8;  // LT: 0-255 → 0-65280
	state.halfAxes[PJTI_R] = (u16)w3[1] << 8;  // RT: 0-255 → 0-65280

	// No analog sticks from W3 — center position
	state.fullAxes[PJAI_X1] = 0;
	state.fullAxes[PJAI_Y1] = 0;
	state.fullAxes[PJAI_X2] = 0;
	state.fullAxes[PJAI_Y2] = 0;
}

bool init(int p1Port, int p2Port)
{
#ifdef _WIN32
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

	_sockP1 = createUdpListener(p1Port);
	if (_sockP1 == INVALID_SOCK)
	{
		printf("[maplecast] failed to bind P1 port %d\n", p1Port);
		return false;
	}

	_sockP2 = createUdpListener(p2Port);
	if (_sockP2 == INVALID_SOCK)
	{
		printf("[maplecast] failed to bind P2 port %d\n", p2Port);
		closesocket(_sockP1);
		_sockP1 = INVALID_SOCK;
		return false;
	}

	_active = true;
	printf("[maplecast] server mode — listening P1:%d P2:%d\n", p1Port, p2Port);
	printf("[maplecast] run: python pc_gamepad_sender.py 127.0.0.1 %d\n", p1Port);
	printf("[maplecast] run: python pc_gamepad_sender.py 127.0.0.1 %d\n", p2Port);
	return true;
}

void shutdown()
{
	if (_sockP1 != INVALID_SOCK) { closesocket(_sockP1); _sockP1 = INVALID_SOCK; }
	if (_sockP2 != INVALID_SOCK) { closesocket(_sockP2); _sockP2 = INVALID_SOCK; }
	_active = false;
	printf("[maplecast] shutdown\n");
}

void getInput(MapleInputState inputState[4])
{
	// Drain all pending UDP packets — keep latest state per player
	drainLatest(_sockP1, _p1W3);
	drainLatest(_sockP2, _p2W3);

	// Write into mapleInputState — game sees this
	w3ToInput(_p1W3, inputState[0]);
	w3ToInput(_p2W3, inputState[1]);
}

bool active()
{
	return _active;
}

}  // namespace maplecast
