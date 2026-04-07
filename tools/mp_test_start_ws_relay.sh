#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

HOST="${1:-127.0.0.1}"
PORT="${2:-24681}"

exec python3 "${REPO_ROOT}/tools/ws_relay_server.py" "${HOST}" "${PORT}"
