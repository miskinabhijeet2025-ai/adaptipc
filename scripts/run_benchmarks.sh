#!/usr/bin/env bash
# run_benchmarks.sh -- compile AdaptIPC with -O3 and run the full
# benchmark matrix, appending raw metrics to benchmarks/results_*.csv.
#
# Environment overrides:
#   BIMODAL_ITERS  iterations for bimodal/thrash workloads (default 100000)
#   CORES          CPU cores to pin to on Linux (default "2,3")
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH_DIR="$ROOT/benchmarks"
BUILD_DIR="$ROOT/build-bench"
BIN="$BUILD_DIR/bin/benchmark_suite"
SUMMARY="$BENCH_DIR/summary.txt"

CC="${CC:-cc}"
CFLAGS="-std=c11 -O3 -Wall -Wextra -Wpedantic -I$ROOT/include"
BIMODAL_ITERS="${BIMODAL_ITERS:-100000}"
CORES="${CORES:-2,3}"

mkdir -p "$BENCH_DIR" "$BUILD_DIR/bin"

echo "== Compiling with -O3 =="
"$CC" $CFLAGS -c "$ROOT/src/shm_ringbuffer.c" -o "$BUILD_DIR/shm_ringbuffer.o"
"$CC" $CFLAGS -c "$ROOT/src/uds_fallback.c"   -o "$BUILD_DIR/uds_fallback.o"
"$CC" $CFLAGS -c "$ROOT/src/adapt_ipc.c"      -o "$BUILD_DIR/adapt_ipc.o"
ar rcs "$BUILD_DIR/libadaptipc.a" "$BUILD_DIR"/shm_ringbuffer.o \
    "$BUILD_DIR"/uds_fallback.o "$BUILD_DIR"/adapt_ipc.o
"$CC" $CFLAGS "$ROOT/tests/benchmark_suite.c" "$BUILD_DIR/libadaptipc.a" \
    -o "$BIN" -lpthread
echo "built: $BIN"

# CPU pinning: taskset is Linux-only; run unpinned elsewhere (e.g. macOS).
PIN=()
if [[ "$(uname)" == "Linux" ]] && command -v taskset >/dev/null 2>&1; then
    PIN=(taskset -c "$CORES")
    echo "pinning producer/consumer to cores: $CORES"
else
    echo "taskset unavailable ($(uname)); running unpinned"
fi

run_case() {
    local mode="$1" workload="$2" out="$3"; shift 3
    echo "=== mode=$mode workload=$workload -> $out ===" | tee -a "$SUMMARY"
    # /usr/bin/time statistics when available (-v on Linux, -l on macOS)
    local time_stats=""
    if [[ -x /usr/bin/time ]]; then
        if /usr/bin/time -v true >/dev/null 2>&1; then
            time_stats="-v"
        elif /usr/bin/time -l true >/dev/null 2>&1; then
            time_stats="-l"
        fi
    fi
    if [[ -n "$time_stats" ]]; then
        { /usr/bin/time "$time_stats" ${PIN[@]+"${PIN[@]}"} "$BIN" \
            --mode "$mode" --workload "$workload" --out "$BENCH_DIR/$out" "$@" ; } \
            2>&1 | tee -a "$SUMMARY"
    else
        { date '+start: %s.%N'; ${PIN[@]+"${PIN[@]}"} "$BIN" \
            --mode "$mode" --workload "$workload" --out "$BENCH_DIR/$out" "$@" ; } \
            2>&1 | tee -a "$SUMMARY"
        { date '+end:   %s.%N'; } 2>&1 | tee -a "$SUMMARY"
    fi
}

echo "AdaptIPC benchmark run started: $(date)" > "$SUMMARY"

# --- Sweep test: all three modes -----------------------------------------
for mode in uds shm adapt; do
    run_case "$mode" sweep results_sweep.csv
done

# --- Bimodal + thrashing: all three modes --------------------------------
for mode in uds shm adapt; do
    run_case "$mode" bimodal results_bimodal.csv --iters "$BIMODAL_ITERS"
    run_case "$mode" thrash results_bimodal.csv --iters "$BIMODAL_ITERS"
done

echo ""
echo "== Done. Raw CSVs in $BENCH_DIR/, log in $SUMMARY =="
echo "Plot with: python3 scripts/plot_results.py"
