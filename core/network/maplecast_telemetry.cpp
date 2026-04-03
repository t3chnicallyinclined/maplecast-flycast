/*
	MapleCast Telemetry — fire-and-forget UDP logging.
	Sends to localhost:7300. If nothing is listening, packets are silently dropped.
	Zero impact on game performance — non-blocking UDP sendto.
*/
#include "maplecast_telemetry.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define closesocket close
typedef int SOCKET;
#define INVALID_SOCKET -1
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
		sendto(_sock, buf, len, 0, (struct sockaddr*)&_dest, sizeof(_dest));
}

}
