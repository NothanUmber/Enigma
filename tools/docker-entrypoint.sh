#!/usr/bin/env bash
set -euo pipefail

LOBBY_HOST="${LOBBY_HOST:-0.0.0.0}"
LOBBY_PORT="${LOBBY_PORT:-12347}"
RELAY_HOST="${RELAY_HOST:-0.0.0.0}"
RELAY_PORT="${RELAY_PORT:-12348}"
TCP_RELAY_HOST="${TCP_RELAY_HOST:-0.0.0.0}"
TCP_RELAY_PORT="${TCP_RELAY_PORT:-12349}"

echo "Starting lobby server on ${LOBBY_HOST}:${LOBBY_PORT}"
python3 -u /opt/enigma/tools/internet_lobby_server.py --host "$LOBBY_HOST" --port "$LOBBY_PORT" &
lobby_pid=$!

echo "Starting relay server on ${RELAY_HOST}:${RELAY_PORT}"
/usr/local/bin/enigma-relay "$RELAY_HOST" "$RELAY_PORT" &
relay_pid=$!

echo "Starting TCP relay server on ${TCP_RELAY_HOST}:${TCP_RELAY_PORT}"
/usr/local/bin/enigma-tcp-relay "$TCP_RELAY_HOST" "$TCP_RELAY_PORT" &
tcp_relay_pid=$!

trap 'kill $lobby_pid $relay_pid $tcp_relay_pid' SIGINT SIGTERM

wait -n "$lobby_pid" "$relay_pid" "$tcp_relay_pid"
