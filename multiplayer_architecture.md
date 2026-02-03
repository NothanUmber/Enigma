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

- Host connects to the relay and registers the session id.
- Clients connect to the relay with the same session id.
- Relay assigns a client id, notifies the host, and forwards packets.
- Host sends data back using a relay envelope (client id + payload).

Relay server:

- `tools/relay_server.cc`
- Default: lobby port + 1 (e.g., 12347 → 12348)
- This mode works for typical home NAT setups because both host and clients
  initiate outbound connections to the relay.

Lobby UI:

- Uses lobby server field as base; relay is auto‑derived.
- `multiplayer::SetRelayServer()` stores relay address used by client connect.

## Deployment (simple single container)

For Internet play, both services can be run in one container:

- `Dockerfile` builds `tools/relay_server.cc` and bundles
  `tools/internet_lobby_server.py`.
- The relay is built against the bundled ENet sources to match the game.
- `tools/docker-entrypoint.sh` starts both processes.
- Expose UDP 12347 (lobby) and 12348 (relay).

This is intentionally a single container setup; docker‑compose is not required
unless you want to split services or add extra tooling.

Build and run:

```
docker build -t enigma-mp .
docker run --rm -p 12347:12347/udp -p 12348:12348/udp enigma-mp
```

Optional overrides:

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
