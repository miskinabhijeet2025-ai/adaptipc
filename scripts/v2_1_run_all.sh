#!/usr/bin/env bash
# v2.1 hardening experiment suite -- fails loudly on any failure.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT=experiments/v2_1/raw
mkdir -p "$OUT"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O3 -Wall -Wextra -Wpedantic -pthread -Iinclude"
BIN="$ROOT/build-v21/hardening_suite"
mkdir -p "$ROOT/build-v21"
echo "== building hardening suite =="
"$CC" $CFLAGS -DGIT_REV="\"$(git rev-parse --short HEAD)\"" \
    benchmarks/hardening_suite.c \
    src/shm_ringbuffer.c src/uds_fallback.c src/adapt_ipc.c \
    src/cost_model.c src/runtime_context.c src/transport_health.c \
    -o "$BIN" -lpthread
for exp in occupancy epsilon margin prediction adversarial stall \
           crossover lazy; do
    echo "== experiment: $exp =="
    "$BIN" "$exp"
done
echo "== all v2.1 experiments completed =="
