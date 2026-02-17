# Multiplayer (User Guide + Architecture)

This document covers the full multiplayer feature set. It is split into:

- **User Guide**: how to use LAN/Internet multiplayer.
- **Architecture**: how it works internally and how we avoid desync.

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
- Connectivity:
  - Presets: **Good / Normal / Bad** apply a tuned set of multiplayer runtime settings.
  - **Auto detect connectivity** (recommended): when hosting, the preset is selected automatically
    based on the worst link to any client (see Architecture).

Notes:

- The actual game session port for direct-connect is `12345` (`kGamePort`). This is currently not
  configurable via the UI and is shared by LAN and Internet direct-connect.
- LAN discovery uses a separate UDP broadcast port `12346` (`kLobbyPort`), also not configurable.
- While a game is running, transport/lobby settings are intentionally not editable in Options.
  Some runtime-tuning settings in `MP Debug`, `MP Sync`, and `MP Netsim` are editable (host-only);
  values that cannot safely be changed during gameplay are shown grey/locked.
- If Internet mode is selected but the server is still unresolved (for example `CHANGEME`),
  the lobby shows a warning to configure the lobby/relay host in Options.

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

Within one process session, leaving/re-entering the multiplayer menu keeps the selected
mode (LAN/Internet) and current Internet room state.

If the host leaves a room, the room is closed immediately for all clients and the room code
can be reused right away.

If UDP is blocked (guest WiFi, corporate networks, VPNs), enable **TCP relay** as fallback.

### Debugging toggles

Debug toggles can be set via environment variables before launching Enigma.

For development builds, `--show-debugoptions` enables a Debug tab in `Options` that
persists the same settings in the preferences file (env vars still take precedence
when set).

- `ENIGMA_MP_DEBUG=1` enables verbose multiplayer logs.
- `ENIGMA_MP_FORCE_RELAY=1` forces relay use (skips direct connect), useful for testing.
- `ENIGMA_MP_BIND_LOCAL=1` forces ENet direct-connect to bind to the probed local interface
  (sometimes helps on multi-homed hosts/VPNs; can hurt in some VM/NAT setups).
- `ENIGMA_MP_DUMP_STATE=1` dumps a one-time deterministic actor digest when a sync mismatch is detected.
- `ENIGMA_MP_TRACE_WORLDINIT=1` traces how `WorldInitLevel()` initializes actors (useful for tracking controller/ownership changes from Lua/compat).
- `ENIGMA_MP_NETSIM=1` enables artificial packet delay/loss/jitter for multiplayer payloads (debug only):
  - `ENIGMA_MP_NETSIM_DELAY_MS=<n>` base one-way delay
  - `ENIGMA_MP_NETSIM_JITTER_MS=<n>` adds +/- jitter
  - `ENIGMA_MP_NETSIM_DROP_PCT=<0..100>` packet drop percentage
  - `ENIGMA_MP_NETSIM_DUP_PCT=<0..100>` packet duplication percentage
  - `ENIGMA_MP_NETSIM_ALL=1` applies simulation to all session packets (default is gameplay packets only)
- `ENIGMA_MP_ZEROFILL_INPUTS=1` disables lockstep stalling on missing per-tick inputs (missing inputs are treated as zero force/actions, which may increase desync corrections under loss/latency).
- `ENIGMA_MP_PREDICT_MISSING_MOUSE_TICKS=<n>` (requires `ENIGMA_MP_ZEROFILL_INPUTS=1`) predicts missing mouse-force inputs by holding the last value and linearly decaying it to zero over `n` missing ticks. Discrete inputs (rotate/activate) are never repeated.

## Architecture

### High-level model: deterministic lockstep

Multiplayer is deterministic lockstep:

- Each instance runs the full simulation locally.
- Inputs are exchanged per fixed tick.
- The host distributes inputs and sends periodic sync snapshots.
- On divergence, clients request authoritative correction snapshots from the host (actors and world grid).

This keeps latency low while maintaining a consistent world state.

### Lobby discovery and start

LAN discovery:

- Each peer periodically broadcasts `LobbyAnnounce` over UDP broadcast.
- The host starts by broadcasting `LobbyStart`.

Ports:

- `kLobbyPort = 12346` for UDP broadcast discovery (`LobbyAnnounce`, `LobbyStart`)
- `kGamePort = 12345` for the actual game session (ENet direct-connect)

Internet discovery (room codes):

- Server: `tools/internet_lobby_server.py` (UDP, default 12347)
- Room membership uses Create/Join/Leave/Poll requests.
- Poll/Join responses can include room member ids + display names (used by the lobby player list).
- If the host leaves, the room is removed immediately (clients are forced out of that room).
- On normal app shutdown, Enigma sends a best-effort `LEAVE` for the tracked current room.
- Timeout is still used as fallback cleanup for orphaned rooms (for example crashes).

`LobbyStart` carries session metadata:

- `session_id`
- `seed` (for deterministic RNG)
- `pack_name` (fully qualifies the map across level packs; clients switch to this pack before loading)
- `host_ips` (optional list of IPv4 candidates the client can try for direct connect on multi-homed hosts, VMs, VPNs)
- `expected_players`
- `host_port` (direct-connect game port; currently `12345`)
- `host_id`
- map filter settings (minimum intended player count)

#### Direct-connect retries on multi-homed hosts

In LAN mode (and in Internet mode when direct-connect is enabled), clients may attempt to connect to
multiple `host_ips` (for example: a VM interface and a WiFi interface on the same machine). Some
networks can produce "half-open" attempts where:

- the host observes an ENet `CONNECT` event, but
- the client never receives the `WELCOME` handshake.

If the host only allows `expected_players - 1` peers and allocates player ids monotonically, such a
failed attempt can consume the only available remote slot and make subsequent retry attempts fail,
leaving the host stuck on "Waiting for other players to connect...".

To make this robust, the host:

- allocates remote player ids from a reusable pool (`player_in_use`), and
- allows extra ENet peer capacity for retries, and
- before the session starts, may drop an existing *unready* peer to accept a retry connection.

In debug logs this shows up as:

- `mp host: dropping unready peer ... (retry connect)`

### "Other players connecting..." start barrier

At level start, the host may need to wait for clients to connect and send `READY` before the
session can begin. To avoid desync and avoid "flashing" the world for a single frame:

- The host and clients enter a dedicated waiting screen (`cls_waiting_for_network_start`).
- The level is only shown once `SessionState::Phase` allows start.
- The game timer only starts ticking once all players are ready.

The host gates the simulation on explicit client readiness:

- Clients send `NET_READY` only once the level pack is switched and the new level is fully loaded.
- The host waits until all peers have reported READY (and any extra-player placement is complete).

To avoid stale packets from previous levels/restarts accidentally unblocking the current run, READY
and START are validated against the current `session_id`, `epoch`, and a per-session monotonic
`load_id` (see "Restarts / next level" below).

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
  the client requests an authoritative world-grid snapshot from the host and applies it. A toast is
  still shown after repeated detections, but the intent is to recover without requiring a restart.

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

#### World-state resync ("grid resync")

World mismatches are common in physics-heavy or scripting-heavy levels even when actors stay close.
To keep gameplay viable, the host can provide an authoritative world-grid snapshot:

- Client sends `NET_WORLD_STATE_REQUEST(epoch, tick)` to the host.
- Host replies with `NET_WORLD_STATE` containing:
  - full grid kind + state for floors/stones/items (compressed via a kind dictionary)
  - positions of movable stones (puzzle stones/doors) by `(x,y)` so swaps can be corrected
- Host may also broadcast `NET_WORLD_STATE` periodically (debug stride) to force convergence without
  waiting for an explicit request. These broadcasts can be unreliable and arrive reordered under
  jitter/duplication, so clients drop stale world-state ticks.
- Client first applies the movable-stone permutation (moves existing movable stones into the
  authoritative `(x,y)` positions) to preserve object identity and attributes.
- Client then applies authoritative per-tile kinds and states. Kind strings are expected to be
  template-backed (usable with `MakeFloor` / `MakeStone` / `MakeItem`). For some objects the XML
  validator kind (`Object::getKind()`) is insufficient to describe runtime variants; puzzle stones are
  the main example (their visual shape depends on `connections` and their kind depends on color).
  World snapshots therefore encode a stable kind derived from the object attributes so receivers do
  not thrash between `st_puzzle` and `st_puzzle_yellow` variants.
- Applying per-tile `state` is done as a diff, not a blind set: many objects treat `state` as an
  operation (start/stop animations, schedule callbacks). Reapplying identical state values can cause
  visible flicker (e.g. doors "open, close, reopen") when snapshots are frequent.

This is intentionally conservative and may visually "snap" world objects back to the host state.

### Session settings synchronization (host -> clients)

To avoid sessions where players run with incompatible multiplayer debug/runtime settings, the host
broadcasts a `NET_DEBUG_OPTIONS` packet before `NET_START` (and periodically during gameplay):

- Session parameters that must match across peers:
  - Tick length (`tick_ms`), negotiated by the host and sent in `WELCOME` (clients apply it immediately).
  - Input delay override (interpreted as "legacy 10ms ticks" and converted to current ticks; see below).
- Runtime/debug behavior flags:
  - zero-fill, rollback/replay, smoothing, local resync skip, host broadcast strides, etc.
- Network simulation settings (debug-only) so host/client behavior matches during tests.

During gameplay, clients continue to apply host-broadcast debug options, but *simulation-critical*
settings are intentionally locked once the world is RUNNING:

- Safe to change while RUNNING (host broadcasts; clients apply immediately): most `MP Debug` flags,
  most `MP Sync` flags, and all `MP Netsim` values.
- Locked while RUNNING (require a restart / reconnect to take effect): tick length, input delay,
  and transport-binding options like force-relay/bind-local.

The Options UI reflects this by greying out locked values during gameplay.

### MP Debug / MP Sync / MP Netsim options (reference)

These options live under `Options -> MP Debug`, `Options -> MP Sync`, and `Options -> MP Netsim`.
They are stored as `MultiplayerDebug*` preferences. When hosting, the selected values are broadcast
to clients via `NET_DEBUG_OPTIONS` (on join/start, and periodically while running).

Important: Several "ticks" values are intentionally interpreted as *legacy 10ms ticks* so presets
remain stable even if `Tick length ms` changes. The engine converts them to "current ticks" based on
the negotiated tick length.

#### MP Debug

- **MP logs** (`MultiplayerDebugLogging`, default: off)
  - Enables verbose multiplayer logging (packet flow, state transitions, desync classification).
- **MP dump** (`MultiplayerDebugDumpState`, default: off)
  - When a sync mismatch is detected, dump a one-time deterministic actor digest to the log to aid
    debugging desyncs.
- **MP trace init** (`MultiplayerDebugTraceWorldInit`, default: off)
  - Traces how `WorldInitLevel()` initializes actors (useful for tracking ownership/controller
    changes from Lua/compat mappings).
- **MP smooth render** (`MultiplayerDebugSmoothRender`, default: on)
  - Render-only smoothing for actor teleports caused by resync corrections. Does not affect
    simulation state.
- **MP skip local resync** (`MultiplayerDebugSkipLocalResync`, default: off)
  - When applying a resync snapshot, skip teleporting actors controlled by the local player.
    This reduces "snap back" locally, but can temporarily increase divergence between peers.
    Automatically ignored in "remote local ball" mode (where the local ball must be overwritten).
- **MP force relay** (`MultiplayerDebugForceRelay`, default: off; locked while RUNNING)
  - Forces relay use (skips direct-connect). Connection-time only.
- **MP bind local** (`MultiplayerDebugBindLocal`, default: off; locked while RUNNING)
  - Forces ENet direct-connect to bind to the probed local interface (can help on multi-homed hosts
    or VPN setups; can also make some NAT/VM setups worse). Connection-time only.
- **Host resync stride** (`MultiplayerDebugHostBroadcastResyncStrideTicks`, default: 50 legacy ticks)
  - Host-side periodic broadcast of authoritative actor snapshots (`NET_RESYNC_STATE`) over
    unreliable transport. A value of `50` means "about 0.5s at 10ms ticks", and is converted to the
    current tick length for stability.
  - In **MP remote local ball** mode, the host forces this to per-tick snapshots to keep the local
    ball responsive.
- **Host world stride** (`MultiplayerDebugHostBroadcastWorldStateStrideTicks`, default: 50 legacy ticks)
  - Host-side periodic broadcast of authoritative world-grid snapshots (`NET_WORLD_STATE`) over
    unreliable transport. Also interpreted as legacy 10ms ticks and converted to current ticks.
- **Rollback keep ticks** (`MultiplayerDebugRollbackKeepTicks`, default: 200)
  - Size of the client rollback history in simulation ticks (used when rollback/replay is enabled).
    The effective value is clamped to also cover input delay and input bundle back-window to avoid
    frequent "too late to replay" cases.

#### MP Sync

- **MP zerofill** (`MultiplayerDebugZeroFillInputs`, default: off)
  - Disables lockstep stalling on missing inputs: if an input for a (tick, player) is missing,
    it is treated as "no input" (zero mouse force and no actions). Improves playability under loss,
    but increases the odds of divergence that must be corrected by resync.
- **MP rollback** (`MultiplayerDebugRollbackEnabled`, default: off)
  - Enables client-side rollback/replay reconciliation when resync snapshots arrive. Requires
    **MP zerofill**. The host never rolls back (authoritative host stays monotonic).
- **MP remote local ball** (`MultiplayerDebugRemoteControlLocalBall`, default: off)
  - "Host authoritative local ball" experiment: on clients, continuous mouse force is suppressed in
    local simulation (discrete actions like rotate/activate still apply), and the host keeps the
    locally controlled ball in sync via frequent authoritative actor snapshots.
- **MP client auth pos** (`MultiplayerDebugClientAuthBallPos`, default: off)
  - "Client authoritative local ball position" experiment: clients send per-tick position/velocity
    updates for their locally controlled steerable actors. The host applies bounded corrections and
    rebroadcasts the owner state to other peers. Sync logic ignores expected local-ball position
    mismatch in this mode.
- **MP host world only** (`MultiplayerDebugHostOnlyWorldInteractions`, default: off)
  - Host authoritative world interactions: on clients, suppress world mutations caused by actor
    movement/collisions (triggers, items, stone touch/hit). Clients rely on host snapshots to
    converge world state. This mitigates flicker and "stone snap back" under poor links.
- **Predict mouse ticks** (`MultiplayerDebugPredictMissingMouseTicks`, default: 0)
  - Only active when **MP zerofill** is enabled. When mouse-force samples are missing, hold the last
    consumed mouse force and linearly decay it to zero over `N` missing ticks. Discrete actions
    (rotate/activate) are never repeated.
- **Input delay ticks** (`MultiplayerDebugInputDelayTicks`, default: 4 legacy ticks; locked while RUNNING)
  - Input lookahead used to stamp local input into a future tick to absorb jitter. The value is in
    legacy 10ms ticks and converted to current ticks based on `Tick length ms`. Locked during
    gameplay because changing it mid-session breaks the lockstep timing assumptions.
- **World desync streak** (`MultiplayerDebugWorldDesyncStreakForWorldStateRequest`, default: 0)
  - Threshold for consecutive world-grid checksum mismatches before requesting a `NET_WORLD_STATE`
    snapshot from the host. `0` means "use built-in default" (`kWorldDesyncStreakForWorldStateRequest`,
    currently 2).
- **Tick length ms** (`MultiplayerDebugTickLengthMs`, default: 10; range: 5..50; locked while RUNNING)
  - Simulation tick duration in milliseconds. Must match across all peers, is negotiated by the host
    at join/start, and cannot be changed safely once the session is RUNNING.

#### MP Netsim

These are debug-only controls that simulate latency, jitter, drop, and duplication in the
multiplayer transport layer.

- **MP netsim** (`MultiplayerDebugNetSimEnabled`, default: off)
  - Enables NetSim for selected packet types (gameplay packets by default).
- **MP netsim all** (`MultiplayerDebugNetSimAll`, default: off)
  - Applies NetSim to all session packets, including control-plane packets like `WELCOME`.
  - Join timeouts are scaled up accordingly so simulated high latency doesn't make joins flaky.
- **Netsim delay ms** (`MultiplayerDebugNetSimDelayMs`, default: 0)
  - Base one-way delay. Applied on both send and receive paths. For RTT measurements, the added
    round-trip time can therefore be roughly `~4 * delay_ms` when both request and response traverse
    delayed send and delayed receive paths.
- **Netsim jitter ms** (`MultiplayerDebugNetSimJitterMs`, default: 0)
  - Adds uniform random jitter in `[-jitter_ms, +jitter_ms]` to each simulated one-way delay.
- **Netsim drop %** (`MultiplayerDebugNetSimDropPct`, default: 0)
  - Drop percentage applied only to payload types that are intended to be lossy at the application
    layer (inputs). Reliable control-plane packets are not dropped to avoid bypassing ENet/TCP
    retransmission semantics.
- **Netsim dup %** (`MultiplayerDebugNetSimDupPct`, default: 0)
  - Duplicate percentage. Duplicates are delivered with their own (potentially different) simulated
    delay.

### Connectivity auto-detect (host)

When **Auto detect connectivity** is enabled, the host runs a short RTT probe once all peers have
loaded the level and reported READY, but before `NET_START`:

- Host sends `NET_PING(id)` bursts to each connected client and records `NET_PONG(id)` timing.
- The host chooses **Good / Normal / Bad** based on the worst observed link (p90 RTT).
  - Thresholds (worst-link p90 RTT): **Good** `<= 70 ms`, **Normal** `<= 180 ms`, **Bad** `> 180 ms`.
- The selected preset is applied to the host settings and broadcast via `NET_DEBUG_OPTIONS` so all
  clients run with the same parameters for that session.

### Global pause (multiplayer)

Because the simulation is lockstep and each instance must keep pumping the network, pausing is
implemented as a replicated state derived from menu-open state:

- Clients notify the host when their ESC menu opens/closes (`NET_MENU`).
- The host broadcasts the resulting pause/unpause decision to all peers (`NET_PAUSE`).
- While paused, the simulation tick does not advance and no inputs are emitted, but the network is
  still polled so unpause/leave/disconnect is handled promptly.
- All instances show a pause screen (`cls_multiplayer_paused`) while any player has the menu open.

### Lockstep stall wait dialog ("waiting for player...")

In lockstep, if an instance cannot advance a tick because it is missing inputs from some peer, the
simulation would otherwise appear "frozen". This can happen transiently during short network outages
or relay hiccups (especially over Internet play), even if the underlying transport connection is
still technically up.

To make this visible and to avoid buffered-input side effects when the stall clears, the client shows
a dedicated waiting screen:

- Trigger: if the session is active and `input::CanAdvanceTick()` stays false for
  `wait::kStallDialogDelaySeconds` (currently 2s).
- State: `cls_multiplayer_waiting_for_players` with `gui::MultiplayerWaitMenu` and a countdown.
- Recovery: as soon as `input::CanAdvanceTick()` becomes true again, the dialog closes automatically
  and the game continues.
- Abort: if the countdown reaches 0 or the player presses **Leave**, the client requests a
  multiplayer abort (`multiplayer::RequestAbort()`), which ends the game for everyone.

This is not a "true reconnect" flow. If the transport reports an actual disconnect, the session
aborts immediately; the wait dialog only covers the window where the connection is alive but inputs
stop arriving.

#### Timeout alignment and input mitigation

To reduce cases where ENet disconnects before the wait dialog finishes, ENet peers are configured
(when available) to use a timeout that matches the wait dialog:

- ENet >= 1.3: `wait::kAbortTimeoutSeconds = 60` and `wait::kEnetPeerTimeoutMs = 60000` via
  `enet_peer_timeout()`.
- Vendored ENet 1.0: peer timeout is not configurable; the wait dialog uses a shorter timeout
  (`wait::kAbortTimeoutSeconds = 30`) to match ENet's default maximum timeout window.

While the wait dialog (and the global pause screen) is shown, the client:

- flushes queued `SDL_MOUSEMOTION` events and drains local pending input so motion/impulses don't
  accumulate during a stall,
- recenters and restores gameplay mouse control when resuming, and
- freezes the multiplayer input clock (`multiplayer::SetInputClockFrozen(true)`) so a long stall does
  not permanently increase input lookahead/latency after recovery.

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

- If auto-placement cannot find a valid free tile for an extra spawned actor (rare; typically only in
  very constrained or script-heavy levels), the session will still start. In that case the extra
  actor remains at its deterministic initial spawn position and the host logs a debug message.

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

Next-level transitions are also host-authoritative.

Historically, the single-player engine advances the current index locally and calls
`client::Msg_AdvanceLevel(...)`. In multiplayer, doing this independently on each peer is fragile:
if a client misses a transition signal (especially via relays), it can remain on the old level while
the host advances.

To keep transitions reliable and deterministic, the host broadcasts a fully qualified load command:

- Host advances the current index locally and broadcasts `NET_LOAD_LEVEL` with:
  - `load_id` (monotonic per session)
  - `pack_name` (level pack name)
  - `level_id` (normalized level path)
- Clients do not advance their index locally; they wait for `NET_LOAD_LEVEL`, switch to the named
  pack, resolve the level proxy by normalized path, and load it.
- After loading, clients send `NET_READY(session_id, epoch, load_id)`.
- Once all peers are READY, the host increments `epoch` and broadcasts
  `NET_START(epoch, load_id)` to begin simulation.

The `load_id` is intentionally included in READY/START so late packets from the previous level do
not unblock the next level's start barrier.

## Maintainer notes

- `doc/multiplayer_pr_review.html` is generated from the current branch diff via:
  - `PYTHONDONTWRITEBYTECODE=1 python3 tools/create_multiplayer_pr_review.py`
  - (default base is `origin/master`; override via `--base <ref>` if needed)
- The generator intentionally excludes this architecture document so the review stays focused on source changes.
- The integration test harness lives under `tools/mp_test_env.py` and uses `--mp-test-*` flags
  (implemented in `src/multiplayer_test_driver.*`) to drive host/client instances.

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
