/*
	MapleCast Windows compat shims.

	Small set of POSIX/GCC-isms we use across the networking layer that
	MSVC doesn't ship by default. Each definition is a thin wrapper that
	gives identical behavior on Windows; on Linux this header is a no-op.

	Include this where you need clock_gettime / strcasecmp / ssize_t /
	__builtin_popcountll on Windows. The networking layer already pulls
	net_platform.h for sockets — keep socket stuff there, keep the
	scalar-tooling stuff here.
*/
#pragma once

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>  // _putenv_s
#include <direct.h>  // _mkdir
#include <thread>
#include <chrono>
#include <atomic>

// ssize_t — POSIX type, MSVC has SSIZE_T in BaseTsd.h with the same shape.
#ifndef _SSIZE_T_DEFINED
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED
#endif

// strcasecmp / strncasecmp — POSIX names; MSVC ships _stricmp / _strnicmp.
#ifndef strcasecmp
#define strcasecmp  _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif

// __builtin_popcountll — GCC/Clang builtin. MSVC has __popcnt64 in <intrin.h>
// with identical semantics for our usage (count of set bits in a uint64_t).
#define __builtin_popcountll(v) ((int)__popcnt64((unsigned long long)(v)))

// clock_gettime / CLOCK_MONOTONIC — POSIX. We approximate CLOCK_MONOTONIC
// with QueryPerformanceCounter, which is what every Windows monotonic-clock
// shim does. Resolution is sub-microsecond, never goes backwards, unaffected
// by wall-clock changes — same guarantees as CLOCK_MONOTONIC on Linux.
//
// MSVC's <time.h> defines `struct timespec` since C11 / VS 2015, so we rely
// on it here rather than redeclaring (which conflicts).
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME  0
#endif

static inline int clock_gettime(int clk_id, struct timespec* ts) {
	(void)clk_id;
	LARGE_INTEGER freq, ctr;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&ctr);
	long long ns = (long long)((double)ctr.QuadPart * 1e9 / (double)freq.QuadPart);
	ts->tv_sec  = (time_t)(ns / 1000000000LL);
	ts->tv_nsec = (long)(ns % 1000000000LL);
	return 0;
}

// MSG_DONTWAIT / MSG_NOSIGNAL — POSIX flags that don't exist on Windows.
// MSG_DONTWAIT: handled by ioctlsocket(FIONBIO) instead — set the socket
//   non-blocking up front and skip the per-call flag.
// MSG_NOSIGNAL: irrelevant on Windows — there's no SIGPIPE.
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// SHUT_RDWR / SHUT_RD / SHUT_WR — POSIX shutdown() how-arg names.
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#endif
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif

// close() on a socket → closesocket() on Windows. We don't want to redirect
// close() globally (it's used for files too), so callers should use the
// existing net_platform.h helper that defines closesocket() as close() on
// Linux. For brevity in this header: nothing.

// ── Socket I/O wrappers ───────────────────────────────────────────────
// Winsock requires `char*` casts on every recv/send/sendto/recvfrom/setsockopt
// call where the MapleCast networking code uses `uint8_t*`/`int*`. Rather
// than annotate every call site, define wrapper functions whose signatures
// take `void*` and let MSVC do the cast internally. On Linux these are
// macro pass-throughs to the POSIX names — zero overhead, zero behavior
// change. Migrate call sites in MapleCast files: `recv(` → `mc_recv(`, etc.
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t_compat;
inline int mc_recv(SOCKET s, void* b, size_t n, int f) {
	return ::recv(s, (char*)b, (int)n, f);
}
inline int mc_send(SOCKET s, const void* b, size_t n, int f) {
	return ::send(s, (const char*)b, (int)n, f);
}
inline int mc_recvfrom(SOCKET s, void* b, size_t n, int f, struct sockaddr* a, int* al) {
	return ::recvfrom(s, (char*)b, (int)n, f, a, al);
}
inline int mc_sendto(SOCKET s, const void* b, size_t n, int f, const struct sockaddr* a, int al) {
	return ::sendto(s, (const char*)b, (int)n, f, a, al);
}
inline int mc_setsockopt(SOCKET s, int l, int o, const void* v, int vl) {
	return ::setsockopt(s, l, o, (const char*)v, vl);
}
inline int mc_getsockopt(SOCKET s, int l, int o, void* v, int* vl) {
	return ::getsockopt(s, l, o, (char*)v, vl);
}
// File-vs-socket close disambiguation: maplecast networking files always
// mean closesocket() when they call close() on a SOCKET handle.
#define mc_closesocket closesocket

// usleep — POSIX. Drop-in replacement using std::this_thread::sleep_for.
static inline int usleep(unsigned long usec) {
	std::this_thread::sleep_for(std::chrono::microseconds(usec));
	return 0;
}

// setenv — POSIX. MSVC has _putenv_s with similar semantics. The third
// arg `overwrite` is ignored — _putenv_s always overwrites, which matches
// the MapleCast call sites that always pass 1.
static inline int setenv(const char* name, const char* value, int overwrite) {
	(void)overwrite;
	return _putenv_s(name, value);
}

// mkdir(path, mode) — POSIX two-arg. _mkdir on Windows ignores mode.
// Shadow the call so existing two-arg sites work.
#define mkdir(path, mode) _mkdir(path)

// __sync_synchronize — GCC builtin. C++11 has the standard equivalent.
#define __sync_synchronize() std::atomic_thread_fence(std::memory_order_seq_cst)

#endif // _WIN32

#ifndef _WIN32
// Linux pass-throughs — same names, no-op.
#define mc_recv     recv
#define mc_send     send
#define mc_recvfrom recvfrom
#define mc_sendto   sendto
#define mc_setsockopt setsockopt
#define mc_getsockopt getsockopt
#define mc_closesocket close
#endif
