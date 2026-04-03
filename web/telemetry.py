#!/usr/bin/env python3
"""
MapleCast Telemetry Server.
Aggregates diagnostics from Flycast server + browser clients.
Writes to telemetry.log for analysis.

Flycast sends UDP telemetry to port 7300.
Browser sends via WebSocket on the proxy (forwarded here).
"""
import asyncio
import socket
import json
import time
import sys
import os

LOG_FILE = "telemetry.log"
UDP_PORT = 7300

# Latest state from all sources
state = {
    "server": {},
    "clients": {},
    "updated": 0,
}


def log(msg):
    ts = time.strftime("%H:%M:%S")
    line = f"[{ts}] {msg}"
    print(line)
    with open(LOG_FILE, "a") as f:
        f.write(line + "\n")


async def udp_listener():
    """Receive telemetry from Flycast server over UDP."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", UDP_PORT))
    sock.setblocking(False)

    log(f"Telemetry UDP listening on :{UDP_PORT}")

    loop = asyncio.get_event_loop()
    while True:
        try:
            data = await loop.run_in_executor(None, lambda: sock.recv(4096))
            msg = data.decode("utf-8", errors="ignore").strip()

            # Parse Flycast log lines
            if "[maplecast-stream]" in msg:
                state["server"]["stream"] = msg
                state["updated"] = time.time()
                log(f"SERVER STREAM: {msg}")

            elif "[maplecast]" in msg:
                state["server"]["input"] = msg
                state["updated"] = time.time()
                log(f"SERVER INPUT: {msg}")

            else:
                log(f"SERVER: {msg}")

        except Exception:
            await asyncio.sleep(0.01)


async def print_summary():
    """Print aggregated summary every 5 seconds."""
    while True:
        await asyncio.sleep(5)
        if state["updated"] == 0:
            continue

        log("=" * 60)
        log("TELEMETRY SUMMARY")
        log("-" * 60)

        if "stream" in state["server"]:
            log(f"  Server stream: {state['server']['stream']}")
        if "input" in state["server"]:
            log(f"  Server input:  {state['server']['input']}")

        for cid, cdata in state["clients"].items():
            log(f"  Client {cid}: {json.dumps(cdata)}")

        log("=" * 60)


async def main():
    # Clear log
    with open(LOG_FILE, "w") as f:
        f.write(f"MapleCast Telemetry — started {time.strftime('%Y-%m-%d %H:%M:%S')}\n")

    print(f"╔══════════════════════════════════════╗")
    print(f"║    MapleCast Telemetry Server        ║")
    print(f"╠══════════════════════════════════════╣")
    print(f"║  UDP telemetry:  :{UDP_PORT}               ║")
    print(f"║  Log file:       {LOG_FILE}       ║")
    print(f"╚══════════════════════════════════════╝")

    await asyncio.gather(
        udp_listener(),
        print_summary(),
    )


if __name__ == "__main__":
    asyncio.run(main())
