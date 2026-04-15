// ============================================================================
// HUB_DISCOVERY — Implementation
// ============================================================================

#include "hub_discovery.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "json/json.hpp"

using json = nlohmann::json;

namespace maplecast_hub {

// ── HTTP fetch via libcurl ──────────────────────────────────────────────

static size_t curlWriteCallback(void* contents, size_t size, size_t nmemb, std::string* out) {
	out->append((char*)contents, size * nmemb);
	return size * nmemb;
}

static std::string httpGet(const std::string& url, long timeout_sec = 5) {
	CURL* curl = curl_easy_init();
	if (!curl) return "";

	std::string body;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "maplecast-native-client/1.0");

	CURLcode rc = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_easy_cleanup(curl);

	if (rc != CURLE_OK) {
		printf("[hub-discovery] HTTP GET %s failed: %s\n", url.c_str(), curl_easy_strerror(rc));
		return "";
	}
	if (http_code < 200 || http_code >= 300) {
		printf("[hub-discovery] HTTP GET %s returned %ld\n", url.c_str(), http_code);
		return "";
	}
	return body;
}

// ── discover() ──────────────────────────────────────────────────────────

std::vector<InputServer> discover(const std::string& hub_url, int limit) {
	std::vector<InputServer> result;

	// Try the canonical /input-servers/nearby first; fall back to /nodes/nearby
	// for backward compatibility with older hub deployments.
	std::string url = hub_url + "/input-servers/nearby?limit=" + std::to_string(limit);
	std::string body = httpGet(url);
	if (body.empty()) {
		url = hub_url + "/nodes/nearby?limit=" + std::to_string(limit);
		body = httpGet(url);
	}
	if (body.empty()) {
		printf("[hub-discovery] Could not reach hub at %s\n", hub_url.c_str());
		return result;
	}

	json doc;
	try {
		doc = json::parse(body);
	} catch (const std::exception& e) {
		printf("[hub-discovery] JSON parse failed: %s\n", e.what());
		return result;
	}

	if (!doc.contains("nodes") || !doc["nodes"].is_array()) {
		printf("[hub-discovery] Hub response missing 'nodes' array — body: %s\n",
		       body.substr(0, 200).c_str());
		return result;
	}

	size_t total = doc["nodes"].size();
	size_t skipped_no_host = 0;
	size_t skipped_not_ready = 0;

	for (const auto& n : doc["nodes"]) {
		InputServer s;
		s.node_id     = n.value("node_id", "");
		s.name        = n.value("name", "unnamed");
		s.public_host = n.value("public_host", "");
		s.region      = n.value("region", "");
		s.status      = n.value("status", "ready");
		s.relay_url   = n.value("relay_url", "");

		if (n.contains("ports") && n["ports"].is_object()) {
			s.input_udp_port  = n["ports"].value("input_udp", 7100);
			s.relay_ws_port   = n["ports"].value("relay_ws", 7201);
			s.control_ws_port = n["ports"].value("control_ws", 7210);
		} else {
			s.input_udp_port  = 7100;
			s.relay_ws_port   = 7201;
			s.control_ws_port = 7210;
		}

		// Skip nodes without a public_host (can't connect to them anyway)
		if (s.public_host.empty()) {
			skipped_no_host++;
			continue;
		}

		// Only consider ready servers
		if (s.status != "ready") {
			skipped_not_ready++;
			printf("[hub-discovery]   skipping %s: status='%s' (want 'ready')\n",
			       s.name.c_str(), s.status.c_str());
			continue;
		}

		result.push_back(std::move(s));
	}

	if (total > 0 && result.empty()) {
		printf("[hub-discovery]   filter dumped all %zu: %zu no public_host, %zu not ready\n",
		       total, skipped_no_host, skipped_not_ready);
	}

	printf("[hub-discovery] Discovered %zu input server(s) from %s\n",
	       result.size(), hub_url.c_str());
	return result;
}

// ── probeServer() ───────────────────────────────────────────────────────

void probeServer(InputServer& server, int probe_count, int interval_ms) {
	server.avg_rtt_ms = INFINITY;
	server.p95_rtt_ms = INFINITY;
	server.samples = 0;

	// Resolve hostname
	struct addrinfo hints = {};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
	struct addrinfo* res = nullptr;
	std::string port_str = std::to_string(server.input_udp_port);
	if (getaddrinfo(server.public_host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
		printf("[hub-discovery] probe %s: DNS failed\n", server.name.c_str());
		return;
	}
	struct sockaddr_in addr = *(struct sockaddr_in*)res->ai_addr;
	freeaddrinfo(res);

	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) return;

	// 50ms recv timeout per probe — server should respond in <100ms even
	// across continents
	struct timeval tv = { .tv_sec = 0, .tv_usec = 50 * 1000 };
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	std::vector<double> samples;
	samples.reserve(probe_count);

	for (int i = 0; i < probe_count; i++) {
		uint8_t pkt[7] = { 0xFF, (uint8_t)i, 0, 0, 0, 0, 0 };
		auto t0 = std::chrono::steady_clock::now();
		int sent = sendto(sock, pkt, sizeof(pkt), 0,
		                  (struct sockaddr*)&addr, sizeof(addr));
		if (sent < 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
			continue;
		}

		uint8_t reply[16];
		struct sockaddr_in from;
		socklen_t fromLen = sizeof(from);
		int n = recvfrom(sock, reply, sizeof(reply), 0,
		                 (struct sockaddr*)&from, &fromLen);
		auto t1 = std::chrono::steady_clock::now();

		if (n >= 8 && reply[0] == 0xFE && reply[1] == (uint8_t)i) {
			double rtt_ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
			samples.push_back(rtt_ms);
		}

		// Spacing between probes (skip after last)
		if (i + 1 < probe_count) {
			std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
		}
	}

	close(sock);

	// Discard the first sample (cold path), average the rest
	if (samples.size() < 2) {
		// Not enough data — leave avg_rtt_ms as INFINITY
		printf("[hub-discovery] probe %s: only %zu sample(s), unreachable\n",
		       server.name.c_str(), samples.size());
		return;
	}

	std::vector<double> warm(samples.begin() + 1, samples.end());
	double sum = 0.0;
	for (double r : warm) sum += r;
	server.avg_rtt_ms = sum / warm.size();
	server.samples = (uint32_t)warm.size();

	// p95 = highest sample (we have so few we approximate)
	double p95 = 0.0;
	for (double r : warm) if (r > p95) p95 = r;
	server.p95_rtt_ms = p95;

	printf("[hub-discovery] probe %s: %.1fms avg (%u samples)\n",
	       server.name.c_str(), server.avg_rtt_ms, server.samples);
}

// ── probeServers() — parallel probe ─────────────────────────────────────

std::vector<InputServer> probeServers(std::vector<InputServer> servers) {
	if (servers.empty()) return servers;

	// Probe in parallel — each on its own thread (servers are typically
	// 1-5, so spawning a thread each is cheap and we want all to finish
	// at roughly the same time)
	std::vector<std::thread> threads;
	threads.reserve(servers.size());
	for (auto& s : servers) {
		threads.emplace_back([&s]() { probeServer(s); });
	}
	for (auto& t : threads) t.join();

	// Sort by avg_rtt_ms ascending (best first)
	std::sort(servers.begin(), servers.end(), [](const InputServer& a, const InputServer& b) {
		return a.avg_rtt_ms < b.avg_rtt_ms;
	});

	return servers;
}

// ── discoverAndSelect() — convenience ───────────────────────────────────

InputServer discoverAndSelect(const std::string& hub_url) {
	auto servers = discover(hub_url, 5);
	if (servers.empty()) {
		printf("[hub-discovery] No input servers available\n");
		return {};
	}

	servers = probeServers(std::move(servers));

	if (!std::isfinite(servers[0].avg_rtt_ms)) {
		printf("[hub-discovery] No reachable input servers — all probes failed\n");
		return {};
	}

	const auto& winner = servers[0];
	printf("[hub-discovery] ═══ Selected: %s (%s) — %.1fms RTT ═══\n",
	       winner.name.c_str(), winner.public_host.c_str(), winner.avg_rtt_ms);

	// Show runner-ups for transparency
	for (size_t i = 1; i < servers.size() && i < 5; i++) {
		if (std::isfinite(servers[i].avg_rtt_ms)) {
			printf("[hub-discovery]    runner-up #%zu: %s — %.1fms\n",
			       i, servers[i].name.c_str(), servers[i].avg_rtt_ms);
		}
	}

	return winner;
}

std::vector<InputServer> discoverAndRank(const std::string& hub_url, int top_n) {
	auto servers = discover(hub_url, 5);
	if (servers.empty()) return {};

	servers = probeServers(std::move(servers));

	std::vector<InputServer> result;
	for (const auto& s : servers) {
		if (!std::isfinite(s.avg_rtt_ms)) continue;
		result.push_back(s);
		if ((int)result.size() >= top_n) break;
	}
	return result;
}

} // namespace maplecast_hub
