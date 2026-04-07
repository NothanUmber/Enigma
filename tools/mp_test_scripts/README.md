# Multiplayer Test Scripts

These scripts are consumed by `tools/mp_test_env.py` to drive two Enigma instances started with the in-engine multiplayer test driver (`--mp-test-*`).

## Usage

```sh
./tools/mp_test_env.py --script tools/mp_test_scripts/basic_join_and_move.txt
```

Per-level scripts live in `tools/mp_test_scripts/` and are meant to be small,
focused reproducers (for both failures and fixed regressions).

## Script Format

- Empty lines and lines starting with `#` are ignored.
- Lines are parsed with shell-style quoting (Python `shlex`).

Supported operations:

- `host <CMD...>`: send a command to the host instance.
- `client <CMD...>`: send a command to the client instance.
- `both <CMD...>`: send a command to both instances.
- `proc_start name=<id> cmd="<argv...>" [cwd="<dir>"]`: start a helper process and capture its combined stdout/stderr into `proc-<id>.log` inside the test workdir.
- `proc_wait name=<id> contains="<substring>" timeout_ms=<ms>`: wait until a helper-process log contains the requested substring.
- `proc_stop name=<id>`: stop a previously started helper process.
- `sleep <ms>` or `sleep ms=<ms>`: wait.
- `wait role=<host|client|any> contains="<substring>" timeout_ms=<ms>`: search received output for a matching frame.
- `wait_next role=<host|client|any> contains="<substring>" timeout_ms=<ms>`: wait for a future matching frame after the current script point.
- `wait_state_change role=<host|client> field=<key> min_abs_delta=<float> timeout_ms=<ms>`: wait until the latest `EVT name=STATE` changes by at least the given delta compared to the current baseline.
- `wait_state_value role=<host|client> field=<key> [equals=<float>] [min_value=<float>] [max_value=<float>] timeout_ms=<ms>`: wait until a numeric `STATE` field reaches a value/range.
- `wait_state_stable role=<host|client> field=<key> stable_ms=<ms> timeout_ms=<ms>`: wait until a numeric `STATE` field stops changing for the requested stability window.
- `wait_state_compare role_a=<host|client> field_a=<key> role_b=<host|client> field_b=<key> [min_delta=<float>] [max_delta=<float>] [min_abs_delta=<float>] [max_abs_delta=<float>] timeout_ms=<ms>`: wait until two numeric `STATE` fields satisfy a delta bound.
- `wait_line_field_compare role_a=<host|client> contains_a="<substring>" field_a=<key> role_b=<host|client> contains_b="<substring>" field_b=<key> [min_delta=<float>] [max_delta=<float>] [min_abs_delta=<float>] [max_abs_delta=<float>] timeout_ms=<ms>`: wait until parsed numeric fields from matching future `OK`/`EVT` lines satisfy a delta bound.

Enigma emits framed lines like `EVT ...`, `OK ...`, `ERR ...` back to the controller; `mp_test_env.py` prints them with timestamps.

## Handy Driver Commands

These are sent via `host ...` / `client ...` / `both ...` lines in scripts:

- `STATE` / `STREAM_STATE interval_ms=<n>`: fetch or periodically emit `EVT name=STATE`.
- `LIST_STEERABLE`: dump steerable actors (controllers/ownership hints).
- `MOUSE_FORCE player=<p> fx=<f> fy=<f>`: add a one-shot local pending mouse force.
- `HOLD_MOUSE_FORCE player=<p> ticks=<n> fx=<f> fy=<f>`: submit a fixed mouse-force delta once per simulation tick for the next `n` ticks.
- `QUEUE_MOUSE_FORCE player=<p> tick=<t>|ticks_ahead=<n> fx=<f> fy=<f>`: schedule a mouse-force submission for a specific simulation tick.
- `QUEUE_LOCAL_INPUT player=<p> tick=<t>|ticks_ahead=<n> [fx=<f> fy=<f>] [rot=<i>] [act=<i>]`: schedule local pending input at a specific tick.
- `INJECT_INPUT player=<p> fx=<f> fy=<f> [rot=<i>] [act=<i>]`: enqueue an input sample for the current tick.
- `OVERRIDE_INPUT player=<p> ticks=<n> fx=<f> fy=<f> [rot=<i>] [act=<i>]`: force an input sample for the next `n` simulation ticks.
- `SET_ACTOR_POS player=<p> x=<f> y=<f> [vx=<f> vy=<f>]`: directly place a player's main actor.
- `SET_STONE x=<i> y=<i> kind=<kind>` / `SET_ITEM x=<i> y=<i> kind=<kind>` / `MOVE_STONE from_x=<i> from_y=<i> to_x=<i> to_y=<i>` / `CLEAR_MOVABLE_STONES`: deterministic world setup helpers.
- `SETUP_SAVE_FILE path=<file>` / `SETUP_LOAD_FILE path=<file>`: persist and restore a grid/actor setup snapshot.
- `SIM_SNAPSHOT_SAVE` / `SIM_SNAPSHOT_LOAD`: capture and restore the full in-process simulation snapshot.
- `GET_CLIENT_DESYNC_HOLD` / `SET_CLIENT_DESYNC_HOLD enabled=<0|1>`: inspect or toggle client-side desync hold for intentional divergence probes.
- `GET_TIMERGADGET_STATE` / `GET_PUZZLESTONE_STATE`: inspect timer-backed semantic objects, including `alarm_left` and absolute `alarm_tick`.
- `SET_INT key=<option> value=<n>` / `SET_BOOL key=<option> value=<0|1>` / `SET_STRING key=<option> value=<text>`: override persisted options inside the current test instance.
- `SET_RELAY_SERVER server=<host:port|-|none>` / `SET_WS_RELAY_URL url=<ws://...|-|none>` / `SET_TCP_RELAY_SERVER server=<host:port|-|none>`: set the in-memory relay endpoints used by session-start fallback logic.
- `INTERNET_CREATE_ROOM server=<host:port> code=<room> session=<id> seed=<seed> expected=<n> port=<host_port> level_id=<id> [host_id=<id>] [pack=<pack>] [filter=<0|1>]`: create an Internet-lobby room with the provided start payload.
- `INTERNET_JOIN_ROOM server=<host:port> code=<room>`: join an Internet-lobby room, emit `EVT name=INTERNET_PEER` snapshots for the returned member list, and return the decoded start payload in `OK`.
- `INTERNET_START_ROOM server=<host:port> code=<room> session=<id> seed=<seed> expected=<n> port=<host_port> level_id=<id> [host_id=<id>] [pack=<pack>] [filter=<0|1>]`: mark an Internet-lobby room as started with the provided start payload.
- `INTERNET_POLL_ROOM server=<host:port> code=<room> [timeout_ms=<ms>] [poll_sleep_ms=<ms>] [expect_started=<0|1>]`: poll until a room-state reply arrives. If `expect_started` is set, keep polling until the room reaches that started state. Emit `EVT name=INTERNET_PEER` member snapshots for the final reply and return `started=0|1` plus room state in `OK`.
- `INTERNET_LEAVE_ROOM server=<host:port> code=<room>`: leave a tracked Internet-lobby room.

## Notes

- Prefer `wait_next` after a command that emits a line you want to assert on more than once. It avoids accidentally matching an older identical sample.
- `wait_state_stable` is useful for counters like semantic-apply telemetry where you need to prove a repair stopped churning without hard-coding the final counter value.
- Helper-process lifecycle is managed by `mp_test_env.py`; any still-running `proc_start` processes are terminated during cleanup.
- `SET_ACTOR_POS`, `SET_STONE`, `SET_ITEM`, `MOVE_STONE`, `CLEAR_MOVABLE_STONES`, and `SETUP_*` are the main deterministic setup helpers added for branch-local regression work.
- A typical desync-hold flow is: query the current hold state, enable it on the client, drive local input while the host stays authoritative, then disable it and wait for the peers to converge again.

Useful transport/lobby regression scripts:

- `tools/mp_test_scripts/basic_join_and_move.txt`: direct-connect gameplay baseline
- `tools/mp_test_scripts/udp_relay_join_and_move.txt`: UDP-relay gameplay baseline
- `tools/mp_test_scripts/ws_relay_join_and_move.txt`: WebSocket-relay gameplay baseline
- `tools/mp_test_scripts/internet_lobby_room_flow.txt`: UDP Internet-lobby room flow
- `tools/mp_test_scripts/internet_lobby_http_room_flow.txt`: HTTP lobby-control fallback room flow
- `tools/mp_test_scripts/internet_http_ws_fallback_join_and_move.txt`: full proxy-style fallback chain (HTTP lobby control + WebSocket gameplay relay)
