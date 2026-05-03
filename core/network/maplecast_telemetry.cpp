/*
	MapleCast Telemetry â€” fire-and-forget UDP logging.
	Sends to localhost:7300. If nothing is listening, packets are silently dropped.
	Zero impact on game performance â€” non-blocking UDP sendto.
*/
#include "types.h"  // u32 etc., needed before net_platform.h
#include "maplecast_telemetry.h"
#include "net_platform.h"
#include "maplecast_compat.h"
#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace maplecast_telemetry
{

static SOCKET _sock = INVALID_SOCKET;
static struct sockaddr_in _dest;

void init(int port)
{
	_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (_sock == INVALID_SOCKET) return;

	memset(&_dest, 0, sizeof(_dest));
	_dest.sin_family = AF_INET;
	_dest.sin_port = htons((unsigned short)port);
	inet_pton(AF_INET, "127.0.0.1", &_dest.sin_addr);
}

void send(const char* fmt, ...)
{
	if (_sock == INVALID_SOCKET) return;

	char buf[1024];
	va_list args;
	va_start(args, fmt);
	int len = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (len > 0)
		mc_sendto(_sock, buf, len, 0, (struct sockaddr*)&_dest, sizeof(_dest));
}

}
