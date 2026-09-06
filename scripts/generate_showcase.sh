#!/usr/bin/env bash
# One command -> all visuals (figures, diagrams, dashboard data).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
FAIL=0
echo "== figures from real measurement data =="
python3 scripts/generate_showcase_figures.py || FAIL=1
echo "== dashboard decision data (real) =="
if [ -f showcase/outputs/decisions.csv ]; then
    python3 scripts/decisions_csv_to_json.py showcase/outputs/decisions.csv \
        showcase/dashboard/decisions.json || FAIL=1
    echo "✓ showcase/dashboard/decisions.json"
else
    echo "⚠ decisions.csv missing -- run showcase/run_demo.sh first"
fi
echo "== validation =="
if bash -n demo/adaptipc_lab.sh && python3 -c "import ast;ast.parse(open('scripts/lab_analyze.py').read())"; then
    echo "✓ scripts parse"
else
    echo "✗ script validation failed"; FAIL=1
fi
echo "========================================"
echo "AdaptIPC Showcase Generated"
echo ""
echo "Figures:"
for f in assets/figures/*.png assets/architecture.png \
         assets/decision_pipeline.png assets/before_vs_adaptive.png; do
    [ -f "$f" ] && echo "✓ $f"
done
echo ""
echo "Results:"
ls showcase/outputs/decisions.csv 2>/dev/null && echo "✓ showcase/outputs/decisions.csv"
echo ""
echo "Dashboard:"
ls showcase/dashboard/index.html >/dev/null 2>&1 && echo "✓ showcase/dashboard/index.html"
echo ""
[ "$FAIL" -eq 0 ] && echo "Validation: ✓ all generated" || echo "Validation: ✗ some steps failed (see above)"
exit "$FAIL"
