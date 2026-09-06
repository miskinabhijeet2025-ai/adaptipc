# AdaptIPC Experiment Guide

Every experiment below runs the REAL implementation, writes raw CSVs
into a unique run directory, and derives all graphs/tables/report from
that raw data. Provenance (run id + git commit) is recorded in
`run_metadata.json` and stamped on the report.

## How results flow

```
C benchmark (benchmarks/, tests/)
    -> raw CSV            experiments/runs/<run>/raw/
    -> lab_analyze.py     experiments/runs/<run>/processed/
    -> plots              experiments/runs/<run>/plots/
    -> tables             experiments/runs/<run>/tables/
    -> report             experiments/runs/<run>/report/report.html
```

## 1. Build & Test (`-experiment correctness`)

* **Objective:** prove the implementation builds and behaves correctly
  before any measurement.
* **Command:** `./demo/adaptipc_lab.sh -experiment correctness`
* **Measurement:** the 10 test binaries (router/EWMA, SPSC ring, flow
  control, lazy negotiation, backpressure gate, cost model, policy
  modes, decision-log consistency, transition accounting, occupancy
  instrumentation).
* **Interpretation:** a red test invalidates every other result.
* **Output:** `test_results.txt`.

## 2. Transport Comparison (`-experiment transport`)

* **Objective:** which transport is faster at which payload size?
* **Hypothesis:** UDS wins for small payloads (no ring setup cost),
  SHM wins for large payloads (no kernel copies); AdaptIPC tracks the
  better one per size.
* **Configuration:** fixed sweep workload (64 B .. 16 MB, the
  repository's standard sweep); modes UDS / SHM / AdaptIPC.
* **Raw output:** `raw/transport_{uds,shm,adapt}.csv` (per-size rows).
* **Graph:** `plots/transport_comparison.png` (log-log).
* **Interpretation:** crossover point between the UDS and SHM curves;
  AdaptIPC should sit near the lower envelope. Report the actual
  crossover you measure -- it is platform dependent.
* **Limitations:** single-run per point; variance is reported as
  sample counts, run `-experiment all` twice for medians.

## 3. Adaptive Routing (`-experiment adaptive`)

* **Objective:** show the EWMA actually driving transport selection.
* **Command:** `./demo/adaptipc_lab.sh -experiment adaptive`
* **Workload:** 3 phases (512 B control -> 16 KB bulk -> 512 B), 3000
  messages; per-message EWMA and route are read back from the library.
* **Raw output:** `raw/routing_trace.csv`
  (`seq,payload_bytes,ewma_bytes,route,switched`).
* **Graph:** `plots/adaptive_routing.png` (EWMA curve, both
  thresholds, route strip underneath).
* **Interpretation:** the route changes exactly when the EWMA crosses
  a threshold -- never on raw message size.

## 4. Hysteresis Stability (`-experiment hysteresis`)

* **Objective:** verify the deadband suppresses route thrashing.
* **Command:** `./demo/adaptipc_lab.sh -experiment hysteresis`
* **Workload:** the repository's adversarial thrash pattern
  (alternates inside the [1024, 4096] deadband), 20k messages,
  run twice: with `size_only` and with `size_hysteresis`.
* **Raw output:** `raw/hysteresis_{size_only,size_hysteresis}.csv`.
* **Graph:** `plots/hysteresis_stability.png`.
* **Interpretation:** compare switches_per_1000 between the two
  policies. Report the actual numbers you measure.

## 5. EWMA Alpha Sensitivity (`-experiment ewma`)

* **Objective:** measure how the smoothing factor alpha changes
  adaptation speed vs stability.
* **Command:** `./demo/adaptipc_lab.sh -experiment ewma`
* **Configuration:** alpha in {0.05, 0.1, 0.2, 0.3, 0.5, 0.8}
  (the library's supported range), bimodal workload (128 B control /
  2 MB bulk with random bursts), 10k messages each.
* **Raw output:** `raw/ewma_alpha_<alpha>.csv`.
* **Graph:** `plots/ewma_alpha_sensitivity.png`.
* **Interpretation:** lower alpha -> fewer switches but slower
  adaptation; higher alpha -> the opposite. Report the measured
  tradeoff; do not assume it is monotonic.

## 6. Latency Breakdown (`-experiment latency`)

* **Objective:** decompose producer-side latency into measured
  components (EWMA classify, payload copy) using the library's own
  counters (ADAPTIPC_STATS=1).
* **Raw output:** `raw/latency_bimodal.csv` +
  `raw/latency_breakdown_bimodal_producer.txt`.
* **Graph:** `plots/latency_breakdown.png`.
* **Limitations:** only components the library actually instruments;
  no invented decomposition.

## Full research campaign

`./demo/adaptipc_lab.sh -runall` runs everything with full iteration
counts (several minutes). Presentation mode
(`./demo/adaptipc_lab.sh -presentation`) runs reduced workloads for
in-class demos -- both are labeled in `run_metadata.json`.
