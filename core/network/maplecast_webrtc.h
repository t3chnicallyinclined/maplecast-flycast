/*
	MapleCast WebRTC — P2P DataChannel transport.

	Replaces WebSocket for video/input data transport.
	WebSocket remains for signaling only (SDP, ICE, join/lobby).

	Two DataChannels per peer:
	  "video" (server→client): H.264 frames, unreliable, unordered
	  "input" (client→server): W3 gamepad, unreliable, unordered

	NAT hole punching via ICE/STUN — no port forwarding needed.
*/
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <functional>

namespace maplecast_webrtc
{

// Callback types for signaling (server sends these back via WebSocket)
using SignalCallback = std::function<void(const std::string& playerId, const std::string& type, const std::string& data)>;

// Initialize WebRTC subsystem
// signalCb is called when server needs to send SDP/ICE to browser via WebSocket
bool init(SignalCallback signalCb);

// Shutdown all peer connections
void shutdown();

// Handle incoming SDP offer from browser (via WebSocket signaling)
void handleOffer(const std::string& playerId, const std::string& sdp, int slot);

// Handle incoming ICE candidate from browser (via WebSocket signaling)
void handleIceCandidate(const std::string& playerId, const std::string& candidate, const std::string& sdpMid);

// Send H.264 frame to all peers with active video DataChannel
// Returns number of peers sent to via DataChannel (rest fall back to WebSocket)
int broadcastFrame(const void* data, size_t size);

// Check if a specific peer has an active DataChannel (for WebSocket fallback decision)
bool peerHasDataChannel(const std::string& playerId);

// Remove peer (on WebSocket disconnect)
void removePeer(const std::string& playerId);

// Get count of active DataChannel peers
int activePeerCount();

// Is WebRTC available? (compiled with MAPLECAST_WEBRTC)
bool available();

}
