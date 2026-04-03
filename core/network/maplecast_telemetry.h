/*
	MapleCast Telemetry — sends diagnostic strings over UDP.
	Zero overhead when no telemetry server is listening (fire-and-forget UDP).
*/
#pragma once

namespace maplecast_telemetry
{

void init(int port = 7300);
void send(const char* fmt, ...);

}
