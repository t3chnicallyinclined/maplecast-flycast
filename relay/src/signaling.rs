// ============================================================================
// SIGNALING — WebRTC peer tree management
//
// The relay acts as signaling server for the P2P spectator tree.
// Browsers connect via WebSocket, relay assigns roles (seed/relay/leaf)
// and routes SDP offers/answers/ICE candidates between peers.
//
// Tree topology:
//   Relay VPS (WebSocket seed) → 3 seeds
//   Each seed → 3 relays → 3 leaves each = 9 per seed
//   Total capacity per level: 3 → 9 → 27 → 81 → 243...
//
// For now, the relay itself IS the seed — it pushes frames over WebSocket
// to all directly connected clients. P2P tree is Phase 2 optimization
// when we exceed ~50 direct WebSocket clients.
// ============================================================================

use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum SignalMessage {
    // Server → Client
    #[serde(rename = "relay_role")]
    Role {
        #[serde(rename = "peerId")]
        peer_id: String,
        role: String,
    },

    #[serde(rename = "relay_assign_parent")]
    AssignParent {
        #[serde(rename = "parentId")]
        parent_id: String,
    },

    #[serde(rename = "relay_assign_child")]
    AssignChild {
        #[serde(rename = "childId")]
        child_id: String,
    },

    #[serde(rename = "relay_remove_child")]
    RemoveChild {
        #[serde(rename = "childId")]
        child_id: String,
    },

    #[serde(rename = "relay_orphaned")]
    Orphaned,

    // Client → Server or Server → Client (routed)
    #[serde(rename = "relay_signal")]
    Signal {
        #[serde(rename = "fromPeerId", skip_serializing_if = "Option::is_none")]
        from_peer_id: Option<String>,
        #[serde(rename = "toPeerId", skip_serializing_if = "Option::is_none")]
        to_peer_id: Option<String>,
        signal: serde_json::Value,
    },

    // Client → Server
    #[serde(rename = "relay_ready")]
    Ready {
        #[serde(rename = "canRelay")]
        can_relay: bool,
    },

    #[serde(rename = "relay_parent_lost")]
    ParentLost,

    // Status/lobby messages (passthrough)
    #[serde(rename = "status")]
    Status(serde_json::Value),

    #[serde(rename = "join")]
    Join(serde_json::Value),
}

/// Parse a signaling message from JSON text
pub fn parse_signal(text: &str) -> Option<SignalMessage> {
    serde_json::from_str(text).ok()
}
