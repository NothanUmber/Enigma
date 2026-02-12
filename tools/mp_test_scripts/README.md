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
- `sleep <ms>` or `sleep ms=<ms>`: wait.
- `wait role=<host|client|any> contains="<substring>" timeout_ms=<ms>`: wait until a received frame contains the substring.
- `wait_state_change role=<host|client> field=<key> min_abs_delta=<float> timeout_ms=<ms>`: wait until the latest `EVT name=STATE` changes by at least the given delta compared to the current baseline.

Enigma emits framed lines like `EVT ...`, `OK ...`, `ERR ...` back to the controller; `mp_test_env.py` prints them with timestamps.

## Handy Driver Commands

These are sent via `host ...` / `client ...` / `both ...` lines in scripts:

- `STATE` / `STREAM_STATE interval_ms=<n>`: fetch or periodically emit `EVT name=STATE`.
- `LIST_STEERABLE`: dump steerable actors (controllers/ownership hints).
- `MOUSE_FORCE player=<p> fx=<f> fy=<f>`: add a one-shot local pending mouse force.
- `INJECT_INPUT player=<p> fx=<f> fy=<f> [rot=<i>] [act=<i>]`: enqueue an input sample for the current tick.
- `OVERRIDE_INPUT player=<p> ticks=<n> fx=<f> fy=<f> [rot=<i>] [act=<i>]`: force an input sample for the next `n` simulation ticks.
