# SYSTEM-MODEL.md — processes, wires, and the TDW-era topology decision

**Status: CURRENT (2026-07-15). Code-verified inventory (see git blame for the
scan); the decision record for the fan-out tier.**

## 1. The processes (only six, and one of them is almost everything)

| process | job | listens | talks to |
|---|---|---|---|
| **flycast (headless)** | authoritative game sim + ALL hot-path serving — the "input server", "mirror WS", "audio WS", "control WS", "replica-live", "state-sync", "tape" are THREADS in this one process, not separate servers | :7100/udp input (+RTT probe), :7101/udp tape, :7102 state-sync, :7103 lockstep (off), :7200/7210 mirror WS (TDW1/TDWS/SYNC + lobby + browser-input bridge), :7203 audio WS, :7211 control (loopback), :7212 replica-live (loopback) | UDP telemetry out |
| **relay (Rust)** | spectator fan-out multiplexer: ONE upstream WS from flycast → broadcast to N viewers; late-join state cache (SYNC + incremental page cache); browser-input upstream forwarding; WebTransport :443/udp; node registration | :7201 WS, :7202 admin HTTP, :443/udp WebTransport | flycast :7210 (frames), :7211 (admin), :7100/udp (WT input), hub :7220 (register + heartbeat) |
| **hub (Rust)** | COLD-PATH control plane: node registry, /input-servers/nearby, matchmaking queue, replays. Never in the gameplay hot path (by design and by code) | :7220 HTTP (nginx /hub/api) | — |
| **collector (Rust)** | 1 Hz status → SurrealDB rows | — | flycast WS, SurrealDB |
| **nginx** | TLS terminate + path routing (/ws→relay, /play→flycast, /audio, /replica-live, /hub/api, /db) | :443 | the above |
| **SurrealDB** | persistence (players/matches/skins/replays) | :8000 loopback | — |

**A "node" in the distributed network = flycast + relay on one box.** The relay
registers the node with the hub; native clients query the hub for nearby nodes
and UDP-RTT-probe each node's :7100 to pick the lowest-latency one.

## 2. The hot path vs the cold path (players direct — the user's principle, already the code's shape)

```
PLAYER (native client):  UDP input ──────────────► flycast :7100      (direct, no hops)
                         TDW1/TDWS/SYNC ◄────────── flycast :7200      (direct WS)

SPECTATOR (browser/etc): /ws ──► nginx ──► relay :7201 ──► (one upstream) flycast
                         late-join base ◄── relay's OWN cache (not the game server)

CONTROL (cold):          hub :7220 — discovery/matchmaking/replays; RTT probes rank nodes
```

## 3. Redundancy verdicts (TDW era)

| component | verdict |
|---|---|
| **Input server** | NOT redundant — it IS the hot path. Stays in-process, players hit it directly. |
| **Relay** | NOT redundant — **it already IS the "3rd-client fan-out/join server"**: one upstream feed, broadcast fanout, late-join state cache, input forwarding. The planned fan-out tier is the RELAY EVOLVED to speak TDW, not a new process. (TDW1/TDWS already pass through it today as Critical unknown magics — transport works now; the evolution is the join cache.) |
| **Hub** | NOT redundant — control plane only; keep. |
| **replica-live :7212 / state-sync :7102 / tape :7101** | superseded for the render path by TDW (which carries everything); keep for the lockstep/replay arcs; idle in gold mode. |
| **ZCS2 + legacy ZCST legs** | transitional — retire per client class as they adopt TDW (native done; browser next; relay cache last). |
| **NVENC stream + WebRTC** | LEGACY, already compile-excluded from headless prod — declare dead, GOLD-gate the source. |
| **king.html full-mirror path** | retires with the browser TDW decoder. |
| **Port-1 trick / dual topology eras** | consolidate when the relay speaks TDW: hub advertises {input_udp, player_ws (flycast), spectator_ws (relay)} explicitly. |

## 4. The decision: should nodes run viewer fan-out?

**Yes — and they already do; the relay is it.** The TDW-era node:

```
NODE = flycast (game + input, PLAYERS ONLY, ≤2 direct WS + 1 relay upstream)
     + relay   (TDW fan-out: mirrors the dict + last TDWS + stream position;
                serves joiners ENTIRELY from its own state — TDWS snapshot +
                streamStart alignment — the game process never pays for a join)
     + hub registration (relay-side, as today)
```

This satisfies "players direct, minimum connections" exactly: the game server's
socket load is constant regardless of audience; spectator scale is the relay's
problem (and relays can chain relay→relay for big events later — the broadcast
model permits it). The TDW2 persistent-cache handshake (hash-manifest TDWS +
miss-fill) is a RELAY↔viewer protocol, not a game-server concern.

## 5. Build order for the relay evolution
1. Relay TDW awareness: recognize TDW1/TDWS (today they pass through as
   Critical); maintain the dict mirror + last TDWS + zstd stream epoch/seq.
2. Late-join service: on viewer connect, serve cached TDWS + buffer TDW1 from
   the next streamStart (no upstream round-trip, no game-server involvement).
   Retire the relay's ZCST SYNC/page cache for TDW viewers.
3. TDW2 handshake (hash-manifest + client store + miss-fill) — relay↔viewer.
4. Browser TDW decoder (webgpu worker) — connects via /ws exactly as today.
5. Hub advertisement v2: explicit {input_udp, player_ws, spectator_ws} ports.
