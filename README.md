# AdaptIPC — Adaptive IPC Routing Middleware

AdaptIPC is an adaptive inter-process communication (IPC) routing middleware
for POSIX systems. Instead of committing a channel to one transport at
deployment time, AdaptIPC classifies each message by an exponentially
weighted moving average (EWMA) of its payload size and routes it over the
cheapest correct transport: a lock-free single-producer/single-consumer ring
buffer over POSIX shared memory for bulk traffic, or an AF_UNIX datagram
socket for small control messages. A hysteresis deadband between two
thresholds (τ_low = 1024 B, τ_high = 4096 B) provably prevents route
oscillation; on an adversarial 100,000-iteration thrashing workload the
routing state performs exactly zero false switches.

This repository contains the full source of the middleware, the benchmark
harness used in the accompanying systems paper (`paper/main.pdf`), the raw
and derived measurement data behind every quantitative claim in that paper,
and the scripts that regenerate its figures and verify its internal
consistency.

## Repository structure

```
├── paper/                     LaTeX source of the systems paper
│   ├── main.tex               paper text (IEEEtran conference format)
│   ├── IEEEtran.cls           IEEE class file (checked in; self-contained)
│   └── main.pdf               built PDF (tracked intentionally)
├── include/                   public headers (adapt_ipc.h, shm_ringbuffer.h,
│                              uds_fallback.h)
├── src/                       library implementation
│   ├── adapt_ipc.c            adaptive router: EWMA classifier + hysteresis
│   ├── shm_ringbuffer.c       lock-free SPSC ring buffer over POSIX SHM
│   └── uds_fallback.c         AF_UNIX SOCK_DGRAM transport
├── tests/                     unit tests + multi-process benchmark harness
│   ├── test_shm_ringbuffer.c  SPSC correctness incl. concurrent stress test
│   ├── test_adapt_ipc.c       end-to-end router/EWMA/demux test
│   ├── benchmark_suite.c      fork-based producer/consumer benchmark
│   └── CMakeLists.txt
├── benchmarks/                measurement outputs + microbenchmark sources
│   ├── summary.txt            median-of-3 campaign summary (authoritative)
│   ├── summary_run{1,2,3}.txt per-run logs backing the medians
│   ├── results_{sweep,bimodal}.csv  raw per-message/per-size metrics
│   ├── latency_breakdown_*.txt        send/receive cost decomposition
│   ├── init_latency.txt               adapt_init() endpoint-creation cost
│   ├── alpha_sensitivity.txt          EWMA smoothing-factor sweep
│   ├── cost_model_constants.txt       measured eq.-3 constants + S* calc
│   ├── fig1..fig3 (.png/.pdf)         paper figures
│   └── {cost_model_bench,alpha_sweep,init_latency_bench,uds_rtt_bench}.c
├── scripts/
│   ├── run_benchmarks.sh      full measurement campaign (-O3 build + run)
│   ├── combine_runs.py        median-combine N campaign runs
│   ├── ratio_check.py         consistency gate: Table I vs. summary.txt
│   └── plot_results.py        generate fig1–fig3 from the CSVs
└── LICENSE                    (see License section)
```

## Building

Requirements: any C11 compiler (clang or gcc), CMake ≥ 3.16, POSIX SHM/UDS
support. Developed and measured on macOS (Apple silicon); Linux should work
unchanged.

```sh
cmake -B build && cmake --build build
ctest --test-dir build          # unit tests + a small benchmark smoke test
```

This builds `libadaptipc.a` plus the test binaries and the
`benchmark_suite` harness.

## Reproducing the measurement campaign

The full campaign (9 workload × transport combinations, ~105 GB transferred
per bimodal mode) takes roughly five minutes on an idle machine:

```sh
./scripts/run_benchmarks.sh                 # -O3 build + all measurements
BIMODAL_ITERS=200 ./scripts/run_benchmarks.sh   # quick smoke variant
```

Raw metrics land in `benchmarks/results_*.csv` and a human-readable log in
`benchmarks/summary.txt`. On Linux the script pins producer/consumer to
cores via `taskset` (override with `CORES=...`).

Then:

```sh
python3 scripts/plot_results.py     # fig1–fig3 from the CSVs (pandas/matplotlib/seaborn)
python3 scripts/combine_runs.py out.txt run1.txt [run2.txt ...]
                                    # median-combine N campaigns
python3 scripts/ratio_check.py      # consistency gate: Table I vs. summary.txt
```

`ratio_check.py` re-derives every throughput number in Table I of the paper
from `benchmarks/summary.txt` and exits non-zero on any drift > 5%.

Standalone microbenchmarks (cost-model constants, UDS round-trip latency,
EWMA α sensitivity, `adapt_init()` creation cost) are built individually;
see the header comment of each `.c` file under `benchmarks/` for its exact
build line and methodology.

## Reproducibility notes

Every quantitative claim in the paper traces to the checked-in
`benchmarks/summary.txt` (median of three campaign runs) and the preserved
per-run logs `summary_run{1,2,3}.txt`; `scripts/ratio_check.py`
programmatically re-derives Table I from those logs and exits non-zero on
drift. The numbers were collected on a single Apple-silicon Mac without CPU
pinning and are hardware-dependent — we observed up to ~3× run-to-run
variance on the socket baseline under background load (see the Limitations
subsection, Section V-B of the paper). Camera-ready measurements should use
the median-of-N methodology above on pinned, otherwise-idle hardware.

## Paper

```sh
cd paper && tectonic main.tex       # or: pdflatex main.tex (run twice)
```

`IEEEtran.cls` is checked in, so no TeX distribution package install is
needed beyond a basic engine. Figures resolve from `../benchmarks/`.

## License

No open-source license is currently attached to this repository; all
rights remain with the authors. If you wish to reuse the code or text,
please open an issue or contact the authors. (The paper text in `paper/`
may additionally be subject to copyright transfer to its publication
venue.)
