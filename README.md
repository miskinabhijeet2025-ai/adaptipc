# AdaptIPC

<p align="center">
  <img src="assets/before_vs_adaptive.png" alt="Static IPC vs AdaptIPC" width="720">
</p>

**Adaptive Inter-Process Communication Routing Middleware**

AdaptIPC dynamically selects between POSIX shared memory and Unix domain
sockets using workload context, measured transport cost, queue state,
transport health, and hysteresis-aware decision logic — one API, the
right transport for every message.

![C11](https://img.shields.io/badge/C-11-blue)
![platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey)
![tests](https://img.shields.io/badge/tests-10%2F10%20passing-2ea043)
![sanitizers](https://img.shields.io/badge/ASan%20%7C%20LSan%20%7C%20TSan-clean-2ea043)

<p align="center">
  <img src="assets/architecture.png" alt="AdaptIPC architecture" width="560">
</p>

## 🚀 See It Work

```sh
./showcase/run_demo.sh          # real policy decisions + dashboard (~15 s)
```

This builds the real implementation, runs a demo workload through the
actual adaptive policy, captures the library's decision log, and opens
an interactive dashboard showing every routing decision — payload,
estimated SHM/UDS costs, queue wait, switching margin, reason, and the
routing timeline.

<p align="center">
  <img src="assets/screenshots/dashboard_uds_phase.png" alt="Dashboard — UDS phase" width="720">
</p>
<p align="center"><i>The dashboard stepping through 512 real policy
decisions (cost bars, reasons, routing timeline).</i></p>

> *Interactive demonstration workload — not the paper's authoritative
> benchmark (that is `./demo/adaptipc_lab.sh -runall`).*

## 📊 Key Results (measured on the reference platform)

| | |
|---|---|
| **Throughput** | 12,350 MB/s (SHM ring), 9,400–9,700 MB/s (adaptive policies) |
| **Route stability** | 8 switches / 200k adversarial messages, false-switch rate **0.00** |
| **Backlog bound** | 14 frames (57 KB) under saturation vs unbounded ring |
| **Queue prediction** | linear in occupancy; honest failure mode documented |
| **Lazy setup** | endpoint init 0.40 ms vs 1.82 ms eager when SHM unused |
| **Correctness** | 10/10 tests, ASan/LSan/TSan clean, 1M-msg zero-loss stress |

Every number is traceable to `experiments/raw/`, `experiments/v2_1/raw/`
or `tests/` output — see [Reproduce This Result](#reproduce-this-result).

## How AdaptIPC Makes a Decision

<p align="center">
  <img src="assets/decision_pipeline.png" alt="Decision pipeline" width="520">
</p>

## Experimental Results

| Figure | Source data | Generator |
|---|---|---|
| ![Throughput](assets/figures/throughput.png) | `experiments/raw/payload_sweep.csv` | `scripts/generate_showcase_figures.py` |
| ![Latency](assets/figures/latency.png) | `experiments/raw/payload_sweep.csv` | same |
| ![Routing decisions](assets/figures/routing_decisions.png) | fresh run `raw/routing_trace.csv` | same |
| ![Policy comparison](assets/figures/policy_comparison.png) | `experiments/v2_1/raw/adversarial_delta.csv` | same |
| ![Stability](assets/figures/stability.png) | `experiments/v2_1/raw/adversarial_delta.csv` | same |
| ![Queue-aware](assets/figures/queue_aware.png) | `experiments/v2_1/raw/queue_prediction_accuracy.csv` | same |
| ![Transport health](assets/figures/transport_health.png) | `experiments/v2_1/raw/stall_timeline.csv` | same |
| ![Cost breakdown](assets/figures/cost_breakdown.png) | `showcase/outputs/decisions.csv` | same |

## Reproduce This Result

```sh
# all figures + diagrams + dashboard data (from committed raw CSVs)
./scripts/generate_showcase.sh
# full experiment campaigns (fresh measurements)
./demo/adaptipc_lab.sh -runall                 # ~10 min
python3 scripts/summarize_results.py           # summary tables
# correctness
./demo/adaptipc_lab.sh -experiment correctness # 10/10 tests
```

## Repository Navigation

| Section | Description |
|---|---|
| `src/`, `include/` | AdaptIPC implementation and public API |
| `tests/` | correctness tests (unit + integration, sanitizers clean) |
| `benchmarks/` | benchmark and demo tool sources + reference outputs |
| `experiments/` | adaptive-policy experiment campaigns (raw CSVs, summaries) |
| `scripts/` | reproduction and visualization scripts |
| `showcase/` | interactive demo + dashboard (this page's demo) |
| `paper/` | research paper and experimental documentation |
| `demo/` | professor experiment lab CLI |


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


---

# One-command Experiment Lab

Run a polished, reproducible experiment campaign from a single command:

```sh
./demo/adaptipc_lab.sh -presentation   # fast in-class demo (~1 min)
./demo/adaptipc_lab.sh                 # interactive menu
./demo/adaptipc_lab.sh -experiment all # full research campaign
```

* Every experiment (build & test, transport comparison, adaptive
  routing trace, hysteresis stability, EWMA alpha sensitivity, latency
  breakdown) writes raw CSVs into a unique timestamped directory
  `experiments/runs/<date_time>_<label>/` -- historical runs are never
  overwritten (`experiments/runs/LATEST` points at the newest).
* `scripts/lab_analyze.py` derives processed CSVs, PNG figures, result
  tables and a static `report/report.html` **from the raw data**;
  provenance (run id + git commit) is embedded in `run_metadata.json`.
* Live demo (real router, real EWMA, rendered dashboard):
  `./demo/adaptipc_lab.sh -live`.
* Guides: `docs/EXPERIMENT_GUIDE.md` (per-experiment
  objective/hypothesis/data/interpretation) and
  `docs/PROFESSOR_DEMO.md` (an exact 8-10 minute presentation script).

The historical benchmark data under `benchmarks/` and
`experiments/raw|summaries|figures|v2_1/` are preserved reference
results from the measurement campaigns behind the paper; fresh laptop
runs land in `experiments/runs/` and are never mixed with them.

---

# Context-Aware Routing (v2 extension)

The sections above describe the **validated size-based baseline**
(policy `size_hysteresis`). Version 2 extends the router into a
context-aware, cost-aware, latency-aware system while preserving the
original policy as an ablation baseline.

## Architecture

```
Application
    ↓ adapt_send()
Context Collector (runtime_context.c)
    payload size, ring occupancy, EWMA arrival rate,
    EWMA consumer drain rate, consumer-activity signal
    ↓
Cost Estimator (cost_model.c)
    cost_T(S) = fixed_T + slope_T·S          (online-calibrated)
    queue_wait(SHM) = occupancy / drain_rate  (conservative fallback)
    notify floor (SHM) = 2 ms if consumer idle, else ~5 µs
    setup cost (SHM, if unmapped) = 52 µs
    ↓
Adaptive Policy
    Score(T) = transport_cost + queue_cost + setup_cost
             + switching_cost + health_penalty + latency_penalty
    switch only if  Score(new) + margin < Score(current)
    ↓
Transport Health (transport_health.c)
    HEALTHY ⇄ DEGRADED ⇄ BLOCKED → RECOVERING → HEALTHY (debounced)
```

## Policy modes (ablation ladder)

| mode | behavior |
|---|---|
| `size_only` | raw EWMA threshold, no deadband |
| `size_hysteresis` | original validated policy (default) |
| `queue_aware` | + queue-wait escape from a backlogged ring |
| `cost_aware` | + measured per-transport costs, switching margin, setup and notification modeling |
| `full_adaptive` | + transport health, learned crossover, QoS |

Select via `adapt_config_t.policy` or `ADAPTIPC_POLICY=<name>`.
QoS: `adapt_config_t.qos` ∈ {balanced, latency, throughput} plus
`latency_budget_us` (env: `ADAPTIPC_QOS`, `ADAPTIPC_LATENCY_BUDGET_US`).

## Stability argument

The decision margin H implements the same anti-thrash guarantee as the
size-only deadband: if per-side cost-estimation error is bounded by ε,
noise alone cannot flip a route while H > 2ε. The learned crossover S*
is clamped to a bounded range and rate-limited by an EWMA; the health
model requires `debounce` consecutive samples before any transition.

## Decision logging

Set `ADAPTIPC_DECISION_LOG=path` to dump a CSV of the last 512 routing
decisions (payload, occupancy, per-transport costs, queue wait,
switch/setup/health/latency penalties, final scores, selected route,
reason). Disabled by default; zero cost when disabled.

## Experiments

```sh
cc -std=c11 -O3 -Wall -Wextra -pthread -Iinclude \
   benchmarks/adaptive_ablation.c src/*.c -o /tmp/abl
/tmp/abl --out experiments/raw          # runs experiments A-G
python3 scripts/summarize_results.py    # -> experiments/summaries/
python3 scripts/generate_figures.py     # -> experiments/figures/
```

Raw CSVs carry platform/compiler/seed metadata; every experiment uses
the identical workload across policies. Shortfalls (e.g., UDS datagram
receiver-queue overflow under bulk) are recorded as `dropped=N` in the
notes column and never silently dropped or fabricated.

## Known limitations

* macOS shm_open(O_TRUNC) returns EINVAL on existing objects; the ring
  deliberately avoids O_TRUNC and re-initializes cursors instead.
* io_uring support is compile-time optional on Linux and reported as
  UNSUPPORTED elsewhere.
* UDS SOCK_DGRAM drops datagrams silently when the receiver queue
  overflows; pure-UDS baselines cap payload sweeps at 64 KB.
* The cost model is linear in payload size; extreme bimodality within
  one size class degrades the fit until recalibration.
