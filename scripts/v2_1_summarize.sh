#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
echo "summaries are static markdown derived from raw CSVs;"
echo "see experiments/summaries/*.md and EXPERIMENTS_V2_1.md"
