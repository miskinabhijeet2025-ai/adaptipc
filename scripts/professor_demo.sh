#!/bin/bash
# professor_demo.sh -- One-command automated showcase for reviewers.
#
# End-to-end sequence:
#   1. Correctness suite (10 test suites via demo/adaptipc_lab.sh)
#   2. Table I drift check (scripts/ratio_check.py)
#   3. Live decision telemetry + routing trace capture
#   4. All publication figures + diagrams
#   5. REPORT.md with build provenance (platform/compiler/commit/timestamp)
#
# Usage: ./scripts/professor_demo.sh
set -euo pipefail

# Ensure core system utilities are resolvable even in restricted-PATH shells
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin${PATH:+:${PATH}}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

PYTHON_BIN="$(command -v python3 || echo python)"
PASS=0; FAIL=0

banner() {
    echo "============================================================"
    echo "   AdaptIPC -- Professor / Reviewer Verification Run"
    echo "   $(date -u +'%Y-%m-%d %H:%M:%S UTC')"
    echo "============================================================"
    echo
}

check() {  # check <label> <cmd...>
    local label="$1"; shift
    echo "  [RUN] ${label}"
    if "$@" > /tmp/prof_demo_out.txt 2>&1; then
        PASS=$((PASS+1))
        echo "  [PASS] ${label}"
        return 0
    else
        FAIL=$((FAIL+1))
        echo "  [FAIL] ${label} (see /tmp/prof_demo_out.txt)"
        return 1
    fi
}

banner

# -- 1. Correctness suite --------------------------------------------------
if check "Correctness suite (10 suites)" ./demo/adaptipc_lab.sh -experiment correctness; then :; fi

# -- 2. Table I drift check -------------------------------------------------
if check "Table I drift check (ratio_check.py)" ${PYTHON_BIN} scripts/ratio_check.py; then :; fi

# -- 3. Full showcase generation (telemetry, figures, diagrams, REPORT.md) --
if check "Showcase generation (generate_showcase.sh)" ./scripts/generate_showcase.sh; then :; fi

# -- 4. Artifact inventory ----------------------------------------------------
echo
echo "  [INVENTORY] Generated artifacts:"
for f in \
    showcase/outputs/decision_log.jsonl \
    showcase/outputs/routing_trace.csv \
    showcase/results/REPORT.md \
    assets/architecture.png \
    assets/decision_pipeline.png \
    assets/figures/fig01_throughput_vs_size.png \
    assets/figures/fig02_latency_vs_size.png \
    assets/figures/fig03_routing_decisions.png \
    assets/figures/fig04_policy_comparison.png \
    assets/figures/fig05_stability.png \
    assets/figures/fig06_queue_aware.png \
    assets/figures/fig07_transport_health.png \
    assets/figures/fig08_cost_breakdown.png \
    assets/figures/fig09_alpha_sensitivity.png \
    assets/figures/fig10_decision_timeline.png
do
    if [[ -f "$f" ]]; then
        printf "    OK  %8s bytes  %s\\n" "$(/usr/bin/stat -f%z "$f")" "$f"
    else
        printf "    --  MISSING       %s\\n" "$f"
        FAIL=$((FAIL+1))
    fi
done

# -- 5. Final verdict ----------------------------------------------------------
echo
echo "============================================================"
if [[ ${FAIL} -eq 0 ]]; then
    echo "   VERDICT: ALL ${PASS} CHECKS PASSED"
    echo "   Report : showcase/results/REPORT.md"
    echo "   Board : ${PYTHON_BIN} showcase/scripts/serve_dashboard.py --port 8000"
    echo "            open http://localhost:8000/showcase/dashboard/index.html"
else
    echo "   VERDICT: ${FAIL} CHECK(S) FAILED / ${PASS} PASSED"
    echo "   Inspect /tmp/prof_demo_out.txt for details."
fi
echo "============================================================"
exit $(( FAIL > 0 ? 1 : 0 ))
