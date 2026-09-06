# AdaptIPC Showcase

Interactive demonstration and visual results. Everything here is
driven by the real implementation: the decision log is produced by
`showcase/decision_log.c` linking the actual AdaptIPC library, and
every figure is generated from real measurement CSVs and JSONL
telemetry by `showcase/scripts/generate_figures.py`.

* `run_demo.sh` -- one command: build, run the 600-message demo
  workload through the real adaptive policy, capture the JSONL
  decision log, render all figures, open the dashboard.
* `dashboard/` -- zero-dependency HTML/JS dashboard: hero metrics,
  architecture diagrams, telemetry table, and an interactive
  decision replay (play/step/pause through every captured decision).
* `decision_log.c` -- the demo harness (C, links `src/*.c` directly;
  emits per-decision JSONL telemetry).
* `outputs/decision_log.jsonl` -- a real captured decision log
  (committed as a representative example; regenerate with run_demo.sh).
* `outputs/routing_trace.csv` -- per-iteration routing trace from
  `benchmarks/routing_trace.c` (1200 iterations).
* `scripts/generate_figures.py` -- renders fig01-fig10 into
  `assets/figures/` from committed raw data.
* `scripts/generate_diagrams.py` -- renders `assets/architecture.png`
  and `assets/decision_pipeline.png`.
* `scripts/serve_dashboard.py` -- zero-dependency local server;
  serves from repo root so figures resolve.
* `results/REPORT.md` -- auto-generated after each demo run with
  provenance metadata and headline verified numbers.

Quick start (from repo root):

```sh
./showcase/run_demo.sh
# or via the master pipeline:
./scripts/generate_showcase.sh
```

