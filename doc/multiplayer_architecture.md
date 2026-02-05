# Multiplayer (User Guide + Architecture)

This document covers the full multiplayer feature set. It is split into:

- **User Guide**: how to use LAN/Internet multiplayer.
- **Architecture**: how it works internally and how we avoid desync.

It is not a changelog.

## User Guide

### Opening multiplayer

From the main menu, choose **Network Game** to open the multiplayer lobby.

### Internet server configuration (once)

Open **Options -> Multiplayer** and set:

- **Lobby/Relay Server**: hostname or IP of the shared server (default: `CHANGEME`)
- Ports:
  - Lobby (UDP, default 12347)
  - UDP relay (UDP, default 12348)
  - TCP relay (TCP, default 12349)
- Connection strategies (toggles):
  - Direct connect
  - UDP relay
  - TCP relay

If multiple strategies are enabled, clients try them in this order:

`Direct` -> `UDP relay` -> `TCP relay` (skipping disabled strategies).

### LAN mode (local network)

LAN mode uses UDP broadcast discovery.

1) Start Enigma on two (or more) machines in the same LAN (or multiple instances locally).
2) Open **Network Game** on all machines.
3) The other players should appear in the lobby.
4) Pick a **Level Pack**.
5) Use **Maps for N** to filter maps by intended player count.
6) The host starts a game by clicking a level icon.

### Internet mode (room code)

Internet mode uses a lobby server for room membership and "start" coordination, then
uses direct connect or relays for the actual game session.

1) Ensure the server host + ports are configured (Options -> Multiplayer).
2) Switch the lobby to **Internet mode**.
3) Host enters a **Room code** and clicks **Create Room**.
4) Other players enter the same room code and click **Join Room**.
5) Once players joined, only the host selects a level (clients show "Waiting for host...").
6) Host starts a game by clicking a level icon.

If UDP is blocked (guest WiFi, corporate networks, VPNs), enable **TCP relay** as fallback.

### Debugging toggles

Set environment variables before launching Enigma:

- `ENIGMA_MP_DEBUG=1` enables verbose multiplayer logs.
- `ENIGMA_MP_FORCE_RELAY=1` forces relay use (skips direct connect), useful for testing.

## Architecture

### High-level model: deterministic lockstep

Multiplayer is deterministic lockstep:

- Each instance runs the full simulation locally.
- Inputs are exchanged per fixed tick.
- The host distributes inputs but does not stream full authoritative state.
- Periodic sync snapshots detect drift and can correct some divergence.

This keeps latency low while maintaining a consistent world state.

### Lobby discovery and start

LAN discovery:

- Each peer periodically broadcasts `LobbyAnnounce` over UDP broadcast.
- The host starts by broadcasting `LobbyStart`.

Internet discovery (room codes):

- Server: `tools/internet_lobby_server.py` (UDP, default 12347)
- Room membership uses Create/Join/Leave/Poll requests.
- Rooms are removed when the last member has timed out.

`LobbyStart` carries session metadata:

- `session_id`
- `seed` (for deterministic RNG)
- `expected_players`
- `host_port`
- `host_id`
- map filter settings (minimum intended player count)

### Session transport options

All game-session payloads use a single message protocol (`src/multiplayer_protocol.hh`).
How those payloads are transported depends on connectivity:

1) **Direct**: client connects to host via ENet (`host_ip:host_port`).
2) **UDP relay**: client connects to relay via ENet; relay forwards ENet packets between client and host.
3) **TCP relay**: client connects to relay via raw TCP; relay forwards framed payloads between client and host.

#### ENet versioning (vendored vs system)

Enigma historically shipped a vendored ENet (`lib-src/enet`, libenet 1.0). Most modern distro builds
(Ubuntu/Debian, Homebrew) use system ENet (ENet 1.3.x).

ENet API signatures differ between these versions (e.g. `enet_host_create`, `enet_host_connect`,
`enet_socket_create`), so multiplayer uses `src/enet_ver.hh` and small shims to compile against both.

Important: the **UDP relay uses ENet itself**, so the relay must be built against an ENet version
compatible with the clients. In practice, that means using ENet 1.3.x for deployed Internet relays.

### Keeping simulations in sync

#### Input lockstep and delay

- Inputs are queued per tick for all players.
- `input::CanAdvanceTick()` enforces that a tick only advances once all inputs are present.
- A fixed input delay (`kInputDelay`) sends inputs slightly ahead to absorb jitter.

#### Input epoch

Each start/restart increments an `input_epoch`:

- `NET_START`, `NET_INPUT`, `NET_SYNC` carry the epoch.
- Packets from a previous epoch are ignored.

This prevents late packets from a prior level/restart from corrupting the new simulation.

#### Drift detection: RNG + checksums

Periodic `NET_SYNC` snapshots include:

- tick
- RNG state
- checksums over relevant world/actor state

Clients compare these snapshots to local state:

- If only RNG diverged, RNG can be corrected without restarting.
- If world/actor checksums diverge, a resync is attempted.

#### Resync ("soft resync")

When divergence is detected:

- Client requests a resync from the host.
- Host responds with a compact actor-state snapshot (position/velocity + ids).
- Client applies it and continues.

If resync fails repeatedly, the session reports a desync.

### Extra players on low-player maps (non-optimized mode)

If more players join than the map was authored for:

- The first N players use authored start positions.
- Additional players are auto-placed by the host:
  - choose the closest safe, free floor tile with clear sight to the base player start
  - fall back to a random free tile if needed

The host broadcasts placements so all instances spawn identically.

### Restarts / next level

Restarts are host-authoritative:

- Host sends `NET_RESTART`.
- Clients restart via network-specific entry points (avoids host-only guards).

This ensures all instances restart in lockstep.

## Deployment

### Docker (single container)

For Internet play, both services can be run in one container:

- `tools/internet_lobby_server.py` (UDP lobby)
- `tools/relay_server.cc` (UDP ENet relay)
- `tools/tcp_relay_server.cc` (TCP relay fallback)

The Docker image builds the UDP relay against distro ENet (`libenet-dev`) to match typical client
builds that use system ENet.

```sh
docker build -t enigma-mp .
docker run --rm \
  -p 12347:12347/udp \
  -p 12348:12348/udp \
  -p 12349:12349/tcp \
  enigma-mp
```

### Manual (no Docker)

On Ubuntu/Debian you typically want:

- `python3` for the lobby server
- `libenet-dev` for the UDP relay

Then run:

- `python3 tools/internet_lobby_server.py --host 0.0.0.0 --port 12347`
- `./enigma-relay --host 0.0.0.0 --port 12348`
- `./enigma-tcp-relay --host 0.0.0.0 --port 12349`

## Regression tests (ad-hoc)

The repository includes small, framework-free regression tests:

```sh
g++ -std=c++14 -D_THREAD_SAFE \
  -Isrc -Ilib-src/enigma-core -Ilib-src -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  tests/test_multiplayer_protocol.cc lib-src/enigma-core/libecl.a \
  -o /tmp/test_multiplayer_protocol && /tmp/test_multiplayer_protocol

g++ -std=c++14 -D_THREAD_SAFE \
  -Isrc -Ilib-src/enigma-core -Ilib-src -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  tests/test_input.cc src/input.cc lib-src/enigma-core/libecl.a \
  -o /tmp/test_input && /tmp/test_input
```

## Key files

Protocol + engine:
- `src/multiplayer.cc`
- `src/multiplayer.hh`
- `src/multiplayer_protocol.hh`
- `src/multiplayer_state.hh`
- `src/input.cc` / `src/input.hh`

UI:
- `src/gui/MultiplayerMenu.cc`
- `src/gui/MultiplayerMenu.hh`

Internet services:
- `tools/internet_lobby_server.py`
- `tools/relay_server.cc`
- `tools/tcp_relay_server.cc`

