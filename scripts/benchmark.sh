#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

HOST="${1:-127.0.0.1}"
PORT="${2:-6379}"
NREQ="${3:-100000}"

exec ./build/mycache_bench "$HOST" "$PORT" "$NREQ"
