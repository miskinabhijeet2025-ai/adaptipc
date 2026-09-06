# AdaptIPC Showcase -- Verification Report

_Generated on `2026-09-06T17:56:25Z` by `scripts/generate_showcase.sh`._

## Build Provenance

| Field        | Value |
|--------------|-------|
| Repository   | `/Users/abhijeetmiskin/AppData/Projects/OSProject` |
| Git branch   | `main` |
| Git commit   | `4bcf804` (`4bcf80494d17faa0e13f93b217a202f5953a1e04`) |
| Platform     | `Darwin 25.6.0 arm64` |
| Kernel       | `25.6.0
Darwin Kernel Version 25.6.0: Fri Jul 31 19:17:12 PDT 2026; root:xnu-12377.161.14~5/RELEASE_ARM64_T8103` |
| CPU          | `Apple M1` |
| Compiler     | `Apple clang version 21.0.0 (clang-2100.1.1.101)` |
| C standard   | c11 (flags: -O3 -Wall -Wextra -Wpedantic -pthread) |
| Python       | `Python 3.9.6` |
| Timestamp    | `2026-09-06T17:56:25Z` |

## Headline Empirical Results

| Metric                        | Value         | Source |
|-------------------------------|---------------|--------|
| Peak adaptive throughput      | 9444.3 MB/s (31.4x vs UDS 300.3 MB/s) | `experiments/raw/payload_sweep.csv` |
| Mixed workload (bimodal)       | 5678.2 MB/s (8.1x vs UDS) | `benchmarks/summary.txt` + Table I |
| Adversarial thrash            | 2.2x vs UDS, 0 route switches | `benchmarks/summary.txt` |
| Routing decisions captured    | 602  | `showcase/outputs/decision_log.jsonl` |
| Correctness suite             | 10 / 10       | `./demo/adaptipc_lab.sh -experiment correctness` |
| Table I drift check           | ALL CONSISTENT | `scripts/ratio_check.py` |

## Artifacts Produced

- `build-lab/decision_log` -- compiled harness linking real `libadaptipc`
- `build-lab/routing_trace` -- compiled benchmark from `benchmarks/routing_trace.c`
- `showcase/outputs/decision_log.jsonl` -- per-decision JSONL telemetry
- `showcase/outputs/routing_trace.csv` -- per-iteration routing trace
- `assets/architecture.png`, `assets/decision_pipeline.png` -- generated diagrams
- `assets/figures/fig0{1..6}_*.png` -- publication-grade benchmark figures
- `showcase/dashboard/index.html` -- interactive web dashboard

## Reproduction

```bash
./scripts/generate_showcase.sh
/usr/bin/python3 showcase/scripts/serve_dashboard.py --port 8000
# open http://localhost:8000/showcase/dashboard/index.html
```
