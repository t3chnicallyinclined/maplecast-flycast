#!/usr/bin/env python3
"""
MapleCast WebSocket proxy.
Bridges raw TCP H.264 stream to WebSocket + handles player registration.

Video: TCP:7200 → WebSocket binary frames
Input: WebSocket binary (4-byte W3) → UDP:7100
Control: WebSocket text (JSON) → player management

Usage: python proxy.py [flycast_host] [tcp_port] [ws_port]
"""
import asyncio
import websockets
import socket
import struct
import json
import sys
import time
import uuid

FLYCAST_HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
TCP_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 7200
WS_PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8080
GAMEPAD_PORT = 7100

# UDP socket for forwarding gamepad input to Flycast
udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# Player registry — maps player_id to slot assignment
players = {}  # {player_id: {"slot": 0|1, "name": str, "ws": websocket, "connected": bool}}
slots = [None, None]  # [p1_id, p2_id]


def assign_slot(player_id, name):
    """Assign a player to the next available slot. Returns slot number or -1."""
    # Check if already assigned
    if player_id in players and players[player_id]["slot"] >= 0:
        slot = players[player_id]["slot"]
        players[player_id]["connected"] = True
        players[player_id]["name"] = name
        print(f"[proxy] {name} ({player_id[:8]}) reconnected as P{slot+1}")
        return slot

    # Find empty slot
    for i in range(2):
        if slots[i] is None:
            slots[i] = player_id
            players[player_id] = {"slot": i, "name": name, "ws": None, "connected": True}
            print(f"[proxy] {name} ({player_id[:8]}) assigned P{i+1}")
            return i

    print(f"[proxy] {name} ({player_id[:8]}) — no slots available")
    return -1


def release_slot(player_id):
    """Release a player's slot."""
    if player_id in players:
        slot = players[player_id]["slot"]
        if slot >= 0 and slots[slot] == player_id:
            # Don't release immediately — allow reconnect for 30 seconds
            players[player_id]["connected"] = False
            print(f"[proxy] P{slot+1} ({player_id[:8]}) disconnected (slot reserved)")


def get_status():
    """Get current lobby status."""
    return {
        "type": "status",
        "p1": {
            "id": slots[0][:8] if slots[0] else None,
            "name": players[slots[0]]["name"] if slots[0] and slots[0] in players else None,
            "connected": players[slots[0]]["connected"] if slots[0] and slots[0] in players else False,
        } if slots[0] else None,
        "p2": {
            "id": slots[1][:8] if slots[1] else None,
            "name": players[slots[1]]["name"] if slots[1] and slots[1] in players else None,
            "connected": players[slots[1]]["connected"] if slots[1] and slots[1] in players else False,
        } if slots[1] else None,
    }


async def broadcast_status():
    """Send lobby status to all connected clients."""
    status = json.dumps(get_status())
    for pid, p in players.items():
        if p["ws"] and p["connected"]:
            try:
                await p["ws"].send(status)
            except Exception:
                pass


async def read_tcp_frames(reader):
    """Read length-prefixed H.264 frames from Flycast TCP stream."""
    while True:
        hdr = await reader.readexactly(4)
        size = struct.unpack("<I", hdr)[0]
        if size > 1_000_000:
            raise ValueError(f"Bad frame size: {size}")
        data = await reader.readexactly(size)
        yield data


async def handle_client(websocket):
    """Handle one WebSocket client."""
    remote = websocket.remote_address
    print(f"[proxy] client connected: {remote}")

    player_id = None
    tcp_reader = None
    tcp_writer = None

    async def send_video():
        """Forward H.264 frames to browser."""
        nonlocal tcp_reader, tcp_writer
        try:
            tcp_reader, tcp_writer = await asyncio.open_connection(FLYCAST_HOST, TCP_PORT)
            print(f"[proxy] video stream connected for {remote}")
            async for frame in read_tcp_frames(tcp_reader):
                await websocket.send(frame)
        except (asyncio.IncompleteReadError, ConnectionError, ConnectionRefusedError) as e:
            print(f"[proxy] video stream error: {e}")

    async def recv_messages():
        """Receive gamepad input + control messages from browser."""
        nonlocal player_id
        try:
            async for msg in websocket:
                if isinstance(msg, bytes):
                    # Binary = W3 gamepad input (4 bytes)
                    if len(msg) >= 4:
                        udp_sock.sendto(msg[:4], (FLYCAST_HOST, GAMEPAD_PORT))
                elif isinstance(msg, str):
                    # Text = JSON control message
                    try:
                        ctrl = json.loads(msg)
                        if ctrl.get("type") == "join":
                            player_id = ctrl.get("id", str(uuid.uuid4()))
                            name = ctrl.get("name", "Player")
                            slot = assign_slot(player_id, name)
                            players[player_id]["ws"] = websocket

                            await websocket.send(json.dumps({
                                "type": "assigned",
                                "slot": slot,
                                "id": player_id[:8],
                                "name": name,
                            }))
                            await broadcast_status()

                    except json.JSONDecodeError:
                        pass
        except websockets.exceptions.ConnectionClosed:
            pass

    try:
        await asyncio.gather(send_video(), recv_messages())
    except Exception as e:
        print(f"[proxy] client error: {e}")
    finally:
        if player_id:
            release_slot(player_id)
            await broadcast_status()
        if tcp_writer:
            tcp_writer.close()
        print(f"[proxy] client disconnected: {remote}")


async def main():
    print(f"========================================")
    print(f"    MapleCast WebSocket Proxy")
    print(f"========================================")
    print(f"  Flycast stream: {FLYCAST_HOST}:{TCP_PORT}")
    print(f"  Gamepad UDP:    {FLYCAST_HOST}:{GAMEPAD_PORT}")
    print(f"  WebSocket:      ws://0.0.0.0:{WS_PORT}")
    print(f"  Open browser:   http://localhost:3000")
    print(f"========================================")

    async with websockets.serve(handle_client, "0.0.0.0", WS_PORT, max_size=1_000_000):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
