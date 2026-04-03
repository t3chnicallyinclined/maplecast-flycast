/*
	MapleCast XDP Input Filter — kernel-side BPF program.

	Attached to NIC via XDP. Matches UDP packets on the gamepad port
	and redirects them to the AF_XDP socket for zero-copy delivery
	to userspace. All other traffic passes through normally.

	Compiled with: clang -O2 -g -target bpf -I/usr/include/x86_64-linux-gnu \
	               -c xdp_input_kern.c -o xdp_input_kern.o
*/

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

/* AF_XDP socket map — userspace registers its XDP socket here */
struct {
	__uint(type, BPF_MAP_TYPE_XSKMAP);
	__uint(max_entries, 4);  /* one per RX queue */
	__type(key, __u32);
	__type(value, __u32);
} xsks_map SEC(".maps");

/* Gamepad UDP port — matches NOBD stick W3 packets */
#define GAMEPAD_PORT 7100

SEC("xdp")
int xdp_gamepad_filter(struct xdp_md *ctx)
{
	void *data = (void *)(long)ctx->data;
	void *data_end = (void *)(long)ctx->data_end;

	/* Ethernet header */
	struct ethhdr *eth = data;
	if ((void *)(eth + 1) > data_end)
		return XDP_PASS;

	/* Only IPv4 */
	if (eth->h_proto != bpf_htons(ETH_P_IP))
		return XDP_PASS;

	/* IP header */
	struct iphdr *ip = (void *)(eth + 1);
	if ((void *)(ip + 1) > data_end)
		return XDP_PASS;

	/* Only UDP */
	if (ip->protocol != IPPROTO_UDP)
		return XDP_PASS;

	/* UDP header */
	struct udphdr *udp = (void *)ip + (ip->ihl * 4);
	if ((void *)(udp + 1) > data_end)
		return XDP_PASS;

	/* Match destination port */
	if (udp->dest != bpf_htons(GAMEPAD_PORT))
		return XDP_PASS;

	/* This is a gamepad packet — redirect to AF_XDP socket */
	return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}

char _license[] SEC("license") = "GPL";
