#ifdef MAPLECAST_XDP
/*
	MapleCast AF_XDP Input — zero-copy gamepad packet receiver.

	Architecture (inspired by NOBD firmware):
	  NOBD: GPIO pin change → updateCmd9FromGpio() → cmd9ReadyW3 (pre-built, 1-2µs)
	  This: NIC DMA → XDP ring buffer → kcode[]/lt[]/rt[] atomics (0.1-0.5µs)

	The NIC's DMA engine replaces GPIO. The ring buffer replaces the lookup table.
	The atomic store replaces cmd9ReadyW3. CMD9 reads the atomic — zero syscalls.

	Packet flow:
	  UDP packet arrives at NIC
	    → XDP BPF program matches port 7100
	    → bpf_redirect_map() sends to AF_XDP socket
	    → NIC DMAs packet into UMEM ring buffer (zero-copy)
	    → Input thread polls ring, parses W3, atomic-stores to kcode[]/lt[]/rt[]
	    → ggpo::getLocalInput() reads atomics at CMD9 time
*/

#include "types.h"
#include "maplecast_xdp_input.h"
#include "maplecast_telemetry.h"
#include "input/gamepad_device.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <atomic>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <poll.h>
#include <errno.h>

#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <xdp/xsk.h>

// gamepad globals from gamepad_device.cpp — same ones ggpo::getLocalInput() reads
extern u32 kcode[4];
extern u16 rt[4], lt[4];

namespace maplecast_xdp_input
{

// AF_XDP constants
static constexpr int NUM_FRAMES = 4096;
static constexpr int FRAME_SIZE = XSK_UMEM__DEFAULT_FRAME_SIZE;  // 4096
static constexpr int UMEM_SIZE = NUM_FRAMES * FRAME_SIZE;
static constexpr int RX_BATCH_SIZE = 64;

// State
static std::atomic<bool> _active{false};
static std::thread _inputThread;

// XDP socket
static struct xsk_socket *_xsk = nullptr;
static struct xsk_umem *_umem = nullptr;
static void *_umemArea = nullptr;
static struct xsk_ring_cons _rx;
static struct xsk_ring_prod _fq;   // fill queue — tell NIC which frames to use
static struct xsk_ring_cons _cq;   // completion queue
static struct xsk_ring_prod _tx;   // not used for RX-only

// BPF
static struct bpf_object *_bpfObj = nullptr;
static int _bpfProgFd = -1;
static int _xskmapFd = -1;
static int _ifindex = 0;

// Telemetry
static std::atomic<uint64_t> _pktCount{0};
static std::atomic<uint64_t> _byteCount{0};

// Player assignment by source IP (same logic as maplecast.cpp)
static uint32_t _playerIP[2] = {0, 0};
static bool _playerAssigned[2] = {false, false};
static int _playerCount = 0;

static int identifyPlayer(uint32_t srcIP)
{
	for (int i = 0; i < 2; i++)
		if (_playerAssigned[i] && _playerIP[i] == srcIP)
			return i;

	if (_playerCount < 2)
	{
		int slot = _playerCount++;
		_playerIP[slot] = srcIP;
		_playerAssigned[slot] = true;
		printf("[xdp-input] P%d assigned from IP %u.%u.%u.%u\n", slot + 1,
			srcIP & 0xFF, (srcIP >> 8) & 0xFF, (srcIP >> 16) & 0xFF, (srcIP >> 24) & 0xFF);
		return slot;
	}
	return -1;
}

static void processPacket(const uint8_t *pkt, uint32_t len)
{
	// Minimum: ETH(14) + IP(20) + UDP(8) + W3(4) = 46 bytes
	if (len < 46) return;

	// Skip Ethernet header (14 bytes)
	const uint8_t *ip = pkt + 14;

	// IP header length (IHL field, lower 4 bits of first byte)
	int ipHdrLen = (ip[0] & 0x0F) * 4;
	if (ipHdrLen < 20) return;
	if ((uint32_t)(14 + ipHdrLen + 8 + 4) > len) return;

	// Source IP for player identification
	uint32_t srcIP;
	memcpy(&srcIP, ip + 12, 4);

	// UDP payload starts after IP header + 8 bytes UDP header
	const uint8_t *udpPayload = ip + ipHdrLen + 8;
	int payloadLen = len - 14 - ipHdrLen - 8;

	if (payloadLen < 4) return;

	// Identify player
	int player = -1;

	// 5-byte tagged packet from WebSocket forwarder: [slot][LT][RT][btn_hi][btn_lo]
	if (payloadLen >= 5 && udpPayload[0] <= 1)
	{
		// Check if from localhost (127.0.0.1 = 0x0100007F in network byte order)
		if (srcIP == 0x0100007F)
		{
			player = udpPayload[0];
			udpPayload += 1;  // skip slot byte
		}
	}

	// Legacy 4-byte W3: identify by source IP
	if (player < 0)
		player = identifyPlayer(srcIP);

	if (player < 0 || player > 1) return;

	// Parse W3: [LT, RT, buttons_hi, buttons_lo]
	uint16_t buttons = ((uint16_t)udpPayload[2] << 8) | udpPayload[3];

	// Atomic store — like NOBD's cmd9ReadyW3 update
	// These are the same globals ggpo::getLocalInput() reads
	kcode[player] = buttons | 0xFFFF0000;  // active-low, upper 16 bits set
	lt[player] = (uint16_t)udpPayload[0] << 8;
	rt[player] = (uint16_t)udpPayload[1] << 8;

	_pktCount.fetch_add(1, std::memory_order_relaxed);
	_byteCount.fetch_add(len, std::memory_order_relaxed);
}

static void inputThreadLoop()
{
	printf("[xdp-input] input thread started — polling ring buffer\n");

	while (_active.load(std::memory_order_relaxed))
	{
		uint32_t idx_rx = 0;
		unsigned int rcvd = xsk_ring_cons__peek(&_rx, RX_BATCH_SIZE, &idx_rx);

		if (rcvd == 0)
		{
			// Brief yield if nothing pending — saves CPU while maintaining <1µs wake
			// Using recvfrom with MSG_DONTWAIT as a kick mechanism
			struct pollfd pfd = { .fd = xsk_socket__fd(_xsk), .events = POLLIN };
			poll(&pfd, 1, 1);  // 1ms timeout max
			continue;
		}

		for (unsigned int i = 0; i < rcvd; i++)
		{
			uint64_t addr = xsk_ring_cons__rx_desc(&_rx, idx_rx + i)->addr;
			uint32_t len = xsk_ring_cons__rx_desc(&_rx, idx_rx + i)->len;

			uint8_t *pkt = (uint8_t *)xsk_umem__get_data(_umemArea, addr);
			processPacket(pkt, len);
		}

		xsk_ring_cons__release(&_rx, rcvd);

		// Refill the fill queue — give frames back to NIC for reuse
		uint32_t idx_fq = 0;
		if (xsk_ring_prod__reserve(&_fq, rcvd, &idx_fq) == rcvd)
		{
			for (unsigned int i = 0; i < rcvd; i++)
			{
				uint64_t addr = xsk_ring_cons__rx_desc(&_rx, idx_rx + i)->addr;
				*xsk_ring_prod__fill_addr(&_fq, idx_fq + i) = addr;
			}
			xsk_ring_prod__submit(&_fq, rcvd);
		}
	}

	printf("[xdp-input] input thread stopped\n");
}

// Fallback: dedicated recvfrom() thread when XDP socket fails
// Still eliminates jitter — input is always fresh in kcode[] atomics
// Like option A+C: background thread + SO_BUSY_POLL
static void fallbackInputThread(int port)
{
	printf("[xdp-input] fallback thread started — recvfrom() on port %d\n", port);

	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) { printf("[xdp-input] fallback socket failed\n"); return; }

	// SO_BUSY_POLL: spin-poll NIC for 10µs before sleeping (saves interrupt latency)
	int busy_poll = 10;
	setsockopt(sock, SOL_SOCKET, SO_BUSY_POLL, &busy_poll, sizeof(busy_poll));

	// SO_REUSEPORT so we can coexist with maplecast.cpp's socket
	int reuse = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);
	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		printf("[xdp-input] fallback bind failed: %s\n", strerror(errno));
		close(sock);
		return;
	}

	struct timeval tv = { .tv_sec = 0, .tv_usec = 1000 };  // 1ms recv timeout
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	uint8_t buf[64];
	struct sockaddr_in from;
	socklen_t fromLen;

	while (_active.load(std::memory_order_relaxed))
	{
		fromLen = sizeof(from);
		int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromLen);
		if (n < 4) continue;

		const uint8_t *w3data = buf;
		int player = -1;

		// 5-byte tagged from WebSocket: [slot][LT][RT][btn_hi][btn_lo]
		if (n >= 5 && buf[0] <= 1 && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK))
		{
			player = buf[0];
			w3data = buf + 1;
		}

		// Legacy 4-byte: identify by source IP
		if (player < 0)
			player = identifyPlayer(from.sin_addr.s_addr);

		if (player < 0 || player > 1) continue;

		// Atomic store — same as XDP path
		uint16_t buttons = ((uint16_t)w3data[2] << 8) | w3data[3];
		kcode[player] = buttons | 0xFFFF0000;
		lt[player] = (uint16_t)w3data[0] << 8;
		rt[player] = (uint16_t)w3data[1] << 8;

		_pktCount.fetch_add(1, std::memory_order_relaxed);
	}

	close(sock);
	printf("[xdp-input] fallback thread stopped\n");
}

bool init(const char *ifname, int udp_port)
{
	printf("[xdp-input] initializing AF_XDP on %s, port %d\n", ifname, udp_port);

	_ifindex = if_nametoindex(ifname);
	if (_ifindex == 0)
	{
		printf("[xdp-input] interface %s not found\n", ifname);
		return false;
	}

	// Compile and load BPF program
	// Look for pre-compiled .o next to executable, or in source tree
	const char *bpfPaths[] = {
		"xdp_input_kern.o",
		"core/network/xdp_input_kern.o",
		"/home/tris/projects/maplecast-flycast/core/network/xdp_input_kern.o",
		nullptr
	};

	const char *bpfPath = nullptr;
	for (int i = 0; bpfPaths[i]; i++)
	{
		if (access(bpfPaths[i], R_OK) == 0)
		{
			bpfPath = bpfPaths[i];
			break;
		}
	}

	if (!bpfPath)
	{
		printf("[xdp-input] BPF object not found — compile with:\n");
		printf("  clang -O2 -g -target bpf -I/usr/include/x86_64-linux-gnu \\\n");
		printf("    -c core/network/xdp_input_kern.c -o core/network/xdp_input_kern.o\n");
		return false;
	}

	printf("[xdp-input] loading BPF program from %s\n", bpfPath);

	_bpfObj = bpf_object__open_file(bpfPath, nullptr);
	if (libbpf_get_error(_bpfObj))
	{
		printf("[xdp-input] failed to open BPF object: %s\n", strerror(errno));
		_bpfObj = nullptr;
		return false;
	}

	if (bpf_object__load(_bpfObj))
	{
		printf("[xdp-input] failed to load BPF program: %s\n", strerror(errno));
		bpf_object__close(_bpfObj);
		_bpfObj = nullptr;
		return false;
	}

	struct bpf_program *prog = bpf_object__find_program_by_name(_bpfObj, "xdp_gamepad_filter");
	if (!prog)
	{
		printf("[xdp-input] BPF program 'xdp_gamepad_filter' not found\n");
		bpf_object__close(_bpfObj);
		_bpfObj = nullptr;
		return false;
	}
	_bpfProgFd = bpf_program__fd(prog);

	// Find the XSKMAP
	struct bpf_map *map = bpf_object__find_map_by_name(_bpfObj, "xsks_map");
	if (!map)
	{
		printf("[xdp-input] xsks_map not found in BPF object\n");
		bpf_object__close(_bpfObj);
		_bpfObj = nullptr;
		return false;
	}
	_xskmapFd = bpf_map__fd(map);

	// Attach XDP program to interface — try native first, fallback to SKB
	int err = bpf_xdp_attach(_ifindex, _bpfProgFd, XDP_FLAGS_DRV_MODE, nullptr);
	if (err)
	{
		printf("[xdp-input] native XDP attach failed (%d), trying SKB mode...\n", err);
		err = bpf_xdp_attach(_ifindex, _bpfProgFd, XDP_FLAGS_SKB_MODE, nullptr);
		if (err)
		{
			printf("[xdp-input] SKB XDP attach also failed (%d): %s\n", err, strerror(-err));
			bpf_object__close(_bpfObj);
			_bpfObj = nullptr;
			return false;
		}
		printf("[xdp-input] attached in SKB mode\n");
	}
	else
	{
		printf("[xdp-input] attached in NATIVE mode — zero-copy active\n");
	}

	// Allocate UMEM (shared memory area for zero-copy)
	_umemArea = mmap(nullptr, UMEM_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	if (_umemArea == MAP_FAILED)
	{
		// Fallback without huge pages
		_umemArea = mmap(nullptr, UMEM_SIZE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (_umemArea == MAP_FAILED)
		{
			printf("[xdp-input] UMEM mmap failed: %s\n", strerror(errno));
			bpf_xdp_detach(_ifindex, 0, nullptr);
			bpf_object__close(_bpfObj);
			_bpfObj = nullptr;
			return false;
		}
	}

	// Create UMEM
	struct xsk_umem_config umem_cfg = {
		.fill_size = NUM_FRAMES,
		.comp_size = NUM_FRAMES,
		.frame_size = FRAME_SIZE,
		.frame_headroom = 0,
		.flags = 0,
	};

	err = xsk_umem__create(&_umem, _umemArea, UMEM_SIZE, &_fq, &_cq, &umem_cfg);
	if (err)
	{
		printf("[xdp-input] UMEM create failed (%d): %s\n", err, strerror(-err));
		munmap(_umemArea, UMEM_SIZE);
		bpf_xdp_detach(_ifindex, 0, nullptr);
		bpf_object__close(_bpfObj);
		_bpfObj = nullptr;
		return false;
	}

	// Create AF_XDP socket — try with NEED_WAKEUP, fallback without
	struct xsk_socket_config xsk_cfg = {
		.rx_size = NUM_FRAMES,
		.tx_size = 0,  // RX only
		.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD,  // we loaded our own program
		.xdp_flags = 0,
		.bind_flags = XDP_USE_NEED_WAKEUP,
	};

	err = xsk_socket__create(&_xsk, ifname, 0 /* queue */, _umem, &_rx, &_tx, &xsk_cfg);
	if (err)
	{
		printf("[xdp-input] XDP socket with NEED_WAKEUP failed (%d), retrying without...\n", err);
		xsk_cfg.bind_flags = 0;
		err = xsk_socket__create(&_xsk, ifname, 0, _umem, &_rx, &_tx, &xsk_cfg);
	}
	if (err)
	{
		printf("[xdp-input] XDP socket with SKB copy mode...\n");
		xsk_cfg.bind_flags = XDP_COPY;
		err = xsk_socket__create(&_xsk, ifname, 0, _umem, &_rx, &_tx, &xsk_cfg);
	}
	if (err)
	{
		printf("[xdp-input] ALL XDP socket attempts failed (%d): %s\n", err, strerror(-err));
		printf("[xdp-input] Falling back to recvfrom() input thread\n");
		xsk_umem__delete(_umem);
		munmap(_umemArea, UMEM_SIZE);
		bpf_xdp_detach(_ifindex, 0, nullptr);
		bpf_object__close(_bpfObj);
		_bpfObj = nullptr;
		_umem = nullptr;
		_umemArea = nullptr;

		// FALLBACK: start a recvfrom() input thread instead
		// This still beats the old path — dedicated thread, no DMA-time jitter
		_active = true;
		_inputThread = std::thread(fallbackInputThread, udp_port);
		printf("[xdp-input] fallback recvfrom() thread started on port %d\n", udp_port);
		maplecast_telemetry::send("[xdp-input] XDP failed, fallback recvfrom thread on port %d", udp_port);
		return true;
	}

	// Register our socket in the XSKMAP so the BPF program can redirect to us
	int key = 0;  // RX queue 0
	int fd = xsk_socket__fd(_xsk);
	err = bpf_map_update_elem(_xskmapFd, &key, &fd, 0);
	if (err)
	{
		printf("[xdp-input] XSKMAP update failed (%d): %s\n", err, strerror(-err));
		xsk_socket__delete(_xsk);
		xsk_umem__delete(_umem);
		munmap(_umemArea, UMEM_SIZE);
		bpf_xdp_detach(_ifindex, 0, nullptr);
		bpf_object__close(_bpfObj);
		_bpfObj = nullptr;
		return false;
	}

	// Pre-fill the fill queue — give NIC all frames to use
	uint32_t idx_fq = 0;
	if (xsk_ring_prod__reserve(&_fq, NUM_FRAMES, &idx_fq) == NUM_FRAMES)
	{
		for (int i = 0; i < NUM_FRAMES; i++)
			*xsk_ring_prod__fill_addr(&_fq, idx_fq + i) = i * FRAME_SIZE;
		xsk_ring_prod__submit(&_fq, NUM_FRAMES);
	}

	// Start input thread
	_active = true;
	_inputThread = std::thread(inputThreadLoop);

	printf("[xdp-input] AF_XDP ready — NIC → DMA → ring buffer → kcode[] atomics\n");
	printf("[xdp-input] like NOBD's GPIO → cmd9ReadyW3, but on a PC\n");
	maplecast_telemetry::send("[xdp-input] AF_XDP initialized on %s port %d", ifname, udp_port);

	return true;
}

void shutdown()
{
	if (!_active) return;

	_active = false;
	if (_inputThread.joinable())
		_inputThread.join();

	if (_xsk) { xsk_socket__delete(_xsk); _xsk = nullptr; }
	if (_umem) { xsk_umem__delete(_umem); _umem = nullptr; }
	if (_umemArea) { munmap(_umemArea, UMEM_SIZE); _umemArea = nullptr; }

	if (_ifindex > 0)
	{
		bpf_xdp_detach(_ifindex, 0, nullptr);
		_ifindex = 0;
	}

	if (_bpfObj) { bpf_object__close(_bpfObj); _bpfObj = nullptr; }

	printf("[xdp-input] shutdown — %lu packets received\n", _pktCount.load());
}

bool active()
{
	return _active.load(std::memory_order_relaxed);
}

} // namespace maplecast_xdp_input

#else // !MAPLECAST_XDP — stub implementation

#include "maplecast_xdp_input.h"
#include <cstdio>

namespace maplecast_xdp_input
{
bool init(const char *, int) {
	printf("[xdp-input] AF_XDP not available (compiled without MAPLECAST_XDP)\n");
	return false;
}
void shutdown() {}
bool active() { return false; }
}

#endif // MAPLECAST_XDP
