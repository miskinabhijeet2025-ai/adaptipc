#!/usr/bin/env bash
# ============================================================================
# AdaptIPC interactive demonstration (~15 seconds).
# Runs the REAL implementation, captures a real decision log (JSONL),
# generates all showcase figures/diagrams, and opens the dashboard.
# The demo workload is reduced -- it is NOT the paper's authoritative
# benchmark (that is scripts/run_benchmarks.sh / demo/adaptipc_lab.sh -runall).
# ============================================================================
set -euo pipefail
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin${PATH:+:${PATH}}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O3 -Wall -Wextra -Wpedantic -pthread -Iinclude"
SRC="src/shm_ringbuffer.c src/uds_fallback.c src/adapt_ipc.c src/cost_model.c src/runtime_context.c src/transport_health.c"
OUT=showcase/outputs
mkdir -p "$OUT" showcase/results

# Resolve a real C compiler (avoid `cc` aliasing to non-compilers)
if ! "$CC" --version >/dev/null 2>&1; then
  CC=/usr/bin/clang
fi

echo "== building (real implementation) =="
"$CC" $CFLAGS $SRC showcase/decision_log.c -o "$OUT/decision_log"

echo "== running demo workload (small -> bulk -> mixed -> small) =="
"$OUT/decision_log" --out "$OUT/decision_log.jsonl" --msgs 600

echo "== generating showcase figures from real data =="
python3 showcase/scripts/generate_figures.py || \
    echo "⚠ some figures unavailable (matplotlib missing?)"
python3 showcase/scripts/generate_diagrams.py || true

cat > "$OUT/REPORT.md" <<MD
# AdaptIPC Demo Report

- git commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)
- branch: $(git rev-parse --abbrev-ref HEAD 2>/dev/null)
- platform: $(uname -s) $(uname -m)
- compiler: $("$CC" --version 2>/dev/null | head -1)
- flags: $CFLAGS
- timestamp: $(date -u +%Y-%m-%dT%H:%M:%SZ)
- workload: decision_log harness (small -> bulk -> mixed -> small), real
  policy decisions logged by the library as JSONL telemetry
- validation status: see tests (./demo/adaptipc_lab.sh -experiment
  correctness)

This is the INTERACTIVE DEMONSTRATION workload, not the paper's
authoritative benchmark. Figures generated: assets/figures/.
MD

echo ""
echo "========================================"
echo "AdaptIPC Demo Generated"
echo ""
echo "Decision log:  $OUT/decision_log.jsonl ($(grep -c '' "$OUT/decision_log.jsonl") lines)"
echo "Dashboard:     showcase/dashboard/index.html"
echo "Report:        $OUT/REPORT.md"
echo "========================================"
echo "Starting local web server (Ctrl-C to stop)..."
echo "Open: http://localhost:8123/showcase/dashboard/index.html"
exec python3 showcase/scripts/serve_dashboard.py --port 8123
