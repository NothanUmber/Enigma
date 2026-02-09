# Multiplayer (User Guide + Architecture)

This document covers the full multiplayer feature set. It is split into:

- **User Guide**: how to use LAN/Internet multiplayer.
- **Architecture**: how it works internally and how we avoid desync.

It is not a changelog.

## User Guide

### Opening multiplayer

From the main menu, choose **Network Game** to open the multiplayer lobby.

### In-game controls (multiplayer)

- `ESC`: open the in-game menu (local) and pause the session (global)
- `SHIFT+ESC`: abort the current level immediately

In multiplayer, the in-game menu is still shown, but it is stepped from the main loop (non-blocking).
That keeps the multiplayer network pump alive while a menu is open.

While at least one player has the menu open, all instances show the global pause screen. Other players
may press `ESC` to open their own menu as well. The session resumes once all menus are closed.

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
- `ENIGMA_MP_DUMP_STATE=1` dumps a one-time deterministic actor digest when a sync mismatch is detected.
- `ENIGMA_MP_TRACE_WORLDINIT=1` traces how `WorldInitLevel()` initializes actors (useful for tracking controller/ownership changes from Lua/compat).

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
- `pack_name` (fully qualifies the map across level packs; clients switch to this pack before loading)
- `host_ips` (optional list of IPv4 candidates the client can try for direct connect on multi-homed hosts, VMs, VPNs)
- `expected_players`
- `host_port`
- `host_id`
- map filter settings (minimum intended player count)

### "Other players connecting..." start barrier

At level start, the host may need to wait for clients to connect and send `READY` before the
session can begin. To avoid desync and avoid "flashing" the world for a single frame:

- The host and clients enter a dedicated waiting screen (`cls_waiting_for_network_start`).
- The level is only shown once `SessionState::Phase` allows start.
- The game timer only starts ticking once all players are ready.

`READY` is validated against the current `session_id` and `epoch` to avoid stale packets from
previous runs accidentally unblocking a new session.

### Session transport options

All game-session payloads use a single message protocol (`src/multiplayer_protocol.hh`).
How those payloads are transported depends on connectivity:

1) **Direct**: client connects to host via ENet (`host_ip:host_port`).
2) **UDP relay**: client connects to relay via ENet; relay forwards ENet packets between client and host.
3) **TCP relay**: client connects to relay via raw TCP; relay forwards framed payloads between client and host.

To keep session logic independent of the concrete transport, multiplayer uses a small transport facade:

- `src/multiplayer_transport.hh` / `src/multiplayer_transport.cc` provides one `Poll()` + `Send()` API.
- Session code consumes that facade and does not branch on "direct vs relay" at every call site.

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
- player reference positions (p0/p1)
- checksums over relevant world/actor state

Clients compare these snapshots to local state:

- If only RNG diverged, RNG is corrected without a resync.
- If reference positions drift beyond `kSyncPosEpsilon` or RNG diverges (non-RNG-only case), a
  *soft resync* is attempted after a short consecutive mismatch streak.
- If actor checksums diverge without any position/RNG/world mismatch, a soft resync can still be
  attempted, but only after a longer mismatch streak (actor checksums can be sensitive).
- If the world checksum diverges *without* any position/RNG/actor mismatch ("world-only mismatch"),
  we currently treat this as not auto-recoverable and show a "World desync detected. Please restart."
  toast after repeated detections. (This is where a future hard-resync could go.)

In debug logs this is visible in the `mp desync... flags(pos=... rand=... actor=... world=...) action=...`
line, where `action` is one of:

- `rng-fix`: RNG-only mismatch, fixed by setting local RNG to the snapshot value
- `soft-resync`: position/RNG mismatch likely recoverable by resyncing actor state
- `actor-resync`: actor checksum mismatch only (position/RNG/world match), resync after longer streak
- `diagnostic-only`: checksum mismatch observed, but no recovery action taken

##### Reference actor probing (multi-ball + "stable sinks")

The "reference positions" (`p0`/`p1`) are not always a literal "main ball". Instead, for each player we
choose a deterministic *probe actor* via `sync_reference_actor(player, tick)`:

- Prefer steerable actors controlled by that player.
- Use a stable sort key (kind/owner/controllers/color/name/id), not position.
- Cycle the chosen probe deterministically over time.

The cycling is crucial for levels where a player controls multiple marbles (e.g. Per.Oxyd meditation
pearls). If a single fixed probe happens to be in a "stable sink" (for example: stuck in a hole), its
position can remain identical across peers even while other controlled marbles diverge. Cycling ensures
each controlled marble is eventually sampled and can trigger recovery.

##### Checksum surface (what is hashed)

The goal of checksums is to detect "world divergence" even if it does not show up as an obvious
position mismatch yet (for example: rotors, stateful stones, puzzle elements, etc.).

`WorldChecksum()` (see `src/world.cc`) hashes:

- Level dimensions (`w`, `h`).
- The *kind* and selected *state* of objects in the floor, stone, and item layers (for each tile).
- The list of actors: stable ids and quantized physics state (position/velocity).
- The list of "other" objects: stable id and kind (and selected state where applicable).

Notes:

- Positions and velocities are quantized before hashing to avoid false positives from tiny float drift.
- The checksum is intended to be deterministic across platforms as long as object state enumeration
  stays stable. If you change what is hashed, you change the desync detection behavior.

#### Resync ("soft resync")

When divergence is detected:

- Client requests a resync from the host.
- Host responds with a compact actor-state snapshot.
- Client applies it and continues.

Resync snapshots include, per actor:

- stable identifiers (`object_id`, `kind_id`, and `name_hash` where available)
- metadata that affects control (`owner`, `controllers`, `color`)
- physics state (`pos`, `vel`)

Applying a resync is intentionally conservative:

- Snapshots are applied "as-is" (no velocity projection). Projecting positions forward by the
  observed tick delta tends to amplify divergence in high-acceleration physics (rubberbands,
  collisions).
- To avoid gameplay side-effects, resync teleports update physics state and the spatial index only.
- Actors are matched primarily by `object_id`, but fall back to `name_hash` (and finally to a
  best-effort match by kind/owner/position). This is important for levels where scripting can
  create identical actors in a non-deterministic order across peers (e.g. meditation pearls).
- After applying, rubberband constraints are re-evaluated to avoid persistent post-resync drift in
  multi-ball levels.

If resync fails repeatedly, the session reports a desync.

### Global pause (multiplayer)

Because the simulation is lockstep and each instance must keep pumping the network, pausing is
implemented as a replicated state derived from menu-open state:

- Clients notify the host when their ESC menu opens/closes (`NET_MENU`).
- The host broadcasts the resulting pause/unpause decision to all peers (`NET_PAUSE`).
- While paused, the simulation tick does not advance and no inputs are emitted, but the network is
  still polled so unpause/leave/disconnect is handled promptly.
- All instances show a pause screen (`cls_multiplayer_paused`) while any player has the menu open.

### Extra players on low-player maps (non-optimized mode)

If more players join than the map was authored for:

- The first N players use authored start positions.
- If the authored level provides fewer steerable actors (balls) than session players, additional
  players are handled by *duplication + placement*:
  - duplicate a "base" actor (usually the base player's main marble/ball)
  - auto-place that duplicate on a safe, free floor tile near the base start position:
    - prefer the closest safe, free floor tile with clear sight to the base start
    - fall back to a random free tile if needed
- If the authored level provides more steerable actors (balls) than session players (common for
  Per.Oxyd "Meditation" levels), players are handled by *redistribution* instead:
  - do not spawn new balls
  - distribute the authored steerable actors as evenly as possible across the session players
    (for 2 players and 4 balls: 2+2; for 3 players and 4 balls: 2+1+1)
  - existing relationships between balls (e.g. rubberbands) remain exactly as authored; only
    which player controls which ball changes

The host broadcasts placements so all instances spawn identically.

Notes:

- Control assignment uses the explicit `controllers` bitmask on each actor. The legacy engine also
  supports control-by-`color` for old levels that never set controllers. In multiplayer we rely on
  controllers to be authoritative, so `Actor::controlled_by()` only falls back to `color` when
  `controllers == 0`. This allows distributing multiple same-colored balls (e.g. all-white pearls)
  across multiple players without changing their gameplay color.

- Redistribution is performed both before and after `WorldInitLevel()`. Some compatibility
  modes and Lua init code can overwrite `controllers` during initialization (notably meditation
  pearls created via old API mappings). The post-init pass ensures the final runtime controller
  masks match the intended distribution.

### Restarts / next level

Restarts are host-authoritative:

- Host sends `NET_RESTART`.
- Clients restart via network-specific entry points (avoids host-only guards).

This ensures all instances restart in lockstep.

## Key code locations

The refactor splits multiplayer into small translation units with focused responsibilities:

- Lobby:
  - `src/multiplayer_lan_lobby.cc` (UDP broadcast lobby)
  - `src/multiplayer_internet_lobby.cc` (room-code lobby client)
  - `tools/internet_lobby_server.py` (room-code lobby server)
- Session protocol:
  - `src/multiplayer_protocol.hh` (binary session protocol and lobby packets)
  - `src/multiplayer_relay_codec.cc` (relay framing header)
- Session runtime/state machine:
  - `src/multiplayer_internal.hh` (shared state structs and enums)
  - `src/multiplayer_session_runtime.cc` (high-level session API used by `src/multiplayer.cc`)
  - `src/multiplayer_session_transport.cc` (packet dispatch and transport sink)
  - `src/multiplayer_session_start.cc` (join/start/restart transitions)
  - `src/multiplayer_session_sync.cc` (sync/checksum/resync helpers)
- Transport implementations:
  - `src/multiplayer_transport.cc` (transport facade: poll + send/broadcast)
  - `src/multiplayer_enet_socket.cc` (ENet API shims and setup)
  - `src/multiplayer_tcp_socket.cc` (raw TCP relay socket)
  - `tools/relay_server.cc` (UDP/ENet relay)
  - `tools/tcp_relay_server.cc` (TCP relay)
- UI:
  - `src/gui/MultiplayerMenu.cc` + helpers in `src/gui/MultiplayerMenu_*.cc`
  - `src/gui/OptionsMenu.cc` (Multiplayer options tab)

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

g++ -std=c++14 -D_THREAD_SAFE \
  -Isrc -Ilib-src/enigma-core -Ilib-src -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  tests/test_multiplayer_ball_assignment.cc \
  -o /tmp/test_multiplayer_ball_assignment && /tmp/test_multiplayer_ball_assignment
```

## Key files

Protocol + engine:
- `src/multiplayer.cc`
- `src/multiplayer.hh`
- `src/multiplayer_config.cc` / `src/multiplayer_config.hh`
- `src/multiplayer_protocol.hh`
- `src/multiplayer_state.hh`
- `src/multiplayer_session.hh` (session entry points; implementation is split across `src/multiplayer_session_*.cc`)
- `src/multiplayer_internal.hh` (shared internal state and constants)
- `src/multiplayer_transport.cc` / `src/multiplayer_transport.hh` (unified send/poll across direct + relays)
- `src/multiplayer_lan_lobby.cc` (LAN broadcast lobby)
- `src/multiplayer_internet_lobby.cc` (Internet room requests to the lobby server)
- `src/multiplayer_extra_players.cc` / `src/multiplayer_extra_players.hh` (extra-player spawn/placement for non-optimized maps)
- `src/input.cc` / `src/input.hh`

UI:
- `src/gui/MultiplayerMenu.cc`
- `src/gui/MultiplayerMenu.hh`
- `src/gui/MultiplayerMenu_actions.cc`
- `src/gui/MultiplayerMenu_levels.cc`
- `src/gui/MultiplayerMenu_tick.cc`
- `src/gui/MultiplayerMenu_common.cc`
- `src/gui/MultiplayerMenu_internal.hh`

Internet services:
- `tools/internet_lobby_server.py`
- `tools/relay_server.cc`
- `tools/tcp_relay_server.cc`
