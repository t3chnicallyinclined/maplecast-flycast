// ============================================================================
// HUB_DISCOVERY — Native client auto-discovery of input servers via the hub
//
// On native client startup, if MAPLECAST_HUB_URL is set, fetch the list of
// nearby input servers from the hub, UDP-probe each one to measure REAL
// input-path RTT, and pick the lowest-latency winner.
//
// The hub is NEVER in the gameplay hot path — it's queried once at startup
// (or on manual rediscovery), then we connect directly to the chosen server.
// ============================================================================

#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace maplecast_hub {

struct InputServer {
	std::string node_id;       // UUID
	std::string name;          // human-readable
	std::string public_host;   // hostname or IP
	uint16_t input_udp_port;   // typically 7100
	uint16_t relay_ws_port;    // typically 7201
	uint16_t control_ws_port;  // typically 7210 (or same as relay)
	std::string relay_url;     // operator-supplied wss:// override if any
	std::string control_url;
	std::string audio_url;
	std::string region;
	std::string status;        // "ready" | "in_match" | etc.

	// Filled by probeServers()
	double avg_rtt_ms = -1.0;  // -1 = not probed yet, INF = unreachable
	double p95_rtt_ms = -1.0;
	uint32_t samples = 0;
};

// Fetch the list of input servers from the hub.
// Returns empty vec on failure (logs warning, doesn't throw).
//   hub_url:  e.g. "https://nobd.net/hub/api"
//   limit:    geographic pre-filter (5 = nearest 5 by GeoIP)
std::vector<InputServer> discover(const std::string& hub_url, int limit = 5);

// UDP-probe a single server to measure input-path RTT.
// Sends 5 probe packets at 200ms intervals, discards the first (TCP cold),
// averages the remaining 4. Returns avg_rtt_ms in the InputServer struct.
// Timeout: 3 seconds total. Sets avg_rtt_ms = INFINITY on failure.
void probeServer(InputServer& server, int probe_count = 5, int interval_ms = 200);

// Probe all servers (in parallel), then return them sorted by avg_rtt_ms.
// Servers that fail to respond have avg_rtt_ms = INFINITY and sort to the end.
std::vector<InputServer> probeServers(std::vector<InputServer> servers);

// Convenience: full discover + probe + select best in one call.
// Returns the lowest-RTT server, or empty InputServer (empty node_id) if no
// reachable server was found.
InputServer discoverAndSelect(const std::string& hub_url);

// Like discoverAndSelect but returns up to N best (sorted by avg_rtt_ms).
// out[0] is primary, out[1] is hot-standby, etc. Empty result if no reachable.
std::vector<InputServer> discoverAndRank(const std::string& hub_url, int top_n = 2);

} // namespace maplecast_hub
