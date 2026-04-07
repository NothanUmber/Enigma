#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

HOST="${1:-127.0.0.1}"
PORT="${2:-12348}"
OUT="${ENIGMA_MP_TEST_RELAY_BIN:-${TMPDIR:-/tmp}/enigma-mp-test-relay}"
SRC="${REPO_ROOT}/tools/relay_server.cc"

if [[ ! -x "${OUT}" || "${SRC}" -nt "${OUT}" ]]; then
  "${CXX:-c++}" -std=c++14 "${SRC}" $(pkg-config --cflags --libs libenet) -o "${OUT}"
fi

exec "${OUT}" "${HOST}" "${PORT}"
