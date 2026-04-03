/*
	MapleCast WebRTC — P2P DataChannel transport.

	Architecture:
	  Browser connects via WebSocket (signaling)
	  Browser creates RTCPeerConnection + DataChannels
	  Browser sends SDP offer via WebSocket → server answers
	  ICE candidates exchanged via WebSocket
	  Once DataChannels open → video/input flow P2P (NAT hole-punched)
	  WebSocket stays alive for lobby/status only

	DataChannel config: {ordered: false, maxRetransmits: 0}
	  = UDP semantics. Dropped frame = gone. No retransmit. No head-of-line blocking.
	  SCTP fragments 53KB H.264 frames automatically (max 256KB).

	Threading: libdatachannel callbacks run on internal threads.
	  - dc->send() is thread-safe (called from render thread)
	  - Input callback writes to kcode[] atomics (lock-free)
*/

#ifdef MAPLECAST_WEBRTC

#include "types.h"
#include "maplecast_webrtc.h"
#include "maplecast_input_server.h"

#include <rtc/rtc.hpp>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>
#include <memory>
#include <atomic>

namespace maplecast_webrtc
{

// Per-peer state
struct Peer {
	std::string playerId;
	int slot = -1;
	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<rtc::DataChannel> videoDc;
	std::shared_ptr<rtc::DataChannel> inputDc;
	std::shared_ptr<rtc::DataChannel> audioDc;
	std::atomic<bool> videoReady{false};
	std::atomic<bool> inputReady{false};
	std::atomic<bool> audioReady{false};
};

static std::map<std::string, std::shared_ptr<Peer>> _peers;
static std::mutex _peerMutex;
static SignalCallback _signalCb;
static bool _initialized = false;

// STUN servers for ICE NAT traversal
static rtc::Configuration makeRtcConfig()
{
	rtc::Configuration config;
	config.iceServers.emplace_back("stun:stun.l.google.com:19302");
	config.iceServers.emplace_back("stun:stun1.l.google.com:19302");
	return config;
}

bool init(SignalCallback signalCb)
{
	// Fix locale issue — libdatachannel SDP must use "C" locale for numbers
	// Otherwise "5000" becomes "5,000" and browser rejects the SDP
	setlocale(LC_NUMERIC, "C");

	_signalCb = signalCb;
	_initialized = true;
	printf("[maplecast-rtc] WebRTC DataChannel transport initialized\n");
	printf("[maplecast-rtc] STUN: stun.l.google.com:19302\n");
	printf("[maplecast-rtc] NAT hole punching enabled — no port forwarding needed\n");
	return true;
}

void shutdown()
{
	std::lock_guard<std::mutex> lock(_peerMutex);
	for (auto& [id, peer] : _peers)
	{
		if (peer->pc) peer->pc->close();
	}
	_peers.clear();
	_initialized = false;
	printf("[maplecast-rtc] shutdown\n");
}

void handleOffer(const std::string& playerId, const std::string& sdp, int slot)
{
	if (!_initialized) return;

	auto config = makeRtcConfig();
	auto pc = std::make_shared<rtc::PeerConnection>(config);
	auto peer = std::make_shared<Peer>();
	peer->playerId = playerId;
	peer->slot = slot;
	peer->pc = pc;

	printf("[maplecast-rtc] creating PeerConnection for %s (slot %d)\n",
		playerId.c_str(), slot);

	// Send SDP answer back to browser via WebSocket signaling
	pc->onLocalDescription([playerId](rtc::Description desc) {
		if (_signalCb)
			_signalCb(playerId, "answer", std::string(desc));
	});

	// Send ICE candidates to browser via WebSocket signaling
	pc->onLocalCandidate([playerId](rtc::Candidate candidate) {
		if (_signalCb)
		{
			// Format: candidate|sdpMid
			std::string data = std::string(candidate) + "|" + candidate.mid();
			_signalCb(playerId, "ice-candidate", data);
		}
	});

	// Connection state monitoring
	pc->onStateChange([playerId](rtc::PeerConnection::State state) {
		const char* stateStr = "unknown";
		switch (state) {
			case rtc::PeerConnection::State::New: stateStr = "new"; break;
			case rtc::PeerConnection::State::Connecting: stateStr = "connecting"; break;
			case rtc::PeerConnection::State::Connected: stateStr = "connected"; break;
			case rtc::PeerConnection::State::Disconnected: stateStr = "disconnected"; break;
			case rtc::PeerConnection::State::Failed: stateStr = "failed"; break;
			case rtc::PeerConnection::State::Closed: stateStr = "closed"; break;
		}
		printf("[maplecast-rtc] peer %s: %s\n", playerId.c_str(), stateStr);
	});

	// Handle DataChannels created by browser
	pc->onDataChannel([peer](std::shared_ptr<rtc::DataChannel> dc) {
		std::string label = dc->label();

		if (label == "video")
		{
			peer->videoDc = dc;
			dc->onOpen([peer]() {
				peer->videoReady = true;
				printf("[maplecast-rtc] video DC open for %s\n", peer->playerId.c_str());
			});
			dc->onClosed([peer]() {
				peer->videoReady = false;
				printf("[maplecast-rtc] video DC closed for %s\n", peer->playerId.c_str());
			});
		}
		else if (label == "input")
		{
			peer->inputDc = dc;
			dc->onOpen([peer]() {
				peer->inputReady = true;
				printf("[maplecast-rtc] input DC open for %s (slot %d)\n",
					peer->playerId.c_str(), peer->slot);
			});
			dc->onClosed([peer]() {
				peer->inputReady = false;
			});

			// Input received via DataChannel → inject directly (no UDP hop)
			dc->onMessage([peer](std::variant<rtc::binary, rtc::string> message) {
				if (std::holds_alternative<rtc::binary>(message))
				{
					const auto& data = std::get<rtc::binary>(message);
					if (data.size() >= 4 && peer->slot >= 0)
					{
						const uint8_t* w3 = reinterpret_cast<const uint8_t*>(data.data());
						uint16_t buttons = ((uint16_t)w3[2] << 8) | w3[3];
						maplecast_input::injectInput(peer->slot, w3[0], w3[1], buttons);
					}
				}
			});
		}
		else if (label == "audio")
		{
			peer->audioDc = dc;
			dc->onOpen([peer]() {
				peer->audioReady = true;
				printf("[maplecast-rtc] audio DC open for %s\n", peer->playerId.c_str());
			});
			dc->onClosed([peer]() {
				peer->audioReady = false;
				printf("[maplecast-rtc] audio DC closed for %s\n", peer->playerId.c_str());
			});
		}
		else
		{
			printf("[maplecast-rtc] unknown DC label: %s\n", label.c_str());
		}
	});

	// Set remote description (browser's offer) — triggers auto-answer
	pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Offer));

	{
		std::lock_guard<std::mutex> lock(_peerMutex);
		// Close existing peer if reconnecting
		auto it = _peers.find(playerId);
		if (it != _peers.end() && it->second->pc)
			it->second->pc->close();
		_peers[playerId] = peer;
	}
}

void handleIceCandidate(const std::string& playerId, const std::string& candidate, const std::string& sdpMid)
{
	std::lock_guard<std::mutex> lock(_peerMutex);
	auto it = _peers.find(playerId);
	if (it != _peers.end() && it->second->pc)
	{
		it->second->pc->addRemoteCandidate(rtc::Candidate(candidate, sdpMid));
	}
}

int broadcastFrame(const void* data, size_t size)
{
	int sent = 0;
	std::lock_guard<std::mutex> lock(_peerMutex);
	for (auto& [id, peer] : _peers)
	{
		if (peer->videoReady.load(std::memory_order_relaxed) && peer->videoDc)
		{
			try {
				peer->videoDc->send(
					reinterpret_cast<const std::byte*>(data), size);
				sent++;
			} catch (...) {}
		}
	}
	return sent;
}

int broadcastAudio(const void* data, size_t size)
{
	int sent = 0;
	std::lock_guard<std::mutex> lock(_peerMutex);
	for (auto& [id, peer] : _peers)
	{
		if (peer->audioReady.load(std::memory_order_relaxed) && peer->audioDc)
		{
			try {
				peer->audioDc->send(
					reinterpret_cast<const std::byte*>(data), size);
				sent++;
			} catch (...) {}
		}
	}
	return sent;
}

bool peerHasDataChannel(const std::string& playerId)
{
	std::lock_guard<std::mutex> lock(_peerMutex);
	auto it = _peers.find(playerId);
	return it != _peers.end() && it->second->videoReady.load(std::memory_order_relaxed);
}

void removePeer(const std::string& playerId)
{
	std::lock_guard<std::mutex> lock(_peerMutex);
	auto it = _peers.find(playerId);
	if (it != _peers.end())
	{
		if (it->second->pc) it->second->pc->close();
		printf("[maplecast-rtc] removed peer %s\n", playerId.c_str());
		_peers.erase(it);
	}
}

int activePeerCount()
{
	int count = 0;
	std::lock_guard<std::mutex> lock(_peerMutex);
	for (auto& [id, peer] : _peers)
		if (peer->videoReady.load(std::memory_order_relaxed))
			count++;
	return count;
}

bool available() { return true; }

} // namespace maplecast_webrtc

#else // !MAPLECAST_WEBRTC

#include "maplecast_webrtc.h"
#include <cstdio>

namespace maplecast_webrtc
{
bool init(SignalCallback) { return false; }
void shutdown() {}
void handleOffer(const std::string&, const std::string&, int) {}
void handleIceCandidate(const std::string&, const std::string&, const std::string&) {}
int broadcastFrame(const void*, size_t) { return 0; }
int broadcastAudio(const void*, size_t) { return 0; }
bool peerHasDataChannel(const std::string&) { return false; }
void removePeer(const std::string&) {}
int activePeerCount() { return 0; }
bool available() { return false; }
}

#endif // MAPLECAST_WEBRTC
