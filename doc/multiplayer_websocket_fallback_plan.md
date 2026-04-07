# Multiplayer WebSocket Fallback Plan

This note describes what would have to change to turn the current raw TCP
relay fallback into a WebSocket-based fallback that can traverse normal HTTPS
proxies.

It is based on the current source layout, plus the official libcurl
documentation because libcurl is already a required dependency in this tree and
is the most obvious client-side implementation candidate. The current branch
implementation ended up using manual HTTP Upgrade over libcurl
`CONNECT_ONLY` on `http://` / `https://` URLs instead of libcurl's native
`ws://` URL mode, because the system libcurl in this environment exposes the
headers but does not advertise runtime `ws` / `wss` protocol support.

## Current Branch Status

Status on April 7, 2026:

- step 1 is complete:
  - direct gameplay regression coverage is in place
  - UDP-relay gameplay regression coverage is in place
  - Internet-lobby room create/join/start/poll regression coverage is in place
- step 2 is complete:
  - resolved endpoint objects now exist for lobby / UDP relay / TCP relay
  - session-start and multiplayer-menu code now consume those resolved endpoint
    objects instead of rebuilding and reparsing `host:port` strings at each call site
- step 3 is complete:
  - the current UDP Internet-lobby path now sits behind an internal backend seam
  - the public `Internet*Room` API is now a facade instead of being the UDP implementation itself
- step 4 is complete:
  - the raw TCP relay socket/rx/frame state now sits behind a dedicated
    stream-relay helper layer
  - session start, runtime, and transport code now target that seam instead
    of open-coding raw TCP relay operations
  - the existing direct, UDP-relay, and Internet-lobby regression scripts all
    still pass after the refactor
- step 5 is complete:
  - host-side UDP-relay and TCP-relay player bookkeeping now lives in one
    shared relay-route table instead of duplicated player/ready maps
  - runtime, transport, sync, and regression-driver code now query that shared
    route layer instead of maintaining transport-specific bookkeeping loops
  - the existing direct, UDP-relay, and Internet-lobby regression scripts all
    still pass after the cleanup
- step 6 is complete:
  - the WebSocket relay config/UI surface is in place
  - the actual client-side WebSocket fallback now works through the shared
    stream-relay seam
  - the client implementation uses libcurl HTTP(S) `CONNECT_ONLY` plus a
    manual RFC6455 Upgrade / frame layer instead of depending on libcurl native
    `ws://` protocol support
  - a local WebSocket relay regression helper exists and the new
    `ws_relay_join_and_move` script passes
  - Docker/runtime wiring now includes the Python WebSocket relay helper as a
    normal service for VM testing and redeploys
  - the existing direct, UDP-relay, and Internet-lobby regression scripts still
    pass after adding the new fallback path
- step 7 is complete:
  - the room-code Internet-lobby path now has an HTTP(S)-compatible control
    backend in addition to the existing UDP path
  - the client can fall back from UDP lobby traffic to an explicit control URL
    without spreading transport conditionals into menu/session code
  - the Python lobby server now serves both UDP and HTTP on the shared room state
  - the local `internet_lobby_http_room_flow` regression passes
  - the local `internet_http_ws_fallback_join_and_move` regression passes,
    covering the full forced fallback chain
  - Docker/runtime wiring now exposes the lobby control backend on TCP as well
- next active slice:
  - external VM / reverse-proxy validation of `https://.../lobby` plus
    `wss://.../relay`
  - decide whether the current Python WebSocket relay should stay as the long-term
    deployed backend or be replaced by a more production-focused implementation

## Goal

- Keep the existing fast paths:
  - direct ENet/UDP for LAN and for Internet play when direct-connect works
  - UDP relay for Internet play when UDP is available but direct-connect is not
- Replace the current raw TCP relay fallback with a WebSocket fallback.
- Make that fallback usable through ordinary HTTPS proxies.

Important scope clarification:

- Replacing only the gameplay transport is not enough to make current
  Internet-mode room-code multiplayer work on proxy-only networks.
- The room-code lobby path is currently UDP as well, so a true
  "works-everywhere" path also needs an HTTPS-compatible lobby/control channel.

## Current Shape In Tree

### Existing transport ladder

The client currently tries transports in this order:

- direct
- UDP relay
- TCP relay

Relevant files:

- `src/multiplayer_session_start.cc`
- `src/multiplayer_transport.hh`
- `src/multiplayer_transport.cc`
- `src/multiplayer_session_transport.cc`
- `src/multiplayer_session_runtime.cc`
- `src/multiplayer.hh`

### Relay protocol layering

The game-session payload protocol is already transport-agnostic.

- `src/multiplayer_relay_codec.cc` defines the relay header used by both UDP
  relay and TCP relay.
- `src/multiplayer_transport.cc` hides most direct-vs-relay send/poll details
  behind one facade.
- `src/multiplayer_session_transport.cc` only branches where host/client
  routing really differs.

This is the main reason the change is feasible without redesigning the whole
session protocol.

### Current TCP relay implementation

The TCP fallback consists of:

- `src/multiplayer_tcp_socket.cc`
  - raw nonblocking TCP connect/send/recv helpers
  - 4-byte little-endian frame prefix
- `tools/tcp_relay_server.cc`
  - host/client session table
  - host registration via `RELAY_HELLO_HOST`
  - client registration via `RELAY_HELLO_CLIENT`
  - connect/disconnect notifications
  - forwarding of framed relay packets

### Current Internet lobby path

The current room-code Internet lobby is UDP-only.

- `src/multiplayer_internet_lobby.cc`
  - `internet_exchange()`
  - `internet_poll_start()`
  - `internet_poll_pump()`
- `tools/internet_lobby_server.py`
  - UDP request/response server for create/join/poll/leave/start

This is the critical non-obvious blocker for the "works everywhere through
HTTPS proxies" goal.

### Current configuration surface

The multiplayer config currently assumes:

- one shared host field
- separate numeric ports for lobby / UDP relay / TCP relay

Relevant files:

- `src/multiplayer_config.hh`
- `src/multiplayer_config.cc`
- `src/gui/OptionsMenu.cc`
- `data/schemas/enigmarc.xml`

That shape works for UDP + raw TCP on sibling ports, but it is too narrow for
a realistic public WebSocket fallback, which will usually want a full
`wss://host[:port]/path` URL and may not share the same host/port pattern as
the UDP services.

### Build and deployment reality today

The relay tools are not normal autotools targets today.

- `tools/Makefile.am` does not build the relay servers.
- `Dockerfile` compiles `tools/relay_server.cc` and `tools/tcp_relay_server.cc`
  directly.
- `tools/docker-entrypoint.sh` starts the Python lobby, UDP relay, and TCP
  relay.

So the WebSocket server choice is also a build/deployment decision, not only a
transport decision.

## Key Conclusions

1. The gameplay session protocol can stay mostly unchanged.
   - Reuse the existing relay header and existing game payloads.
   - Only the outer transport/framing changes.

2. The WebSocket fallback should use binary messages.
   - The simplest mapping is: one current relay packet becomes one WebSocket
     binary message.
   - The raw TCP 4-byte length prefix should not survive on the WebSocket wire.

3. The public fallback endpoint should be `wss://`, not plain `ws://`.
   - The HTTPS-proxy requirement effectively implies TLS-wrapped WebSocket.

4. A WebSocket gameplay fallback alone does not deliver full proxy-only
   Internet play.
   - The room create/join/poll/leave path also needs an HTTPS-compatible
     fallback.

5. Client side can stay on libcurl, but should not assume native `ws://`
   protocol support is available at runtime.
   - A manual HTTP Upgrade over libcurl-managed HTTP(S) sockets is a workable
     fallback and still preserves TLS + proxy handling for `wss://`.
   - Server side: there is still no existing in-tree production WebSocket
     server stack, so that runtime choice remains explicit.

## Recommended Direction

### Client side

Use libcurl for the WebSocket client path.

Why this is attractive in this tree:

- `configure.ac` already requires libcurl.
- `src/main.cc` already calls `InitCurl()`.
- No separate client-side TLS or proxy stack needs to be introduced.
- libcurl already has the HTTP(S), TLS, proxy, and `CONNECT_ONLY` pieces needed
  for a proxy-friendly `wss://` transport.

Recommended transport shape:

- resolve `ws://` / `wss://` config as WebSocket URLs at the config layer
- internally map them to `http://` / `https://` connect-only URLs
- use libcurl to establish the TCP/TLS/proxy-managed socket
- perform the RFC6455 Upgrade handshake in the multiplayer stream-relay layer
- send one relay packet as one masked WebSocket binary message
- reassemble incoming fragmented WebSocket messages before handing them to the
  existing relay codec

This avoids depending on libcurl native `ws://` protocol support being present
in the linked runtime library while still keeping proxy and TLS handling inside
libcurl.

### Server side

Do not force the relay backend to own TLS certificates directly unless there is
a strong reason to do so.

Recommended deployment model:

- run a plain WebSocket relay backend internally
- expose it as `wss://...` through a reverse proxy on port 443
- let the reverse proxy handle TLS termination and HTTP Upgrade

That reduces certificate-management complexity and is the most practical way to
fit the HTTPS-proxy requirement into the current deployment shape.

### Migration strategy

Implement this in two layers:

1. WebSocket gameplay relay fallback
2. HTTPS-compatible Internet-lobby control fallback

If only step 1 is done, the result is still useful for manually bootstrapped
sessions or future alternate bootstrap flows, but it is not yet a full
"works-everywhere Internet mode" replacement.

## Required Abstractions

The implementation should not grow by adding another round of
`if (udp) ... else if (tcp) ... else if (ws) ...` in every call site.

The right goal is:

- transport-specific branching stays local to transport code
- lobby/control branching stays local to lobby/control code
- configuration parsing stays local to configuration code
- session logic continues to consume a small stable facade

### Separate "session payload transport" from "room-code control channel"

These are different concerns and should not share ad-hoc conditionals.

Recommended split:

- `ISessionTransport` or equivalent existing facade evolution
  - direct / UDP relay / WebSocket relay for gameplay payloads
- `IInternetLobbyClient`
  - create room
  - join room
  - poll room
  - start room
  - leave room

Concretely, `src/multiplayer_transport.cc` and
`src/multiplayer_internet_lobby.cc` should evolve independently instead of
trying to multiplex both problem domains through one shared pile of flags.

### Add a dedicated stream-relay abstraction

The current raw TCP relay and the proposed WebSocket relay are both
stream-oriented fallback links that sit underneath the existing relay codec.

That suggests an internal abstraction such as:

- `IRelayStreamLink`
  - `BeginConnect(...)`
  - `Poll(...)`
  - `SendRelayPacket(...)`
  - `Close()`
  - `IsConnected()`

Implementations:

- `TcpRelayStreamLink`
- `WebSocketRelayStreamLink`

This keeps the relay codec and higher-level host/client registration logic in
one place while isolating the raw-socket vs libcurl-WebSocket mechanics.

Important practical point:

- this does not need to be a large inheritance hierarchy if that would feel too
  heavy for the codebase
- a small state object plus function table would also be fine
- the key requirement is one local seam, not a specific OO style

### Introduce a lobby-control backend abstraction

The Internet room-code layer should stop knowing whether it is talking over UDP
or HTTPS.

Recommended surface:

- `InternetLobbyBackend`
  - `Exchange(...)` for one-shot request/response operations
  - optional `BeginPoll(...)` / `PumpPoll(...)` if the async polling model is
    kept

Implementations:

- `UdpLobbyBackend`
- `HttpLobbyBackend`

Then `InternetCreateRoom()`, `InternetJoinRoom()`, `InternetPollRoom()`, and
friends can become thin orchestration wrappers around one backend interface
instead of hardcoding UDP packet handling directly.

### Make endpoint resolution a first-class object

Right now the config shape is host + ports, which encourages transport-specific
string assembly at the call sites.

Replace that with one resolved endpoint bundle, for example:

- `InternetEndpoints`
  - `lobby_udp`
  - `lobby_https`
  - `udp_relay`
  - `ws_relay`

Then:

- Options/config code owns parsing and validation
- UI owns editing
- runtime code only consumes already-resolved endpoint objects

That keeps `host + port + path + scheme` churn out of session-start and menu
logic.

### Make client connection attempts table-driven

`src/multiplayer_session_start.cc` already has the beginning of a strategy
list, but the actual connect code still branches hard by transport type.

That should move further toward:

- one `ConnectStrategySpec`
  - kind
  - enabled predicate
  - endpoint supplier
  - begin-attempt callback
  - poll-attempt callback

This does not need to become a framework. A compact table-driven layer is
enough. The point is that adding `WS_RELAY` should mean adding one strategy
entry, not editing many separate fallback ladders by hand.

### Stop duplicating relay player bookkeeping by transport

Today there are separate maps such as:

- `relay_players` / `relay_ready`
- `tcp_relay_players` / `tcp_relay_ready`

If WebSocket is added naively, this becomes another full copy.

Recommended end-state:

- one remote-route record per non-direct remote player
- route carries:
  - transport kind
  - relay-side remote id
  - ready state

That would let host-side logic answer questions like:

- who owns player N?
- how do I send to that remote?
- did that remote report READY?

without duplicating the same loops for each relay flavor.

This is a slightly larger cleanup, but it is the main thing that will keep
`src/multiplayer_session_transport.cc` maintainable if more than one relay
fallback exists.

### Keep transport-specific policy in one place

Values like input delay, resend back-window, latency text, and user-visible
transport names should not keep spreading through `switch (TransportKind)`.

Preferred direction:

- one transport policy/helper layer
  - display name
  - input-delay profile
  - resend profile
  - latency-label behavior

Then the rest of the code asks for policy by transport kind instead of open
coding more transport switches.

## Refactoring Order

To avoid churn and regressions, the abstraction work should come before the
protocol migration.

Recommended order:

1. promote current direct/UDP-relay coverage into explicit regression tests
2. introduce endpoint and backend abstractions without changing behavior
3. hide current UDP lobby behind the new lobby backend seam
4. hide current TCP relay behind the new stream-relay seam
5. simplify duplicated relay bookkeeping enough that a third fallback path is
   cheap to add
6. add WebSocket relay as a new backend implementation
7. add HTTPS lobby/control as a new backend implementation
8. remove raw TCP once WebSocket is stable

## Implementation Steps

### 1. Promote current direct/UDP behavior into regression tests first

Before refactoring transport or lobby code, make sure the currently working
paths are protected by tests that can be rerun unchanged afterwards.

Priority for this stage:

- keep or strengthen existing direct-connect gameplay smoke coverage
- add or strengthen UDP-relay gameplay smoke coverage
- add room create/join/start/poll coverage for the current UDP Internet-lobby
  path
- make these tests backend-agnostic at the harness level where possible, so the
  same gameplay assertions can later be reused for WebSocket relay runs

Important clarification:

- the goal is behavioral preservation, not preserving exact call structure or
  test-driver API compatibility
- if the abstraction work makes it sensible to reshape helpers, harness calls,
  or test setup structure, the tests should be updated accordingly
- what must stay stable is the covered functionality and the expected outcomes,
  not the old test invocation details

The purpose of this stage is characterization:

- prove that the refactor did not break today's paths
- give the new abstraction seams a safety net
- avoid discovering much later that the WebSocket work regressed the old fast
  paths

### 2. Introduce the abstraction seams first

Before changing protocols, add the maintainability seams above.

Minimum required first pass:

- endpoint bundle object for resolved lobby/relay endpoints
- lobby backend seam around UDP room-code control
- stream-relay seam around the current raw TCP relay fallback
- table-driven client connect strategy list

This step should preserve current behavior while reducing the amount of
transport-specific branching in:

- `src/multiplayer_session_start.cc`
- `src/multiplayer_internet_lobby.cc`
- `src/multiplayer_session_transport.cc`

### 3. Decide the public configuration model

Before code changes, settle the externally visible shape:

- Do we keep one shared "Lobby/Relay server" host field?
- Or do we add a dedicated WebSocket relay URL field?
- Do we keep the current TCP relay option during migration?
- Do we rely on proxy environment variables first, or add explicit proxy
  settings in the UI?

Recommended first cut:

- keep existing lobby host + UDP relay ports for the current fast paths
- add a dedicated `WebSocket relay URL` option for the fallback path
- keep TCP relay only as a temporary development option until WebSocket relay
  is proven

Likely touch points:

- `data/schemas/enigmarc.xml`
- `src/multiplayer_config.hh`
- `src/multiplayer_config.cc`
- `src/gui/OptionsMenu.cc`
- `src/gui/MultiplayerMenu_actions.cc`

### 4. Add a WebSocket client wrapper

Implement the client WebSocket wrapper inside the existing stream-relay seam
instead of adding transport-specific logic back into session/runtime code.

Current branch implementation:

- `src/multiplayer_stream_relay.cc`
- `src/multiplayer_internal.hh`

Responsibilities:

- open a `wss://` connection
- perform the HTTP Upgrade handshake
- expose connect / send / recv / close operations through the existing
  stream-relay helper API
- assemble fragmented incoming WebSocket messages into complete relay packets
- translate close/error conditions into the same disconnect model the session
  code already expects
- honor proxy and TLS verification settings

Important implementation detail:

- Do not reuse the global download easy handle from `src/file.cc`.
- Create a dedicated easy handle per multiplayer WebSocket session.
- Do not assume libcurl native `ws://` support is available just because the
  WebSocket headers compile. The current branch had to fall back to manual
  Upgrade over HTTP(S) `CONNECT_ONLY`.

### 5. Thread the WebSocket path through session state and transport enums

There are two viable strategies:

- temporary additive strategy:
  - add `TransportKind::WS_RELAY`
  - add `HostSource::WS_RELAY`
  - keep `TCP_RELAY` until the WebSocket path is stable
- direct replacement strategy:
  - rename the TCP-relay concepts in place
  - delete raw TCP as part of the same branch

Recommended plan:

- start additive
- switch over once the WebSocket path is tested
- remove raw TCP afterwards

Likely touch points:

- `src/multiplayer.hh`
- `src/multiplayer_internal.hh`
- `src/multiplayer_transport.hh`
- `src/multiplayer_transport.cc`
- `src/multiplayer_session_transport.cc`
- `src/multiplayer_session_runtime.cc`
- `src/multiplayer_session_sync.cc`
- `src/multiplayer_debug.cc`
- `src/gui/MultiplayerMenu_tick.cc`

### 6. Reuse the existing relay codec, change only the outer framing

Keep:

- `RELAY_HELLO_HOST`
- `RELAY_HELLO_CLIENT`
- `RELAY_CLIENT_CONNECT`
- `RELAY_CLIENT_DISCONNECT`
- `RELAY_SEND`
- `RELAY_DATA`

Keep:

- the inner game payloads exactly as they are

Change:

- raw TCP frame prefixing
- socket connect/send/recv plumbing

Recommended wire mapping:

- one relay packet from `encode_relay_header(...) + payload`
- becomes one WebSocket binary message

That keeps the gameplay/session logic almost unchanged and avoids unnecessary
protocol churn.

### 7. Update host/client join flow and fallback ordering

Current join logic is in `src/multiplayer_session_start.cc`.

Required changes:

- host must be able to register with the WebSocket relay
- client must be able to connect to the WebSocket relay and wait for `WELCOME`
- async join state machine must include a WebSocket connect/welcome phase
- fallback order becomes:
  - direct
  - UDP relay
  - WebSocket relay

Initially, the WebSocket path can reuse the current TCP-relay tuning:

- `kInputDelayTcpRelay`
- `kInputBundleBackTicksTcpRelay`
- `kInputBundleMaxCountTcpRelay`

These should probably be renamed later to something like `...StreamRelay` or
`...FallbackRelay` once raw TCP is gone.

Current branch note:

- `WELCOME` waiting is still handled by the async join poller
- the current WebSocket attempt performs the connect + HTTP Upgrade step as one
  bounded operation when the WebSocket fallback attempt starts
- if UI freeze during that step turns out to be noticeable, the connect stage
  can be moved back into a fully nonblocking state machine later without
  changing the higher-level abstraction seams

### 8. Implement the WebSocket relay backend

The current `tools/tcp_relay_server.cc` is already a good behavioral template.

The new backend needs to preserve:

- session table by `session_id`
- one host registration per session
- monotonically assigned relay `client_id`
- host notifications when clients connect/disconnect
- forwarding host-to-client and client-to-host relay packets

But it must speak WebSocket instead of raw TCP.

This step has an explicit runtime/library decision:

- C++ WebSocket server library
- Python WebSocket service
- some other backend runtime

The repository structure does not currently force one answer.

Current branch note:

- `tools/ws_relay_server.py` now exists as a local regression helper and
  behavioral reference
- that helper is good enough for smoke/regression coverage, but it is not yet
  the final deployment/runtime answer

Recommended evaluation criteria:

- easy local development
- simple Docker packaging
- low operational complexity
- no unnecessary TLS/certificate handling inside the backend itself

### 9. Add an HTTPS-compatible room-code lobby/control path

This is necessary if the goal is truly "works everywhere through normal HTTPS
proxies" while keeping the current Internet-mode UX.

Current blocker:

- `InternetCreateRoom()`
- `InternetJoinRoom()`
- `InternetPollRoom()`
- `InternetLeaveRoom()`

all depend on the UDP path in `src/multiplayer_internet_lobby.cc`.

The simplest compatible fix is not another WebSocket unless there is a strong
reason. The lobby/control traffic is tiny and request/response shaped, so a
small HTTPS API is the simpler fit.

Recommended shape:

- keep existing UDP lobby server for the fast path
- add HTTPS endpoints for create/join/poll/leave/start
- client tries UDP first where appropriate
- client can fall back to HTTPS when UDP is unavailable or when the user
  explicitly wants proxy-compatible Internet mode

Likely touch points:

- `src/multiplayer_internet_lobby.cc`
- `tools/internet_lobby_server.py`
- multiplayer options/help text/docs

### 10. Update UI, help text, and naming

Current UI and docs explicitly say "TCP relay".

This needs to become "WebSocket relay" or similar in:

- `src/gui/OptionsMenu.cc`
- `src/gui/MultiplayerMenu_tick.cc`
- `doc/multiplayer_architecture.md`
- any user-facing multiplayer help text

Suggested user-facing wording:

- `WebSocket relay`
- `HTTPS-proxy compatible fallback`

Avoid calling it merely "TCP relay" once the transport is no longer raw TCP.

### 11. Update build, Docker, and deployment assets

Because relay tools are not normal build targets today, this needs explicit
follow-up work.

Decide whether to:

- keep Docker-only compilation/startup for the backend tools
- or add proper build targets/scripts for the relay services

Likely touch points:

- `Dockerfile`
- `tools/docker-entrypoint.sh`
- maybe `tools/Makefile.am`
- deployment documentation
- optional reverse-proxy config examples

### 12. Extend regression and soak coverage for the new paths

Current multiplayer test tooling is focused on host/client game instances, not
on externally managed relay/proxy services.

New coverage is needed for:

- WebSocket relay host registration
- WebSocket relay client registration
- `WELCOME` delivery over the fallback path
- input / input-bundle forwarding
- resync/world-state traffic over fallback
- abort/disconnect propagation
- fallback ordering: direct -> UDP relay -> WebSocket relay
- runtime latency overlay still producing sensible values
- host auto-detect still classifying the fallback path sensibly
- `wss://` via HTTPS proxy
- bad cert / unreachable proxy / auth-required proxy behavior

Practical harness follow-up:

- add helper scripts to start the relay backend for automated local tests
- add a smoke path that exercises the WebSocket fallback end-to-end
- if feasible, add one proxy-assisted smoke case in CI or as a developer-only
  script

## Incremental Rollout

### Phase A

- promote current direct/UDP behavior into rerunnable regression coverage
- introduce endpoint/backend/stream-relay seams without behavior changes
- decide config surface
- choose server runtime/library
- choose proxy configuration story

### Phase B

- move existing UDP lobby and raw TCP relay behind the new seams
- implement WebSocket client wrapper
- implement WebSocket relay backend
- wire WebSocket fallback through the transport/session stack
- keep rerunning the existing direct/UDP regression set during the refactor

### Phase C

- switch fallback order to direct -> UDP relay -> WebSocket relay
- keep old raw TCP path only behind a temporary debug/dev switch if needed

### Phase D

- add HTTPS-compatible room-code lobby/control fallback
- make Internet-mode room code work on proxy-only networks

### Phase E

- delete raw TCP relay code
- rename leftover `TCP_RELAY`-specific constants/state to the final naming
- update docs and deployment examples fully

## Main Risks And Decisions

### Server-side implementation choice

There is no existing in-tree WebSocket server stack. This is the first major
decision and will determine how much build/deployment work is needed.

### Lobby compatibility scope

If the branch only replaces gameplay fallback transport, the user-visible claim
must stay narrow. The existing room-code Internet mode still depends on UDP
unless the lobby/control path is also upgraded.

### Proxy UX

Need to decide whether:

- libcurl proxy environment variables are enough for the first cut
- or whether explicit proxy UI/config fields are required

### Version floor

If libcurl is used for WebSockets, configure/build documentation should state a
minimum libcurl/runtime capability level for the needed HTTP(S)
`CONNECT_ONLY`, `curl_easy_send()` / `curl_easy_recv()`, and proxy/TLS
behavior. Do not rely on WebSocket header presence alone as the capability
check.

### Naming churn

A temporary additive `WS_RELAY` path is safer for bring-up, but it delays final
cleanup and renaming.

## Definition Of Done

The WebSocket fallback can be considered complete when all of the following are
true:

1. direct ENet and UDP relay behavior stay unchanged
2. the fallback gameplay path uses WebSocket and works reliably
3. the fallback is exposed as `wss://` and works through normal HTTPS proxies
4. room-code Internet mode also works on proxy-only networks
5. UI/docs refer to the new fallback accurately
6. Docker/deployment assets document how to run it
7. automated or scripted regression coverage exists for the fallback path
8. the pre-existing direct/UDP regression set still passes after the refactor
9. lobby/control and transport-specific branching stay behind the documented
   abstraction seams instead of spreading through session/UI code

## External References

- libcurl WebSocket overview:
  - https://curl.se/libcurl/c/libcurl-ws.html
- libcurl WebSocket `CONNECT_ONLY` example:
  - https://curl.se/libcurl/c/websocket.html
- libcurl API overview:
  - https://curl.se/libcurl/c/libcurl.html
- Example proxy-related libcurl option reference:
  - https://curl.se/libcurl/c/CURLOPT_PROXY_CAINFO.html
