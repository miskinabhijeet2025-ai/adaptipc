#!/usr/bin/env bash
# Polished terminal demonstration: builds, validates, runs the real
# adaptive-routing showcase, prints results. macOS/Linux friendly.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
echo "==============================================="
echo "ADAPTIPC DEMONSTRATION"
echo "==============================================="
echo "Experiment : Context-Aware Adaptive Routing"
echo "Workload   : Mixed control + bulk traffic"
echo "Policies   : size_only, size_hysteresis,"
echo "             queue_aware, cost_aware, full_adaptive"
echo "Commit     : $(git rev-parse --short HEAD 2>/dev/null)"
echo "-----------------------------------------------"
echo "Running (real implementation)..."
if [ -x demo/adaptipc_lab.sh ]; then
    ./demo/adaptipc_lab.sh -presentation
fi
echo "-----------------------------------------------"
echo "Generating visualizations..."
bash showcase/run_demo.sh 2>&1 | grep -Ev "^http|Ctrl-C" || true
echo ""
echo "✓ routing timeline        showcase/dashboard"
echo "✓ cost comparison         assets/figures/cost_breakdown.png"
echo "✓ policy comparison       assets/figures/policy_comparison.png"
echo "✓ stability plot          assets/figures/stability.png"
echo "✓ decision log            showcase/outputs/decisions.csv"
echo ""
echo "Results written to showcase/outputs/"
echo "==============================================="
