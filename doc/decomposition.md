# Multiplayer decomposition plan (branch `multiplayer_enet_unreliable` → `origin/master`)

This document proposes how to decompose the current experimental branch into **manageable pull requests**, each consisting of **small, coherent commits** that are reviewable in one sitting.

Reference point for this decomposition:
- Diff base: `origin/master` at `085e2bda1cba`
- Branch head: `multiplayer_enet_unreliable` at `06f682c645b6`
- Files changed vs base: `136`

Branch naming convention (proposal):
- Each pull request gets a dedicated branch named `NN_<short_topic>` where `NN` is 2-digit order.
- Foundational/tooling PRs are simply the first numbers (`01_...`, `02_...`, ...), so naming stays consistent across the whole series.
- Clearly out-of-roadmap experiments use a high number like `99_...`.

Notes:
- Many files are shared across multiple PRs (especially `src/multiplayer_protocol.hh`, `src/multiplayer_session_*.cc`, `src/gui/OptionsMenu.cc`). Where that happens, the commit plan explicitly calls out “add minimal subset now / extend later”.
- This is a **planning document only**; it intentionally does **not** apply any decomposition yet.
- Optional “pre-PRs” are listed first because they significantly reduce review load and risk, but they can be merged into PR1 if maintainers prefer.
- Documentation policy recommendation:
  - Each PR ends with a **docs commit** that updates `doc/multiplayer_architecture.md` and regenerates `doc/multiplayer_pr_review.html` **for that PR only** (i.e., with the PR’s base as `--base` for the generator). That way both documents grow incrementally with the merged stages.
- Debug UI policy recommendation:
  - Prefer **connectivity profiles/presets** over exposing raw `MultiplayerDebug*` knobs.
  - Keep any in-game overlays / debug-only settings either **omitted** or **strictly developer-only** so the shipped UI stays simple.
- Test harness policy recommendation:
  - Each PR should include at least one **scriptable test-harness scenario** under `tools/mp_test_scripts/` that exercises the PR’s new behavior (or moves an already-existing scenario into that PR if it already exists).
  - When a PR introduces a behavior with no existing scenario, add a new `tools/mp_test_scripts/*.txt` for it (plus any small harness extensions in `tools/mp_test_env.py` if needed).

---

## PR01–PR02 (foundation)

These are not in your 1–9 list, but splitting them out keeps the gameplay roadmap PRs smaller and more reviewable.

### PR01 — PR review tooling (no runtime impact)
Branch: `01_pr_review_generator`
**Goal:** Land the *generator* first; keep `doc/multiplayer_pr_review.html` and `doc/multiplayer_architecture.md` updates incremental as “final commits” inside each PR (recommended above).

Commits:
1. **Tools: PR review generator**
   - Touches:
     - `tools/create_multiplayer_pr_review.py`
2. **Docs: ignore rules / scaffolding (no full review HTML yet)**
   - Touches:
     - `.gitignore`
     - `doc/.gitignore`

### PR02 — Scriptable multiplayer test harness (developer-only)
Branch: `02_mp_test_harness`
**Goal:** Make it easy to reproduce LAN/Internet scenarios (latency/jitter/loss) while keeping gameplay PRs lean.

Commits:
1. **Test driver plumbing + CLI flags**
   - Touches:
     - `src/multiplayer_test_driver.cc`
     - `src/multiplayer_test_driver.hh`
     - `src/main.cc`
2. **Python harness + test scripts**
   - Touches:
     - `tools/mp_test_env.py`
     - `tools/mp_test_scripts/README.md`
     - `tools/mp_test_scripts/basic_join_and_move.txt` (baseline smoke)

---

## PR03 (Roadmap 1) — Bare-bones LAN/loopback lockstep (tick length + delay + zerofill; constant tick)
Branch: `03_mp_lockstep_lan`

Target outcome:
- 2+ players can create/join a session over loopback or LAN with **<10ms** latency.
- **Deterministic lockstep**: fixed tick length, input delay, and optional **zerofill** to avoid stalls.
- No Internet lobby/relay, no auto-resync (beyond “restart”), no host-authoritative modes.

### Commit 1.1 — Build & portability prerequisites for ENet + Windows headers
Touches:
- `src/Makefile.am` (ENet include selection, git rev define, add new sources incrementally)
- `lib-src/enigma-core/ecl_video.hh` (undef `RGB` macro clash)

Changes:
- Make “system ENet vs vendored ENet headers” selectable without runtime ABI mismatch.
- Avoid `RGB` macro collision on Windows when ENet/SDL pull in `<windows.h>`.

### Commit 1.2 — Determinism prerequisite: locale-independent numeric parsing
Touches:
- `src/Value.cc`

Changes:
- Parse floating point values using the classic locale (avoid `LC_NUMERIC` differences causing cross-platform divergence).

### Commit 1.3 — Introduce deterministic per-tick input queue module (standalone)
Touches:
- `src/input.cc`
- `src/input.hh`
- `tests/test_input.cc`

Changes:
- Add `enigma::input` queue keyed by `(tick, player)` with snapshot/restore hooks (used later by experiments).
- No gameplay wiring yet (keeps review focused and testable).

### Commit 1.4 — Multi-player mouse force plumbing (per-player forces)
Touches:
- `src/world_internal.hh` (MouseForce stores an array of forces)
- `src/world.hh` (`SetMouseForce(unsigned player, ...)` declaration)
- `src/world.cc` (`SetMouseForce(unsigned player, ...)` implementation)

Changes:
- Replace single global mouse-force vector with per-player mouse forces, aggregated by controller masks.

### Commit 1.5 — Server simulation advances in lockstep ticks (variable tick length)
Touches:
- `src/server.cc`

Changes:
- Drive simulation using `input::TickTimestep()` and `input::CurrentTick()`.
- Apply discrete actions once per tick; distribute continuous mouse force into 10ms substeps (keeps large-tick gameplay controllable).

### Commit 1.6 — Player system supports N players in networked games
Touches:
- `src/player.cc`
- `src/player.hh`

Changes:
- Size player/inventory state based on `input::ExpectedPlayers()` while networked.
- Make item pickup/drop inhibition deterministic for multiplayer (tick-based, not SDL mouse-state based).
- Add `player::PlayerCount()` helper.

### Commit 1.7 — Level metadata: declare network player count (XSD + Proxy API)
Touches:
- `data/schemas/level.xsd` (new `players` attribute under `<modes network="...">`)
- `src/lev/Proxy.cc` / `src/lev/Proxy.hh` (`getNetworkPlayers()`)

Changes:
- Allow levels (and UI) to declare “this network level is for N players”.

### Commit 1.8 — Core multiplayer protocol (LAN/direct only; minimal message set)
Touches:
- `src/multiplayer_protocol.hh`
- `tests/test_multiplayer_protocol.cc`

Changes:
- Introduce baseline session packet formats needed for PR1:
  - Welcome/join, input, ready/start, restart, pause/menu, abort, load-level.
- Keep Internet/relay/world-state/ping messages for later commits/PRs (or behind “extend later” commits).

### Commit 1.9 — Direct ENet socket + transport facade (direct only)
Touches:
- `src/multiplayer_enet_socket.cc`
- `src/multiplayer_transport.cc`
- `src/multiplayer_transport.hh`

Changes:
- Provide a unified “send/poll” interface for direct connections.
- Do not add relay sinks yet (keeps PR1 focused).

### Commit 1.10 — LAN lobby discovery (broadcast announce + start token)
Touches:
- `src/multiplayer_lan_lobby.cc`
- `src/multiplayer_config.cc`
- `src/multiplayer_config.hh`
- `src/multiplayer_util.cc`
- `src/multiplayer_identity.cc`
- `src/multiplayer_debug.cc` (numeric IP formatting helper used by LAN joins)

Changes:
- Broadcast lobby presence and discover peers on the local network.
- Encode/decode a start token that includes level + session settings.
- Centralize “effective multiplayer config” resolution used by UI + lobby (env overrides + prefs).

### Commit 1.11 — Session state machine (host/client), minimal lockstep transport
Touches:
- `src/multiplayer_internal.hh`
- `src/multiplayer_globals.cc`
- `src/multiplayer_wait_settings.hh`
- `src/multiplayer_session.hh`
- `src/multiplayer_session_impl.hh`
- `src/multiplayer_session_runtime.cc`
- `src/multiplayer_session_start.cc`
- `src/multiplayer_session_transport.cc`
- `src/multiplayer_state.hh`
- `src/multiplayer.cc`
- `src/multiplayer.hh`

Changes:
- Implement host/client session lifecycle and “defer start until READY/START” barrier.
- Send/receive per-tick inputs and enforce input delay.
- Implement host-authoritative pause/menu-open propagation (no resync yet).

### Commit 1.12 — Game loop integration (defer simulation while joining/waiting)
Touches:
- `src/client.cc`
- `src/client_internal.hh`
- `src/game.cc`
- `src/server.cc` (session callbacks: restart/load-level hooks without Internet)

Changes:
- While connecting/joining, keep SDL pumping + transport ticking but do not advance simulation.
- Ensure abort/restart semantics are session-wide in multiplayer.

### Commit 1.13 — Multiplayer UI: lobby menu + “waiting for players” screen
Touches:
- `src/gui/MultiplayerMenu.cc`
- `src/gui/MultiplayerMenu.hh`
- `src/gui/MultiplayerMenu_actions.cc`
- `src/gui/MultiplayerMenu_common.cc`
- `src/gui/MultiplayerMenu_internal.hh`
- `src/gui/MultiplayerMenu_levels.cc`
- `src/gui/MultiplayerMenu_tick.cc`
- `src/gui/MultiplayerWaitMenu.cc`
- `src/gui/MultiplayerWaitMenu.hh`
- `src/gui/MainMenu.cc`
- `src/gui/Menu.cc` / `src/gui/Menu.hh`

Changes:
- Add entry point from the main menu into the multiplayer lobby.
- Provide UX for waiting barrier and transient disconnect stalls (without “Internet” yet).

### Commit 1.14 — Options UI for lockstep parameters (tick length, delay, zerofill)
Touches:
- `src/gui/OptionsMenu.cc`
- `src/gui/OptionsMenu.hh`
- `src/gui/TextField.cc` / `src/gui/TextField.hh` (locked fields when settings are immutable during a session)

Changes:
- Expose only *session-critical, user-facing* parameters in the Options UI (no full debug matrix).
- Enforce “locked while RUNNING” where applicable (tick length, input delay).

### Commit 1.17 — MP test scripts: lockstep join/start/tick basics
Touches:
- `tools/mp_test_scripts/join_load_level_handshake_before_ready.txt`
- `tools/mp_test_scripts/tick_length_ms_negotiated.txt`
- `tools/mp_test_scripts/tick_length_ms_scales_legacy_ticks.txt`

Changes:
- Add (or move into PR1) scenarios that cover join/start barriers and tick-length/input-delay negotiation semantics.

---

## PR04 (Roadmap 2) — Deviation detection for actors + world (toast: “deviation detected, please restart level”)
Branch: `04_mp_desync_detection`

Target outcome:
- Detect divergence early and reliably.
- Show a user-facing toast instructing restart.
- No automatic resync yet.

### Commit 2.1 — World + actor checksum functions (determinism diagnostics)
Touches:
- `src/world.hh`
- `src/world.cc`

Changes:
- Add `WorldChecksum()` and `ActorChecksum()` helpers (quantized hashing).
- Keep checksum surface explicit (what is hashed, what is not).

### Commit 2.2 — Add periodic NET_SYNC and mismatch classification (no resync actions)
Touches:
- `src/multiplayer_protocol.hh` (add/enable `NET_SYNC` fields used by PR2)
- `src/multiplayer_session_sync.cc` (record local samples; compare to remote)
- `src/multiplayer_internal.hh` (checksum sample history / counters)

Changes:
- Host periodically broadcasts sync samples.
- Clients compare local vs remote and raise “desync detected” state.

### Commit 2.3 — User-facing toast + host-only restart gating
Touches:
- `src/multiplayer_session_sync.cc` (toast trigger point)
- `src/gui/GameMenu.cc` (only host may restart)

Changes:
- Show toast after a short streak threshold to avoid false positives from transient jitter.
- Keep restart initiation host-authoritative.

### Commit 2.4 (optional) — MP Sync debug overlay page (read-only)
Touches:
- `src/display.cc` / `src/display.hh`
- `src/multiplayer_session_runtime.cc` (overlay line builder)

Changes:
- Make drift visible to testers without requiring logs.

### Commit 2.5 — MP test scripts: deviation detection (no resync)
Touches:
- `tools/mp_test_scripts/peroxyd_open_sesame_fast_moves_with_netsim.txt`

Changes:
- Provide a reproducible scenario that triggers drift under netsim and asserts that the run results in a “desync detected” outcome (restart toast / abort policy as implemented for PR2).

---

## PR05 (Roadmap 3) — Automatic resync for actors (smooth small deviations, snap big ones; no rollback+replay)
Branch: `05_mp_actor_resync`

Target outcome:
- Automatically recover from actor drift.
- Use smoothing for visual continuity on small deltas; snap on large.
- Still no rollback/replay; host stays monotonic.

### Commit 3.1 — Protocol: actor resync request/response messages
Touches:
- `src/multiplayer_protocol.hh` (enable `NET_RESYNC_REQUEST`, `NET_RESYNC_STATE`)

Changes:
- Add packet types and encode/decode helpers for actor state snapshots.

### Commit 3.2 — Host builds authoritative actor snapshot; clients request it
Touches:
- `src/multiplayer_session_sync.cc`

Changes:
- Host: build `ResyncState` snapshot (actors + RNG state).
- Client: request snapshots upon drift detection (rate-limited).

### Commit 3.3 — Client applies actor snapshot safely (physics-only teleport)
Touches:
- `src/multiplayer_session_sync.cc`

Changes:
- Apply resync without triggering gameplay side-effects (no floor/item enter/leave).
- Include robust mapping for “multi identical marbles”:
  - object-id match first
  - stable name-hash match second
  - kind/owner/position fallback last

### Commit 3.4 — Render-only smoothing for post-resync teleports
Touches:
- `src/actors.hh` (render state fields)
- `src/actors.cc` (smooth `move_screen()` in multiplayer)
- `src/world.hh` / `src/world.cc` (`RenderFrameDtime()` and wall-clock tracking in `TickFinished`)

Changes:
- Smooth small corrections; snap large ones to avoid drifting through walls.
- Keep simulation determinism untouched (render-only).

### Commit 3.5 — Rubberband resync stabilization hook
Touches:
- `src/others/Rubberband.cc` (handle `_mp_resync_flags`)
- `src/multiplayer_session_sync.cc` (trigger recompute after actor resync)

Changes:
- Recompute rubberband violation flags after teleport to prevent persistent divergence.

### Commit 3.6 (optional) — Host-side periodic resync broadcast (“keep-alive correction”)
Touches:
- `src/multiplayer_session_sync.cc`
- `src/gui/OptionsMenu.cc` (stride option)

Changes:
- Host broadcasts actor snapshots at a configurable stride to reduce drift accumulation.

### Commit 3.7 — MP test scripts: actor resync under jitter/loss
Touches:
- `tools/mp_test_scripts/peroxyd_open_sesame_fast_moves_with_netsim_resync_broadcast.txt`
- `tools/mp_test_scripts/peroxyd_open_sesame_fast_moves_with_netsim_resync_broadcast_delay12.txt`
- `tools/mp_test_scripts/peroxyd_open_sesame_fast_moves_with_netsim_resync_broadcast_predict_missing.txt`

Changes:
- Add (or move into PR3) scenarios that demonstrate actor resync convergence under controlled delay/jitter and optional prediction/zerofill settings.

---

## PR06 (Roadmap 4) — Automatic resync for world elements (recover all desyncs; no remaining “restart needed”)
Branch: `06_mp_world_resync`

Target outcome:
- World-state divergence becomes recoverable via authoritative snapshots.
- By the end of PR4, the “restart level” toast should be effectively obsolete for deterministic play.

### Commit 4.1 — Protocol: world snapshot messages + request messages
Touches:
- `src/multiplayer_protocol.hh` (enable `NET_WORLD_STATE`, `NET_WORLD_STATE_REQUEST`)

Changes:
- Define `WorldStatePacket` with:
  - per-cell floor/stone/item “state”
  - movable stone positions (optional extension)
  - oxyd colors (optional extension)
  - authoritative kind dictionary + per-cell kind indices (optional extension)

### Commit 4.2 — World-layer checksums (kind/state/movable) and sync extensions
Touches:
- `src/world.hh` / `src/world.cc` (grid checksums + combiner)
- `src/multiplayer_protocol.hh` (SyncPacket fields for grid checksums)
- `src/multiplayer_session_sync.cc` (populate and compare the hierarchical checksums)

Changes:
- Make “world drift” distinguishable from “actor drift”.
- Provide the detection surface that drives world snapshot requests.

### Commit 4.3 — Host builds authoritative world snapshot
Touches:
- `src/multiplayer_session_sync.cc` (snapshot builder + broadcast)
- `src/world.hh` / `src/world.cc` (helpers like `GetRubberbands` if needed by snapshot logic)

Changes:
- Serialize authoritative world grid into packet format.

### Commit 4.4 — Client applies authoritative world snapshot safely
Touches:
- `src/multiplayer_session_transport.cc` (stable kind reconstruction helper)
- `src/multiplayer_session_sync.cc` (apply logic)
- `src/stones/OxydStone.cc` / `src/stones/OxydStone.hh` (force external state + oxydcolor)

Changes:
- Apply per-tile states without re-triggering non-idempotent gameplay logic.
- Fix oxyd/puzzle-stone determinism issues by forcing authoritative identities.

### Commit 4.5 — Automatic world resync trigger (streak-based, request + cooldown)
Touches:
- `src/multiplayer_session_sync.cc`
- `src/gui/OptionsMenu.cc` (threshold option)

Changes:
- Request world snapshots after repeated “world-only” divergence.
- Provide “syncing from host…” toast while the request is in flight.

### Commit 4.6 — Remove/retire remaining “restart required” cases (goal: none)
Touches:
- `src/multiplayer_session_sync.cc`

Changes:
- Ensure all previously observed world divergence cases are recoverable via PR4 snapshots.

### Commit 4.7 — MP test scripts: authoritative world-state recovery
Touches:
- `tools/mp_test_scripts/world_state_reorder_smoke.txt`

Changes:
- Add (or move into PR4) scenarios that validate world snapshot request/apply behavior and convergence after a forced reorder/divergence.

---

## PR07 (Roadmap 5) — Add back Internet mode with UDP relay
Branch: `07_mp_internet_udp_relay`

Target outcome:
- Internet room-code lobby (server-assisted).
- UDP relay transport for NAT traversal and networks that block direct UDP.
- Keep the *abstractions* compatible with a future TCP relay, but **do not implement TCP relay yet**.

### Commit 5.1 — Persisted multiplayer Internet configuration (prefs + schema + UI)
Touches:
- `data/schemas/enigmarc.xml` (lobby server + port keys)
- `src/gui/OptionsMenu.cc` / `src/gui/OptionsMenu.hh`
- `src/gui/TextField.cc` / `src/gui/TextField.hh` (locked fields support already added in PR1)

Changes:
- Add persistent settings: lobby server host, enable/disable direct vs UDP relay, UDP relay port.

### Commit 5.2 — Relay framing codec (shared by UDP + future TCP relay)
Touches:
- `src/multiplayer_relay_codec.cc`

Changes:
- Define relay framing so session code doesn’t branch on transport at every call site.

### Commit 5.3 — UDP relay transport path (client↔relay↔host)
Touches:
- `src/multiplayer_transport.cc` / `src/multiplayer_transport.hh` (add UDP relay sink)
- `src/multiplayer_session_transport.cc` (routing to/from relay)

Changes:
- Add relay send/poll integration and transport selection order (direct → UDP relay).

### Commit 5.4 — Internet room-code lobby client
Touches:
- `src/multiplayer_internet_lobby.cc`
- `src/multiplayer.hh` / `src/multiplayer.cc` (API exposure)

Changes:
- Create/join/start/poll/leave room operations.
- Track room so Enigma can leave it on shutdown.

### Commit 5.5 — Internet lobby server tool (Python)
Touches:
- `tools/internet_lobby_server.py`

Changes:
- Implement room allocation, join/poll/start state, host discovery data.

### Commit 5.6 — UDP relay server tool (C++)
Touches:
- `tools/relay_server.cc`

Changes:
- ENet-based UDP relay for session payload forwarding.

### Commit 5.7 — Docker deployment for lobby+relay services
Touches:
- `Dockerfile`
- `tools/docker-entrypoint.sh`

Changes:
- One-container deployment for the lobby + relay services.

### Commit 5.8 — MP test scripts: Internet lobby + UDP relay happy-path
Touches:
- `tools/mp_test_env.py` (only if additional wiring is needed to spawn the services)
- `tools/mp_test_scripts/basic_join_and_move.txt` (extend to cover relay mode) 

Changes:
- Ensure the harness can exercise “room-code lobby + UDP relay session” and produce a reproducible smoke run (even if it’s marked developer-only / manual until CI exists).

---

## PR08 (Roadmap 6) — Add back latency measurement + latency-dependent profiles (tick length + delay)
Branch: `08_mp_latency_profiles`

Target outcome:
- Measure connectivity (RTT distribution, p90).
- Expose “profiles” that currently only set:
  - lockstep tick length
  - lockstep input delay

### Commit 6.1 — Protocol: ping/pong packets
Touches:
- `src/multiplayer_protocol.hh` (enable `NET_PING`, `NET_PONG`)

Changes:
- Add lightweight RTT probes (host↔clients).

### Commit 6.2 — Session transport: collect RTT samples + runtime display values
Touches:
- `src/multiplayer_session_transport.cc`
- `src/multiplayer_internal.hh` (buffers/counters)

Changes:
- Maintain per-peer RTT histories (auto-detect) and latest RTT (runtime HUD/overlay).

### Commit 6.3 — Auto-detect state machine (host side) and p90 classification
Touches:
- `src/multiplayer_session_runtime.cc`

Changes:
- Run probes before starting the session; compute worst-link p90 RTT; select a profile id.

### Commit 6.4 — Connectivity presets (v1: tick length + delay only) + Options UI
Touches:
- `src/multiplayer_connectivity_presets.hh` (initially only `tick_ms` + `input_delay_legacy_ticks` are considered “stable”)
- `src/gui/OptionsMenu.cc` (show/apply profile name; avoid exposing raw debug knobs)

Changes:
- Keep presets minimal for PR6; do not enable host-authoritative or client-authoritative switches yet.
- UI shows the chosen profile and “auto-detect” toggle; it does not need to expose the underlying per-flag settings.

### Commit 6.5 — Visual polish: connectivity color fonts
Touches:
- `data/models-16.lua`
- `data/models-32.lua`
- `data/models-40.lua`
- `data/models-48.lua`
- `data/models-64.lua`

Changes:
- Add fonts used to color-code “Good/Normal/Bad” connectivity labels in UI.

### Commit 6.6 (optional) — In-game overlay: show latency + selected profile
Touches:
- `src/display.cc` / `src/display.hh`
- `src/multiplayer_session_runtime.cc`

Changes:
- Show current RTT and profile in the F8 overlay for testers.

### Commit 6.7 — MP test scripts: latency probe + profile selection
Touches:
- `tools/mp_test_scripts/basic_join_and_move_high_latency.txt`

Changes:
- Add (or move into PR6) a scenario that runs with controlled netsim delay/jitter and verifies that probing completes and the selected profile is applied.

---

## PR09 (Roadmap 7) — High-latency mode v1: host-authoritative world, client inputs forwarded
Branch: `09_mp_host_world_remote_ball`

Target outcome:
- For high latency, stop discarding too many inputs in lockstep.
- Run world interactions only on host; clients receive world updates and actor snapshots/events.
- First step: client inputs still sent; client balls also controlled by host snapshots.

### Commit 7.1 — Client-side suppression of world interactions (host-only world)
Touches:
- `src/world.cc` (suppress stone hit/touch side effects when configured)
- `src/actors.cc` (suppress floor/item enter/leave side effects when configured)
- `src/gui/OptionsMenu.cc` (toggle exposure)

Changes:
- When enabled and not host: keep local simulation running but do not mutate world state from interactions.

### Commit 7.2 — Host periodic world-state broadcast (keep clients visually aligned)
Touches:
- `src/multiplayer_session_sync.cc`
- `src/gui/OptionsMenu.cc` (stride option)

Changes:
- Broadcast authoritative world snapshots at a stride tuned for high-latency play.

### Commit 7.3 — “Remote-control local ball” mode (host drives client-controlled balls)
Touches:
- `src/multiplayer_session_transport.cc` (suppress local continuous control; apply host snapshots frequently)
- `src/multiplayer_session_sync.cc` (ensure local actor is not skipped during resync)
- `src/gui/OptionsMenu.cc`

Changes:
- In this mode, clients become mostly renderers for balls; host state is the source of truth.

### Commit 7.4 — MP test scripts: host-world-only + remote-control mode
Touches:
- `tools/mp_test_scripts/remote_control_local_ball_basic.txt`
- `tools/mp_test_scripts/remote_control_local_ball_netsim_zickzack.txt`
- `tools/mp_test_scripts/host_ball_moves_in_host_sim.txt`

Changes:
- Add (or move into PR7) scenarios that validate “host-only world interactions” and the remote-control mode under latency/jitter.

---

## PR10 (Roadmap 8) — High-latency mode v2: client-authoritative local ball simulation (send positions)
Branch: `10_mp_client_auth_ball_pos`

Target outcome:
- Clients simulate their own local ball positions and send positions/velocities to host.
- Host uses those remote-controlled balls for world interactions; clients still receive authoritative world updates.

### Commit 8.1 — Protocol: client-authoritative owner actor state
Touches:
- `src/multiplayer_protocol.hh` (enable `NET_OWNER_ACTOR_STATE`)

Changes:
- Add packet format that carries `(tick, player, actor identity hints, pos, vel)`.

### Commit 8.2 — Client sends owner actor state each tick (when enabled)
Touches:
- `src/multiplayer_session_transport.cc`

Changes:
- Send local actor state to host; keep it unreliable and monotonic.

### Commit 8.3 — Host applies owner actor state to its copy; broadcasts to other clients
Touches:
- `src/multiplayer_session_transport.cc`

Changes:
- Best-effort match incoming packet to a controlled steerable actor.
- Apply bounded corrections to avoid destabilizing movable-stone pushing.

### Commit 8.4 — MP test scripts: client-authoritative ball position
Touches:
- `tools/mp_test_scripts/peroxyd_open_sesame_client_ball_moves_in_host_sim_netsim.txt`
- `tools/mp_test_scripts/peroxyd_open_sesame_client_drop_item_visible.txt`

Changes:
- Add (or move into PR8) scenarios that validate that the host consumes client-authoritative state for world interactions and that key visible effects (e.g., item drops) remain consistent.

---

## PR11 (Roadmap 9) — Auto-select lockstep vs host-authoritative based on measured latency
Branch: `11_mp_auto_mode_select`

Target outcome:
- Use measured connectivity to pick between:
  - lockstep (good/normal)
  - host-authoritative world + client-authoritative ball (bad)

### Commit 9.1 — Connectivity presets v2: include mode switches + resync/world strides
Touches:
- `src/multiplayer_connectivity_presets.hh`
- `src/gui/OptionsMenu.cc`

Changes:
- Extend presets to set:
  - `MultiplayerDebugHostOnlyWorldInteractions`
  - `MultiplayerDebugClientAuthBallPos`
  - (optionally) `MultiplayerDebugRemoteControlLocalBall`
  - host resync/world broadcast strides
- Keep these switches driven by profiles/auto-detect (not a permanent “debug menu” surface unless explicitly desired).

### Commit 9.2 — Host auto-detect applies presets and broadcasts authoritative session settings
Touches:
- `src/multiplayer_session_runtime.cc`
- `src/multiplayer_session_transport.cc` (debug options broadcast)

Changes:
- Host picks a preset after RTT probing and makes it authoritative for the session.
- Clients treat these settings as immutable during RUNNING.

### Commit 9.3 — UX: show selected mode/profile clearly
Touches:
- `src/multiplayer_session_runtime.cc`
- `src/display.cc` / `src/display.hh`
- `src/gui/MultiplayerMenu_common.cc` (if profile shown in lobby)

Changes:
- Display the chosen mode (lockstep vs host-world/client-ball) and the connectivity profile name.

### Commit 9.4 — MP test scripts: mode switching cutoff
Touches:
- `tools/mp_test_scripts/basic_join_and_move.txt` (low-latency baseline)
- `tools/mp_test_scripts/basic_join_and_move_high_latency.txt` (high-latency baseline)

Changes:
- Provide two scenarios that land on opposite sides of the preset cutoffs and validate that the session selects the intended mode/profile.

---

## Optional future PRs (post-PR11)

These are useful, but should not be part of `01_mp_lockstep_lan` even if already implemented on the experimental branch.

### PR12 — Extra players on low-player maps (non-optimized mode)
Branch: `12_mp_extra_players`
Goal:
- Make low-player or single-player-authored maps playable with more session players by spawning/placing extra actors and/or rebalancing control.

Commits (suggested):
1. **Extra actors + placement protocol**
   - Touches:
     - `src/multiplayer_extra_players.cc`
     - `src/multiplayer_extra_players.hh`
     - `src/multiplayer_protocol.hh` (if placement packets are introduced/extended here)
2. **Deterministic multi-ball rebalance helper + test**
   - Touches:
     - `src/multiplayer_ball_assignment.hh`
     - `tests/test_multiplayer_ball_assignment.cc`
3. **Integration call sites**
   - Touches:
     - `src/server.cc` (`PrepareExtraActors`, `SetupExtraPlayerStartPositions`)
     - `src/player.cc` (if additional player-count integration is still needed)

### PR13 — Add simple MP test levels
Branch: `13_mp_test_levels`
Goal:
- Provide tiny levels for manual sanity checks that match the harness scenarios.

Commits (suggested):
1. **Add MP test levels**
   - Touches:
     - `data/levels/enigma_experimental/index.xml`
     - `data/levels/enigma_experimental/mp_test_3p.xml`
     - `data/levels/enigma_experimental/mp_test_4p.xml`

---

## Out-of-roadmap / optional PRs (end)

### PR98 (optional) — Developer-only debug UI/overlay
Branch: `98_mp_dev_overlay`
**Goal:** Preserve experimental instrumentation without committing to a long-term “debug menu” UX. Only do this if maintainers explicitly want it.

Commits (if needed):
1. **In-game stats overlay plumbing (F8 pages)**
   - Touches:
     - `src/display.cc`
     - `src/display.hh`
     - `src/multiplayer_session_runtime.cc`
2. **Expose dev-only toggles in Options (optional)**
   - Touches:
     - `src/gui/OptionsMenu.cc`
     - `src/gui/OptionsMenu.hh`

### PR99 (optional) — Rollback+replay experiment (deferred / out-of-roadmap)
Branch: `99_mp_rollback_replay_experiment`
**Goal:** Keep rollback/replay completely out of the main staged roadmap. If maintainers ever want rollback/replay later, land it as a separate, explicitly experimental PR series.

Recommendation:
- Do **not** merge this before PR03–PR06 (and likely not at all unless there is strong demand).
- Treat it as a “parking lot” series that is easy to review in isolation (or keep it unmerged on a long-lived experiment branch).

Commits (suggested isolation, if ever pursued):
1. **Snapshot/restore primitives**
   - Touches:
     - `src/multiplayer_sim_snapshot.cc`
     - `src/multiplayer_sim_snapshot.hh`
     - `src/StateObject.hh`
     - `src/timer.cc`
     - `src/timer.hh`
     - `src/world.hh` (snapshot APIs)
     - `src/world.cc` (snapshot APIs)
2. **Rollback engine + hooks**
   - Touches:
     - `src/multiplayer_rollback.cc`
     - `src/multiplayer_rollback.hh`
     - `src/server.cc` (rollback hooks like `MaybeRollback`, `OnBeforeSimTick`)
     - `src/actors/Balls.cc` / `src/actors/Balls.hh` (APPEARING fix after restore)

## Appendix: “where do the remaining touched files land?”

This is a quick index for files that don’t fit cleanly into the numbered PRs above.

- Portability / robustness:
  - `lib-src/enigma-core/ecl_font.cc` (font loading fallback; can be PR0A or PR1.1)
  - `src/world.hh` (undef `SendMessage` macro; can be PR1.1)
  - `src/Makefile.am` (`ENIGMA_GIT_REV`; can be PR0B or PR1.1)
- UI/overlay & debugging ergonomics (often best as follow-ups):
  - `src/display.cc`, `src/display.hh` (MP overlay)
  - `src/gui/LevelPackMenu.cc`, `src/gui/LevelPackMenu.hh`, `src/gui/LevelWidget.cc` (pack/level selection polish)
  - `src/video.cc`, `src/MouseCursor.cc` (cursor/focus behavior for MP)
