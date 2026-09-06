#!/usr/bin/env bash
# ============================================================================
# AdaptIPC interactive demonstration (~15 seconds).
# Runs the REAL implementation, captures a real decision log, converts
# it to JSON, and opens the dashboard. The demo workload is reduced --
# it is NOT the paper's authoritative benchmark (that is
# scripts/run_benchmarks.sh / demo/adaptipc_lab.sh -runall).
# ============================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O3 -Wall -Wextra -Wpedantic -pthread -Iinclude"
SRC="src/shm_ringbuffer.c src/uds_fallback.c src/adapt_ipc.c src/cost_model.c src/runtime_context.c src/transport_health.c"
OUT=showcase/outputs
mkdir -p "$OUT"

echo "== building (real implementation) =="
"$CC" $CFLAGS $SRC benchmarks/decision_demo.c -o "$OUT/decision_demo"

echo "== running demo workload (small -> bulk -> mixed -> small) =="
ADAPTIPC_DECISION_LOG="$OUT/decisions.csv" "$OUT/decision_demo"

echo "== converting decision log to dashboard JSON =="
python3 scripts/decisions_csv_to_json.py "$OUT/decisions.csv" \
    showcase/dashboard/decisions.json

echo "== generating showcase figures from real data =="
python3 scripts/generate_showcase_figures.py || \
    echo "⚠ some figures unavailable (matplotlib missing?)"

cat > "$OUT/REPORT.md" <<MD
# AdaptIPC Demo Report

- git commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)
- branch: $(git rev-parse --abbrev-ref HEAD 2>/dev/null)
- platform: $(uname -s) $(uname -m)
- compiler: $("$CC" --version | head -1)
- flags: $CFLAGS
- timestamp: $(date -u +%Y-%m-%dT%H:%M:%SZ)
- workload: decision_demo (small -> bulk -> mixed -> small), real
  full_adaptive policy decisions logged by the library
- validation status: see tests (./demo/adaptipc_lab.sh -experiment
  correctness)

This is the INTERACTIVE DEMONSTRATION workload, not the paper's
authoritative benchmark. Figures generated: assets/figures/.
MD

echo ""
echo "========================================"
echo "AdaptIPC Demo Generated"
echo ""
echo "Decision log:  $OUT/decisions.csv ($(grep -c '' "$OUT/decisions.csv") lines)"
echo "Dashboard:     showcase/dashboard/index.html"
echo "Report:        $OUT/REPORT.md"
echo "========================================"
echo "Starting local web server (Ctrl-C to stop)..."
echo "Open: http://localhost:8123/dashboard/index.html"
cd showcase
exec python3 -m http.server 8123
