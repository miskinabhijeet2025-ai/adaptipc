# Experiment Index

| Experiment | What it demonstrates | Data | Visualization | Command |
|---|---|---|---|---|
| Build & Test | correctness before any measurement | `test_results.txt` per run | — | `./demo/adaptipc_lab.sh -experiment correctness` |
| Transport comparison | UDS vs SHM vs AdaptIPC by payload size | `experiments/raw/payload_sweep.csv` | `assets/figures/throughput.png`, `latency.png` | `./demo/adaptipc_lab.sh -experiment transport` |
| Adaptive routing trace | EWMA drives per-message transport choice | `experiments/runs/<run>/raw/routing_trace.csv` | `assets/figures/routing_decisions.png` | `./demo/adaptipc_lab.sh -experiment adaptive` |
| Hysteresis stability | deadband suppresses route thrashing | `experiments/v2_1/raw/adversarial_delta.csv` | `assets/figures/stability.png` | `./demo/adaptipc_lab.sh -experiment hysteresis` |
| EWMA alpha sensitivity | smoothing factor vs adaptation/stability | `experiments/runs/<run>/raw/ewma_alpha_*.csv` | `plots/ewma_alpha_sensitivity.png` (per run) | `./demo/adaptipc_lab.sh -experiment ewma` |
| Latency breakdown | measured producer-side components | `experiments/runs/<run>/raw/latency_breakdown_*.txt` | `plots/latency_breakdown.png` (per run) | `./demo/adaptipc_lab.sh -experiment latency` |
| Queue pressure | backlog bounds + queue-aware escape | `experiments/raw/queue_pressure.csv` | `assets/figures/queue_aware.png` | `benchmarks/adaptive_ablation --only queue_pressure` |
| Consumer stall / recovery | health-driven escape and recovery | `experiments/v2_1/raw/stall_timeline.csv` | `assets/figures/transport_health.png` | `benchmarks/hardening_suite stall` |
| Crossover learning | self-calibrating threshold vs ground truth | `experiments/v2_1/raw/crossover_sweep.csv` | `assets/figures/…` (see summaries) | `benchmarks/hardening_suite crossover` |
| QoS | latency/balanced/throughput postures | `experiments/raw/qos.csv` | `assets/figures/…` | `benchmarks/adaptive_ablation --only qos` |
| Decision dashboard | real policy decisions, interactive | `showcase/outputs/decisions.csv` | `showcase/dashboard/` | `./showcase/run_demo.sh` |

Every experiment: raw CSV first, then analysis, then visualization —
provenance in each run's `run_metadata.json`.
