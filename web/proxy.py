#!/usr/bin/env python3
"""
MapleCast WebSocket proxy.
Bridges raw TCP H.264 stream (port 7200) to WebSocket (port 8080).
Also receives gamepad W3 packets from browser and forwards to Flycast UDP.

Usage: python proxy.py [flycast_host] [tcp_port] [ws_port]
"""
import asyncio
import websockets
import socket
import struct
import sys

FLYCAST_HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
TCP_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 7200
WS_PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8080

# UDP socket for forwarding gamepad input to Flycast
udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)


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
    """Handle one WebSocket client — stream video out, gamepad in."""
    path = websocket.request.path if hasattr(websocket, 'request') else ""
    print(f"[proxy] client connected: {websocket.remote_address} path={path}")

    # All players send to the same port — server auto-assigns P1/P2
    player_port = 7100
    print(f"[proxy] gamepad input → UDP:{player_port} (auto-assign P1/P2)")

    # Connect to Flycast H.264 TCP stream
    try:
        reader, writer = await asyncio.open_connection(FLYCAST_HOST, TCP_PORT)
    except ConnectionRefusedError:
        print("[proxy] can't connect to Flycast stream")
        await websocket.close()
        return

    print("[proxy] connected to Flycast H.264 stream")

    async def send_video():
        """Forward H.264 frames to browser."""
        try:
            async for frame in read_tcp_frames(reader):
                await websocket.send(frame)
        except (asyncio.IncompleteReadError, ConnectionError):
            pass

    async def recv_gamepad():
        """Receive gamepad W3 from browser, forward to Flycast UDP."""
        try:
            async for msg in websocket:
                if isinstance(msg, bytes) and len(msg) >= 4:
                    # W3 format: 4 bytes {LT, RT, buttons_hi, buttons_lo}
                    udp_sock.sendto(msg[:4], (FLYCAST_HOST, player_port))
        except websockets.exceptions.ConnectionClosed:
            pass

    # Run video send and gamepad receive concurrently
    try:
        await asyncio.gather(send_video(), recv_gamepad())
    except Exception as e:
        print(f"[proxy] client disconnected: {e}")
    finally:
        writer.close()
        print("[proxy] client session ended")


async def main():
    print(f"╔══════════════════════════════════════╗")
    print(f"║     MapleCast WebSocket Proxy        ║")
    print(f"╠══════════════════════════════════════╣")
    print(f"║  Flycast stream: {FLYCAST_HOST}:{TCP_PORT}         ║")
    print(f"║  WebSocket:      ws://0.0.0.0:{WS_PORT}    ║")
    print(f"║  Gamepad P1:     connect to /p1      ║")
    print(f"║  Gamepad P2:     connect to /p2      ║")
    print(f"╚══════════════════════════════════════╝")

    async with websockets.serve(handle_client, "0.0.0.0", WS_PORT, max_size=1_000_000):
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    asyncio.run(main())
