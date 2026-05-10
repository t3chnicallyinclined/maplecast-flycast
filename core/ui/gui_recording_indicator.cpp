// gui_recording_indicator.cpp -- see gui_recording_indicator.h.
//
// Mirror-client F9 hotkey: opens a one-shot raw-socket WS connection to
// whatever server the mirror client is connected to (MAPLECAST_SERVER_HOST/
// PORT, default 127.0.0.1:7200) and sends a record_start / record_stop
// JSON command. Server-side handler in maplecast_ws_server.cpp owns the
// SH4-stopping + dc_savestate + .mcrec write; the client just drives it
// from the keyboard.
//
// Why raw sockets and not websocketpp::client? The native client + audio
// client both use raw-socket WS framing (see wsHandshake in
// maplecast_audio_client.cpp). Trying websocketpp::client on Windows
// triggered a "specified class was not found" HRESULT inside asio's
// init, while the existing raw-socket path works fine. So we follow the
// established pattern. ~80 lines, no extra deps.

#include "gui_recording_indicator.h"

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #pragma comment(lib, "Ws2_32.lib")
  using socket_t = SOCKET;
  #define mc_send(s,b,l,f)  ::send((s),(const char*)(b),(int)(l),(f))
  #define mc_recv(s,b,l,f)  ::recv((s),(char*)(b),(int)(l),(f))
  #define mc_close(s)       ::closesocket(s)
  static const socket_t INVALID_SOCK = INVALID_SOCKET;
#else
  #include <sys/socket.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <errno.h>
  using socket_t = int;
  #define mc_send(s,b,l,f)  ::send((s),(b),(l),(f))
  #define mc_recv(s,b,l,f)  ::recv((s),(b),(l),(f))
  #define mc_close(s)       ::close(s)
  static const socket_t INVALID_SOCK = -1;
#endif

namespace {

std::atomic<bool>                       _recording{false};
std::chrono::steady_clock::time_point   _recordStart;
std::atomic<bool>                       _inFlight{false};

// ── Path generation ────────────────────────────────────────────────────
std::string generateRecordPath()
{
	std::string base;
#ifdef _WIN32
	if (const char* up = std::getenv("USERPROFILE"))
		base = std::string(up) + "\\Documents\\MapleCastReplays";
	else
		base = ".";
	CreateDirectoryA(base.c_str(), nullptr);
#else
	if (const char* home = std::getenv("HOME"))
		base = std::string(home) + "/Documents/MapleCastReplays";
	else
		base = ".";
	mkdir(base.c_str(), 0755);
#endif

	std::time_t t = std::time(nullptr);
	std::tm tm;
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	char ts[32];
	std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);

#ifdef _WIN32
	return base + "\\mvc2-" + ts + ".mcrec";
#else
	return base + "/mvc2-" + std::string(ts) + ".mcrec";
#endif
}

// ── Resolve target endpoint ────────────────────────────────────────────
struct Endpoint { std::string host; int port; };

Endpoint mirrorEndpoint()
{
	Endpoint e;
	e.host = "127.0.0.1";
	e.port = 7200;
	if (const char* h = std::getenv("MAPLECAST_SERVER_HOST"))
		if (h[0]) e.host = h;
	if (const char* p = std::getenv("MAPLECAST_SERVER_PORT"))
		if (p[0]) e.port = std::atoi(p);
	return e;
}

// ── Raw-socket WS plumbing ─────────────────────────────────────────────
// HTTP/1.1 upgrade with a static Sec-WebSocket-Key (matches the pattern
// used by maplecast_audio_client.cpp and maplecast_mirror.cpp's mirror
// client -- the server doesn't validate the key beyond the protocol
// requirement that we send one).
static bool wsHandshake(socket_t fd, const std::string& host, int port)
{
	char req[512];
	int len = std::snprintf(req, sizeof(req),
		"GET / HTTP/1.1\r\n"
		"Host: %s:%d\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n"
		"\r\n", host.c_str(), port);
	if (mc_send(fd, req, len, 0) != len) return false;

	char resp[1024];
	int total = 0;
	while (total < (int)sizeof(resp) - 1) {
		int n = mc_recv(fd, resp + total, 1, 0);
		if (n <= 0) return false;
		total += n;
		if (total >= 4 && std::memcmp(resp + total - 4, "\r\n\r\n", 4) == 0) break;
	}
	resp[total] = 0;
	return std::strstr(resp, "101") != nullptr;
}

// Send one masked text frame. Client-to-server frames MUST be masked
// per RFC 6455.
static bool wsSendText(socket_t fd, const std::string& payload)
{
	std::vector<uint8_t> frame;
	frame.reserve(payload.size() + 14);
	frame.push_back(0x81); // FIN + text opcode

	uint8_t mask[4];
	std::random_device rd;
	for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(rd() & 0xFF);

	const size_t n = payload.size();
	if (n < 126) {
		frame.push_back(0x80 | (uint8_t)n);
	} else if (n <= 0xFFFF) {
		frame.push_back(0x80 | 126);
		frame.push_back((uint8_t)((n >> 8) & 0xFF));
		frame.push_back((uint8_t)(n & 0xFF));
	} else {
		frame.push_back(0x80 | 127);
		for (int i = 7; i >= 0; --i)
			frame.push_back((uint8_t)((n >> (i * 8)) & 0xFF));
	}
	for (int i = 0; i < 4; i++) frame.push_back(mask[i]);
	for (size_t i = 0; i < n; i++) frame.push_back((uint8_t)payload[i] ^ mask[i % 4]);

	int sent = mc_send(fd, (const char*)frame.data(), (int)frame.size(), 0);
	return sent == (int)frame.size();
}

// Read one frame. Server-to-client frames are NOT masked.
static bool wsReadFrame(socket_t fd, std::vector<uint8_t>& out)
{
	uint8_t hdr[2];
	if (mc_recv(fd, hdr, 2, 0) != 2) return false;
	int opcode = hdr[0] & 0x0F;
	(void)opcode;
	uint64_t len = hdr[1] & 0x7F;
	if (len == 126) {
		uint8_t ext[2];
		if (mc_recv(fd, ext, 2, 0) != 2) return false;
		len = ((uint64_t)ext[0] << 8) | ext[1];
	} else if (len == 127) {
		uint8_t ext[8];
		if (mc_recv(fd, ext, 8, 0) != 8) return false;
		len = 0;
		for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
	}
	out.resize((size_t)len);
	size_t got = 0;
	while (got < len) {
		int n = mc_recv(fd, out.data() + got, (int)(len - got), 0);
		if (n <= 0) return false;
		got += (size_t)n;
	}
	return true;
}

// Set a recv timeout so a hung server doesn't wedge the worker thread.
static void setRecvTimeout(socket_t fd, int ms)
{
#ifdef _WIN32
	DWORD tv = (DWORD)ms;
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
	struct timeval tv;
	tv.tv_sec  = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// ── Async WS request dispatch ──────────────────────────────────────────
// Runs on a detached thread so the SDL/UI thread never blocks.
void sendCommandAsync(const std::string& payload,
                      std::function<void(const std::string&)> on_reply)
{
	const Endpoint ep = mirrorEndpoint();
	printf("[recording] WS dial -> ws://%s:%d\n", ep.host.c_str(), ep.port);
	fflush(stdout);

	std::thread([payload, on_reply, ep]() {
		std::string reply;

#ifdef _WIN32
		// WSAStartup is process-wide and the rest of flycast already calls
		// it on init -- this is a no-op safety net for cold launches.
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

		// Resolve.
		struct addrinfo hints, *res = nullptr;
		std::memset(&hints, 0, sizeof(hints));
		hints.ai_family   = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		std::string portStr = std::to_string(ep.port);
		if (getaddrinfo(ep.host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
			printf("[recording] WS getaddrinfo(%s) failed\n", ep.host.c_str());
			fflush(stdout);
			on_reply("");
			return;
		}

		socket_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (fd == INVALID_SOCK) {
			printf("[recording] WS socket() failed\n"); fflush(stdout);
			freeaddrinfo(res);
			on_reply("");
			return;
		}

		if (connect(fd, res->ai_addr, (int)res->ai_addrlen) != 0) {
			printf("[recording] WS connect() failed\n"); fflush(stdout);
			mc_close(fd);
			freeaddrinfo(res);
			on_reply("");
			return;
		}
		freeaddrinfo(res);

		int one = 1;
		::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
		setRecvTimeout(fd, 5000);

		if (!wsHandshake(fd, ep.host, ep.port)) {
			printf("[recording] WS handshake failed\n"); fflush(stdout);
			mc_close(fd);
			on_reply("");
			return;
		}

		if (!wsSendText(fd, payload)) {
			printf("[recording] WS send failed\n"); fflush(stdout);
			mc_close(fd);
			on_reply("");
			return;
		}

		// Wait for the server's reply. mirror-WS broadcasts TA frames
		// (binary) AND periodic status JSON ("type":"status") to every
		// connected client, so we have to filter for our own reply.
		// Server echoes the reply_id field we sent, so match on that.
		// Extract it from the outgoing payload once.
		std::string ridNeedle;
		{
			const std::string key = "\"reply_id\":\"";
			auto pos = payload.find(key);
			if (pos != std::string::npos) {
				auto end = payload.find('"', pos + key.size());
				if (end != std::string::npos)
					ridNeedle = payload.substr(pos, end + 1 - pos);
			}
		}
		for (int i = 0; i < 200; i++) {
			std::vector<uint8_t> frame;
			if (!wsReadFrame(fd, frame)) {
				printf("[recording] WS read failed (no matching reply within 200 frames)\n");
				fflush(stdout);
				break;
			}
			if (frame.empty() || frame[0] != '{') continue;
			std::string body((const char*)frame.data(), frame.size());
			if (!ridNeedle.empty() && body.find(ridNeedle) == std::string::npos)
				continue; // status broadcast or unrelated reply -- keep waiting
			reply = std::move(body);
			break;
		}

		mc_close(fd);
		on_reply(reply);
	}).detach();
}

} // namespace

namespace gui_recording_indicator {

void toggle()
{
	if (_inFlight.exchange(true)) {
		printf("[recording] F9 ignored: previous request still in flight\n");
		fflush(stdout);
		return;
	}

	if (_recording.load()) {
		printf("[recording] F9 -> record_stop\n"); fflush(stdout);
		const std::string payload = R"({"cmd":"record_stop","reply_id":"f9-stop"})";
		sendCommandAsync(payload, [](const std::string& reply) {
			if (!reply.empty() && reply.find("\"ok\":true") != std::string::npos) {
				_recording.store(false);
				printf("[recording] stopped: %s\n", reply.c_str());
			} else {
				printf("[recording] record_stop failed: %s\n",
				       reply.empty() ? "no reply" : reply.c_str());
			}
			fflush(stdout);
			_inFlight.store(false);
		});
		return;
	}

	const std::string path = generateRecordPath();
	printf("[recording] F9 -> record_start: %s\n", path.c_str());
	fflush(stdout);

	std::string jsonPath;
	jsonPath.reserve(path.size() + 16);
	for (char c : path) {
		if (c == '\\') jsonPath += "\\\\";
		else if (c == '"') jsonPath += "\\\"";
		else jsonPath += c;
	}
	const std::string payload =
		std::string("{\"cmd\":\"record_start\",\"path\":\"") + jsonPath +
		"\",\"reply_id\":\"f9-start\"}";

	sendCommandAsync(payload, [](const std::string& reply) {
		if (!reply.empty() && reply.find("\"ok\":true") != std::string::npos) {
			_recordStart = std::chrono::steady_clock::now();
			_recording.store(true);
			printf("[recording] started: %s\n", reply.c_str());
		} else {
			printf("[recording] record_start failed: %s\n",
			       reply.empty() ? "no reply" : reply.c_str());
		}
		fflush(stdout);
		_inFlight.store(false);
	});
}

void draw()
{
	if (!_recording.load()) return;

	const auto elapsed = std::chrono::steady_clock::now() - _recordStart;
	const int secs = (int)std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
	const int mm = secs / 60;
	const int ss = secs % 60;

	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 16, vp->WorkPos.y + 16));
	ImGui::SetNextWindowBgAlpha(0.55f);

	const ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoDecoration
		| ImGuiWindowFlags_AlwaysAutoResize
		| ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoFocusOnAppearing
		| ImGuiWindowFlags_NoNav
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoInputs;

	if (ImGui::Begin("##recording_indicator", nullptr, flags)) {
		const bool blinkOn = (secs % 2) == 0;

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 cp = ImGui::GetCursorScreenPos();
		const float lineH = ImGui::GetTextLineHeight();
		const float radius = lineH * 0.4f;
		const ImVec2 dotCenter(cp.x + radius, cp.y + lineH * 0.55f);
		const ImU32 red = IM_COL32(220, 40, 40, 255);
		if (blinkOn)
			dl->AddCircleFilled(dotCenter, radius, red, 16);
		else
			dl->AddCircle(dotCenter, radius, red, 16, 1.5f);

		ImGui::Dummy(ImVec2(radius * 2.0f + 4.0f, lineH));
		ImGui::SameLine();
		ImGui::Text("REC %02d:%02d", mm, ss);
	}
	ImGui::End();
}

} // namespace gui_recording_indicator
