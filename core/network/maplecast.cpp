/*
	MapleCast — server-clocked lockstep netplay for Dreamcast.

	Maple Bus is the protocol. CMD9 is the API. The network is just a longer wire.
*/
#include "maplecast.h"
#include "hw/maple/maple_cfg.h"
#include "input/gamepad.h"
#include "cfg/option.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
typedef int SOCKET;
#endif

#include <cstring>
#include <chrono>

// Must match maplecast-server protocol.rs
static constexpr int CMD9_SIZE = 32;

// Message types — must match relay.rs
static constexpr u8 MSG_REGISTER = 0x00;
static constexpr u8 MSG_INPUT    = 0x01;
static constexpr u8 MSG_DISCONNECT = 0x03;

#pragma pack(push, 1)

// Server → Client tick (68 bytes)
struct ServerTick
{
	u32 frame;
	u8  p1_cmd9[CMD9_SIZE];
	u8  p2_cmd9[CMD9_SIZE];
};

// Server → Client event (64 bytes)
struct ServerEvent
{
	u32 match_id;
	u8  event;
	u8  _pad[3];
	u8  payload[56];
};

#pragma pack(pop)

static constexpr u8 EVENT_MATCH_START = 0x02;

namespace maplecast
{

static bool _active = false;
static Config _cfg;
static SOCKET _sock = INVALID_SOCKET;
static struct sockaddr_in _serverAddr;

static u32 _frame = 0;
static float _fps = 60.0f;
static float _rtt = 0.0f;

static auto _lastTickTime = std::chrono::high_resolution_clock::now();

// Build CMD9 response from local MapleInputState — same format as GP2040-CE
// This is what a real Dreamcast controller returns for GETCOND
static void buildCmd9FromInput(const MapleInputState& input, u8 cmd9[CMD9_SIZE])
{
	memset(cmd9, 0, CMD9_SIZE);

	// Function type: controller (MFID_0_Input = 0x00000001)
	u32 func = 0x01000000;
	memcpy(&cmd9[0], &func, 4);

	// Buttons — kcode is active-low (0 = pressed), 16-bit
	u16 kcode = (u16)(input.kcode & 0xFFFF);
	memcpy(&cmd9[4], &kcode, 2);

	// Right trigger
	cmd9[6] = (u8)(input.halfAxes[PJTI_R] >> 8);
	// Left trigger
	cmd9[7] = (u8)(input.halfAxes[PJTI_L] >> 8);

	// Analog stick X (convert from signed to unsigned 0-255, 128=center)
	cmd9[8] = (u8)((input.fullAxes[PJAI_X1] >> 8) + 128);
	// Analog stick Y
	cmd9[9] = (u8)((input.fullAxes[PJAI_Y1] >> 8) + 128);

	// Second analog stick X
	cmd9[10] = (u8)((input.fullAxes[PJAI_X2] >> 8) + 128);
	// Second analog stick Y
	cmd9[11] = (u8)((input.fullAxes[PJAI_Y2] >> 8) + 128);
}

// Parse CMD9 response back into MapleInputState
static void parseCmd9ToInput(const u8 cmd9[CMD9_SIZE], MapleInputState& input)
{
	// Buttons
	u16 kcode;
	memcpy(&kcode, &cmd9[4], 2);
	input.kcode = kcode | 0xFFFF0000;  // upper bits always 1

	// Right trigger
	input.halfAxes[PJTI_R] = (u16)cmd9[6] << 8;
	// Left trigger
	input.halfAxes[PJTI_L] = (u16)cmd9[7] << 8;

	// Analog stick X (convert from unsigned back to signed)
	input.fullAxes[PJAI_X1] = ((int16_t)cmd9[8] - 128) << 8;
	// Analog stick Y
	input.fullAxes[PJAI_Y1] = ((int16_t)cmd9[9] - 128) << 8;

	// Second analog stick
	input.fullAxes[PJAI_X2] = ((int16_t)cmd9[10] - 128) << 8;
	input.fullAxes[PJAI_Y2] = ((int16_t)cmd9[11] - 128) << 8;
}

// Send raw bytes to server
static bool sendToServer(const void* data, int len)
{
	int sent = sendto(_sock, (const char*)data, len, 0,
		(struct sockaddr*)&_serverAddr, sizeof(_serverAddr));
	return sent == len;
}

// Register with server
static bool sendRegister()
{
	u8 pkt[6];
	u32 matchId = (u32)_cfg.matchId;
	memcpy(&pkt[0], &matchId, 4);
	pkt[4] = MSG_REGISTER;
	pkt[5] = (u8)_cfg.localPlayer;
	return sendToServer(pkt, sizeof(pkt));
}

// Send CMD9 input to server
static bool sendInput(u32 frame, const u8 cmd9[CMD9_SIZE])
{
	u8 pkt[42];
	u32 matchId = (u32)_cfg.matchId;
	memcpy(&pkt[0], &matchId, 4);         // match_id
	pkt[4] = MSG_INPUT;                     // msg_type
	pkt[5] = (u8)_cfg.localPlayer;          // player
	memcpy(&pkt[6], &frame, 4);             // frame
	memcpy(&pkt[10], cmd9, CMD9_SIZE);      // cmd9
	return sendToServer(pkt, sizeof(pkt));
}

// Send disconnect
static void sendDisconnect()
{
	u8 pkt[6];
	u32 matchId = (u32)_cfg.matchId;
	memcpy(&pkt[0], &matchId, 4);
	pkt[4] = MSG_DISCONNECT;
	pkt[5] = (u8)_cfg.localPlayer;
	sendToServer(pkt, sizeof(pkt));
}

bool init(const Config& cfg)
{
	_cfg = cfg;
	_frame = 0;

#ifdef _WIN32
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

	// Create UDP socket
	_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (_sock == INVALID_SOCKET)
	{
		printf("[maplecast] failed to create socket\n");
		return false;
	}

	// Bind to any port
	struct sockaddr_in localAddr;
	memset(&localAddr, 0, sizeof(localAddr));
	localAddr.sin_family = AF_INET;
	localAddr.sin_addr.s_addr = INADDR_ANY;
	localAddr.sin_port = 0;
	if (bind(_sock, (struct sockaddr*)&localAddr, sizeof(localAddr)) == SOCKET_ERROR)
	{
		printf("[maplecast] failed to bind socket\n");
		closesocket(_sock);
		_sock = INVALID_SOCKET;
		return false;
	}

	// Set server address
	memset(&_serverAddr, 0, sizeof(_serverAddr));
	_serverAddr.sin_family = AF_INET;
	_serverAddr.sin_port = htons((u16)cfg.serverPort);
	inet_pton(AF_INET, cfg.serverAddr.c_str(), &_serverAddr.sin_addr);

	// Register with server
	if (!sendRegister())
	{
		printf("[maplecast] failed to register with server\n");
		closesocket(_sock);
		_sock = INVALID_SOCKET;
		return false;
	}

	printf("[maplecast] registered with %s:%d as P%d, match %d\n",
		cfg.serverAddr.c_str(), cfg.serverPort, cfg.localPlayer + 1, cfg.matchId);

	// Wait for MATCH_START event
	char buf[256];
	struct sockaddr_in fromAddr;
	socklen_t fromLen = sizeof(fromAddr);

	// Block with timeout waiting for match start
#ifdef _WIN32
	DWORD timeout = 30000; // 30 seconds
	setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
	struct timeval tv;
	tv.tv_sec = 30;
	tv.tv_usec = 0;
	setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

	int n = recvfrom(_sock, buf, sizeof(buf), 0,
		(struct sockaddr*)&fromAddr, &fromLen);
	if (n <= 0)
	{
		printf("[maplecast] timeout waiting for match start\n");
		closesocket(_sock);
		_sock = INVALID_SOCKET;
		return false;
	}

	printf("[maplecast] match started! Entering server-clocked mode.\n");

	_active = true;
	_lastTickTime = std::chrono::high_resolution_clock::now();
	return true;
}

void shutdown()
{
	if (_sock != INVALID_SOCKET)
	{
		if (_active)
			sendDisconnect();
		closesocket(_sock);
		_sock = INVALID_SOCKET;
	}
	_active = false;
	_frame = 0;

	printf("[maplecast] disconnected\n");
}

bool waitForTick(MapleInputState inputState[4])
{
	if (!_active)
		return false;

	auto tickStart = std::chrono::high_resolution_clock::now();

	// 1. Read local input from Flycast's normal input system (SDL/gamepad)
	//    This is what the local player is pressing RIGHT NOW.
	//    We don't touch inputState here — Flycast's normal input polling already populated it.
	u8 localCmd9[CMD9_SIZE];
	buildCmd9FromInput(inputState[_cfg.localPlayer], localCmd9);

	// 2. Send our CMD9 to the server
	sendInput(_frame, localCmd9);

	// 3. BLOCK until server tick arrives.
	//    This is THE frame clock. We do not advance until the server says so.
	//    Both players block here simultaneously.
	ServerTick tick;
	char buf[256];
	struct sockaddr_in fromAddr;
	socklen_t fromLen = sizeof(fromAddr);

	while (true)
	{
#ifdef _WIN32
		DWORD timeout = _cfg.tournament ? 100 : 5000;
		setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
		struct timeval tv;
		tv.tv_sec = _cfg.tournament ? 0 : 5;
		tv.tv_usec = _cfg.tournament ? 100000 : 0;
		setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

		int n = recvfrom(_sock, (char*)buf, sizeof(buf), 0,
			(struct sockaddr*)&fromAddr, &fromLen);

		if (n == sizeof(ServerTick))
		{
			memcpy(&tick, buf, sizeof(ServerTick));
			if (tick.frame == _frame + 1)
				break; // Got the tick we're waiting for
			// Wrong frame number — discard and keep waiting
			continue;
		}

		if (n > 0)
		{
			// Non-tick packet (event?) — discard during gameplay
			continue;
		}

		// Timeout or error
		if (_cfg.tournament)
		{
			printf("[maplecast] connection lost (tournament timeout)\n");
			_active = false;
			return false;
		}
		// Casual mode: keep waiting, game freezes
	}

	// 4. Apply synced inputs to mapleInputState
	//    Player 1 gets p1_cmd9, Player 2 gets p2_cmd9
	//    Flycast will read these when maple_DoDma() fires during the frame
	parseCmd9ToInput(tick.p1_cmd9, inputState[0]);
	parseCmd9ToInput(tick.p2_cmd9, inputState[1]);

	_frame = tick.frame;

	// 5. Update stats
	auto now = std::chrono::high_resolution_clock::now();
	auto frameTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
		now - _lastTickTime).count();
	if (frameTimeUs > 0)
		_fps = _fps * 0.9f + (1000000.0f / (float)frameTimeUs) * 0.1f;
	_lastTickTime = now;

	return true;
}

bool active()
{
	return _active;
}

float currentFps()
{
	return _fps;
}

float rttMs()
{
	return _rtt;
}

int currentFrame()
{
	return (int)_frame;
}

}  // namespace maplecast
