#!/usr/bin/env bash
# AdaptIPC website server -- http://localhost:8123/index.html
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${PORT:-8123}"
command -v python3 >/dev/null || { echo "python3 required"; exit 1; }
echo "AdaptIPC Website"
echo "http://localhost:${PORT}/index.html"
cd "$ROOT/website"
exec python3 -m http.server "$PORT"
