#!/usr/bin/env bash
set -euo pipefail

LOBBY_HOST="${LOBBY_HOST:-0.0.0.0}"
LOBBY_PORT="${LOBBY_PORT:-12347}"
LOBBY_UDP_ENABLE="${LOBBY_UDP_ENABLE:-1}"
LOBBY_HTTP_ENABLE="${LOBBY_HTTP_ENABLE:-1}"
LOBBY_HTTP_HOST="${LOBBY_HTTP_HOST:-$LOBBY_HOST}"
LOBBY_HTTP_PORT="${LOBBY_HTTP_PORT:-$LOBBY_PORT}"
LOBBY_HTTP_PATH="${LOBBY_HTTP_PATH:-/lobby}"
RELAY_HOST="${RELAY_HOST:-0.0.0.0}"
RELAY_PORT="${RELAY_PORT:-12348}"
TCP_RELAY_HOST="${TCP_RELAY_HOST:-0.0.0.0}"
TCP_RELAY_PORT="${TCP_RELAY_PORT:-12349}"
WS_RELAY_ENABLE="${WS_RELAY_ENABLE:-1}"
WS_RELAY_HOST="${WS_RELAY_HOST:-0.0.0.0}"
WS_RELAY_PORT="${WS_RELAY_PORT:-12350}"

lobby_args=(--host "$LOBBY_HOST" --port "$LOBBY_PORT")
if [[ "${LOBBY_UDP_ENABLE}" == "0" ]]; then
    lobby_args+=(--disable-udp)
fi
if [[ "${LOBBY_HTTP_ENABLE}" == "0" ]]; then
    lobby_args+=(--disable-http)
    echo "Starting lobby server on UDP ${LOBBY_HOST}:${LOBBY_PORT}"
else
    lobby_args+=(--http-host "$LOBBY_HTTP_HOST" --http-port "$LOBBY_HTTP_PORT" --http-path "$LOBBY_HTTP_PATH")
    echo "Starting lobby server on UDP ${LOBBY_HOST}:${LOBBY_PORT} and HTTP ${LOBBY_HTTP_HOST}:${LOBBY_HTTP_PORT}${LOBBY_HTTP_PATH}"
fi
python3 -u /opt/enigma/tools/internet_lobby_server.py "${lobby_args[@]}" &
lobby_pid=$!

echo "Starting relay server on ${RELAY_HOST}:${RELAY_PORT}"
/usr/local/bin/enigma-relay "$RELAY_HOST" "$RELAY_PORT" &
relay_pid=$!

echo "Starting TCP relay server on ${TCP_RELAY_HOST}:${TCP_RELAY_PORT}"
/usr/local/bin/enigma-tcp-relay "$TCP_RELAY_HOST" "$TCP_RELAY_PORT" &
tcp_relay_pid=$!

ws_relay_pid=""
if [[ "${WS_RELAY_ENABLE}" != "0" ]]; then
    echo "Starting WebSocket relay server on ${WS_RELAY_HOST}:${WS_RELAY_PORT}"
    python3 -u /opt/enigma/tools/ws_relay_server.py "$WS_RELAY_HOST" "$WS_RELAY_PORT" &
    ws_relay_pid=$!
fi

cleanup() {
    kill "$lobby_pid" "$relay_pid" "$tcp_relay_pid" 2>/dev/null || true
    if [[ -n "${ws_relay_pid}" ]]; then
        kill "$ws_relay_pid" 2>/dev/null || true
    fi
}

trap cleanup SIGINT SIGTERM

if [[ -n "${ws_relay_pid}" ]]; then
    wait -n "$lobby_pid" "$relay_pid" "$tcp_relay_pid" "$ws_relay_pid"
else
    wait -n "$lobby_pid" "$relay_pid" "$tcp_relay_pid"
fi
