#!/usr/bin/env bash
# generate_figures.sh -- summarize raw results and render figures.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p experiments/summaries experiments/figures
python3 scripts/summarize_results.py
python3 scripts/generate_figures.py || echo "matplotlib unavailable; summaries still generated"
