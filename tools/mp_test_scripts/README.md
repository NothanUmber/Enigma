# Multiplayer Test Scripts

These scripts are consumed by `tools/mp_test_env.py` to drive two Enigma instances started with the in-engine multiplayer test driver (`--mp-test-*`).

## Usage

```sh
./tools/mp_test_env.py --script tools/mp_test_scripts/basic_join_and_move.txt
```

## Script Format

- Empty lines and lines starting with `#` are ignored.
- Lines are parsed with shell-style quoting (Python `shlex`).

Supported operations:

- `host <CMD...>`: send a command to the host instance.
- `client <CMD...>`: send a command to the client instance.
- `both <CMD...>`: send a command to both instances.
- `sleep <ms>` or `sleep ms=<ms>`: wait.
- `wait role=<host|client|any> contains="<substring>" timeout_ms=<ms>`: wait until a received frame contains the substring.

Enigma emits framed lines like `EVT ...`, `OK ...`, `ERR ...` back to the controller; `mp_test_env.py` prints them with timestamps.

