# AdaptIPC Showcase

Interactive demonstration and visual results. Everything here is
driven by the real implementation: the decision log is produced by
`benchmarks/decision_demo.c` running the actual adaptive policy
(`ADAPT_DECISION_LOG`), and every figure is generated from real
measurement CSVs by `scripts/generate_showcase_figures.py`.

* `run_demo.sh` -- one command: build, run the real demo workload,
  capture the decision log, convert to JSON, open the dashboard.
* `dashboard/` -- zero-dependency HTML/JS dashboard visualizing real
  routing decisions (costs, reasons, timeline, history).
* `outputs/decisions.csv` -- a real captured decision log (committed
  as a representative example; regenerate with run_demo.sh).
* `results/REPORT.md` -- auto-generated after each demo run.
