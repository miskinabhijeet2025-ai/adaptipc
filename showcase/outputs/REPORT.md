# AdaptIPC Demo Report

- git commit: 4bcf80494d17faa0e13f93b217a202f5953a1e04
- branch: main
- platform: Darwin arm64
- compiler: Apple clang version 21.0.0 (clang-2100.1.1.101)
- flags: -std=c11 -O3 -Wall -Wextra -Wpedantic -pthread -Iinclude
- timestamp: 2026-09-06T17:42:57Z
- workload: decision_log harness (small -> bulk -> mixed -> small), real
  policy decisions logged by the library as JSONL telemetry
- validation status: see tests (./demo/adaptipc_lab.sh -experiment
  correctness)

This is the INTERACTIVE DEMONSTRATION workload, not the paper's
authoritative benchmark. Figures generated: assets/figures/.
