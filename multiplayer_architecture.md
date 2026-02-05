# Multiplayer Architecture (Enigma)

This document explains the current multiplayer implementation for developers.
It is intended to be a technical map of the system (LAN + Internet) and the
determinism/anti‑desync safeguards that keep sessions stable.

## High‑level model

Multiplayer uses deterministic lockstep:

- Each instance runs the full simulation locally.
- Inputs are exchanged per fixed tick.
- The host relays inputs but does not authoritatively stream state.
- Periodic sync snapshots are used for drift detection and RNG correction.

This keeps latency low while maintaining a consistent world state.

## Lobby and UI

### LAN lobby (unchanged)

LAN uses UDP broadcast for discovery:

- Each peer periodically broadcasts `LobbyAnnounce` (id/name/selected level/player count).
- The multiplayer lobby UI lists peers and available levels.
- Host clicks a level icon to start. A `LobbyStart` is broadcast.

### Internet lobby (room codes)

Internet mode uses a minimal UDP lobby server (no broadcast).

Server:
- `tools/internet_lobby_server.py` (UDP; default port 12347)
- Supports Create / Join / Leave / Start / Poll
- Rooms are removed when last member leaves or on TTL expiry.

Client flow (Internet mode):

1) **Create room** (host):
   - Host enters room code and clicks **Create Room**.
   - Room is created on the lobby server.
   - Host sees the level list.
2) **Join room** (clients):
   - Clients enter room code and click **Join Room**.
   - Clients see “Waiting for host…” (level list hidden).
3) **Start** (host):
   - Host clicks a level icon.
   - Host sends `INET_START` to the lobby server.
   - Clients poll `INET_POLL` until “started” and then auto‑connect.

### Map filtering (“Maps for N”)

Filter button cycles a **minimum player count** range:

- For N players: `Maps for N`, `Maps for 1‑N`, `Maps for 2‑N`, … `Maps for N‑N`.
- Filtering uses `min_players <= level_players <= N`.
- The selected minimum is encoded in `LobbyStart.filter_optimized` and synced.

## Session start and transport

### Session start (`LobbyStart`)

`LobbyStart` carries:

- `session_id`
- `seed` (level RNG)
- `expected_players`
- `host_port`
- `host_id`
- `filter_optimized` (repurposed as **min players**)

LAN:
- Host broadcasts `LobbyStart` over UDP broadcast.
- Clients connect directly to host IP:port and start.

Internet:
- Host registers and starts via lobby server (UDP).
- Clients poll the lobby server until started, then connect.

### ENet

ENet is used for all game‑session traffic:

- Host runs an ENet server.
- Clients connect via ENet peer.
- Host relays inputs and broadcasts sync snapshots.

## Determinism and anti‑desync

### Input lockstep

- Inputs are queued per tick for all players.
- `input::CanAdvanceTick()` enforces that all inputs are present.
- A fixed input delay (`kInputDelay`) queues future inputs to absorb jitter.

### Input epoch

Every level start/restart increments `input_epoch`.

Why: prevents late packets from a previous level from contaminating the new one.

Mechanism:
- Host increments epoch at start.
- `NET_START`, `NET_INPUT`, `NET_SYNC` include epoch.
- Mismatched epochs are dropped.

### RNG sync

Periodic `NET_SYNC` packets include:

- Tick
- RNG state
- (sample) player positions

Client behavior:

- If positions match but RNG differs → RNG is corrected silently.
- If positions differ → desync is reported.

### Determinism fixes in game logic

Some systems originally used local “current player” state, which diverged
between instances. These were adjusted to use actor/controller state instead,
ensuring consistent simulation decisions across peers.

## Extra players on low‑player maps (non‑optimized mode)

When more players join than the level was authored for:

- The first N players use the level’s authored start positions.
- Additional players are **auto‑placed** by the host:
  1) Choose the closest **safe** floor tile with clear sight to the base player’s
     start (same screen).
  2) If none exist, fall back to any free tile (same screen), then to any free
     tile in the whole level.

“Safe” currently excludes:
- abyss, water, space/space_force (already excluded as non‑free)
- swamp, thief (additional “avoid” list)

The host applies placements and broadcasts them to clients.

## Restarts and resyncs

Restarts are host‑authoritative:

- Host sends `NET_RESTART`.
- Clients now call `RestartLevelFromNetwork()` / `Msg_RestartGameFromNetwork()`
  to avoid host‑only guards.

This ensures all instances restart in lockstep (no local divergence).

## Internet connectivity (direct + relay)

### Direct UDP

Clients attempt direct ENet connection to the host IP:port.

### Relay fallback

If direct connect fails:

- Host connects to the relay and registers the session id (and keeps a per-client mapping).
- Clients connect to the relay with the same session id.
- Relay assigns a client id, notifies the host, and forwards packets.
- Host sends packets back using a relay envelope (client id + payload).

Relay server:

- `tools/relay_server.cc`
- Default: 12348/udp (configurable)
- This mode works for typical home NAT setups because both host and clients
  initiate outbound connections to the relay.

Important: The UDP relay uses ENet itself (clients connect to the relay using
`enet_host_connect`). That means the relay must be built against an ENet
version compatible with the clients. In practice this is easiest if everything
uses ENet 1.3.x (Ubuntu/Debian `libenet-dev`, Homebrew `enet`, etc.).

### TCP relay fallback (UDP blocked)

Some networks (guest WiFi, corporate networks, certain VPNs) block UDP. For
those cases there is a TCP relay fallback:

- Relay server: `tools/tcp_relay_server.cc` (default 12349/tcp, configurable)
- Transport is selectable via config toggles:
  `Direct` > `UDP relay` > `TCP relay` (in that order, skipping disabled ones).

TCP is expected to have worse latency/jitter than direct UDP/ENet, but provides
a “works almost anywhere” fallback.

## Deployment (simple single container)

For Internet play, both services can be run in one container:

- `Dockerfile` builds `tools/relay_server.cc` and bundles
  `tools/internet_lobby_server.py`.
- The UDP relay is built against the distro-provided ENet (`libenet-dev`) to
  match typical client builds that use system ENet (Ubuntu/Debian packages,
  Homebrew builds, etc.).
- `tools/docker-entrypoint.sh` starts both processes.
- Expose UDP 12347 (lobby), UDP 12348 (ENet relay), and TCP 12349 (TCP relay).

This is intentionally a single container setup; docker‑compose is not required
unless you want to split services or add extra tooling.

Build and run:

```
docker build -t enigma-mp .
docker run --rm \
  -p 12347:12347/udp \
  -p 12348:12348/udp \
  -p 12349:12349/tcp \
  enigma-mp
```

Optional overrides:

## Configuration knobs (developer-facing)

Multiplayer configuration is stored in the local Enigma config and includes:

- Internet server host (single host used for lobby + relays)
- Ports: lobby / UDP relay / TCP relay
- Enable/disable: direct connect / UDP relay / TCP relay

Runtime debug toggles (environment variables):

- `ENIGMA_MP_DEBUG=1`: enable verbose multiplayer logs
- `ENIGMA_MP_FORCE_RELAY=1`: skip direct connect and force relay use (useful for
  local testing or to compare latency)

## Running the small regression tests (ad-hoc)

These tests are intentionally tiny and do not use a framework. They can be
built directly against the already-built core library:

```
g++ -std=c++14 -D_THREAD_SAFE \
  -Isrc -Ilib-src/enigma-core -Ilib-src -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  tests/test_multiplayer_protocol.cc lib-src/enigma-core/libecl.a \
  -o /tmp/test_multiplayer_protocol && /tmp/test_multiplayer_protocol

g++ -std=c++14 -D_THREAD_SAFE \
  -Isrc -Ilib-src/enigma-core -Ilib-src -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  tests/test_input.cc src/input.cc lib-src/enigma-core/libecl.a \
  -o /tmp/test_input && /tmp/test_input
```

```
docker run --rm \
  -e LOBBY_HOST=0.0.0.0 -e LOBBY_PORT=12347 \
  -e RELAY_HOST=0.0.0.0 -e RELAY_PORT=12348 \
  -p 12347:12347/udp -p 12348:12348/udp \
  enigma-mp
```

## Debugging

Enable debug logs:

```
ENIGMA_MP_DEBUG=1
```

Useful logs include:

- input send/receive per tick
- missing input warnings
- sync skips and desync details
- connection and start events

## Key files

Core networking:
- `src/multiplayer.cc`
- `src/multiplayer.hh`
- `src/multiplayer_protocol.hh`

UI:
- `src/gui/MultiplayerMenu.cc`
- `src/gui/MultiplayerMenu.hh`

Servers:
- `tools/internet_lobby_server.py`
- `tools/relay_server.cc`

Gameplay integration:
- `src/server.cc` / `src/server.hh`
- `src/player.cc`
- `src/world.cc`
