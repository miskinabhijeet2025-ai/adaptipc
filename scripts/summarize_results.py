#!/usr/bin/env python3
"""summarize_results.py -- derive summary tables from experiments/raw/*.csv.

Writes experiments/summaries/*.md with the ablation and per-experiment
tables used by the paper's experimental section.

Usage: python3 scripts/summarize_results.py
"""
import csv
import os
import sys

RAW = "experiments/raw"
OUT = "experiments/summaries"

METRICS = ("throughput_mbps", "p50_us", "p95_us", "p99_us", "p99_9_us",
           "route_switches", "uds_messages", "shm_messages",
           "backpressure_events", "health_transitions")


def rows(name):
    path = os.path.join(RAW, name)
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def fmt_row(r, cols):
    return " | ".join(str(r.get(c, ""))[:14] for c in cols)


def write(name, text):
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name), "w") as f:
        f.write(text)
    print("wrote", os.path.join(OUT, name))


def ablation():
    r = rows("payload_sweep.csv") + rows("queue_pressure.csv") + \
        rows("burst_workload.csv")
    if not r:
        return
    cols = ("policy", "kind", "message_count", "throughput_mbps",
            "p50_us", "p95_us", "p99_us", "p99_9_us", "route_switches",
            "backpressure_events", "notes")
    out = ["# Ablation matrix (payload_sweep + queue_pressure + burst)\n",
           "| " + " | ".join(cols) + " |",
           "|" + "---|" * len(cols)]
    for row in r:
        out.append("| " + fmt_row(row, cols) + " |")
    write("ablation_matrix.md", "\n".join(out) + "\n")


def per_experiment(name, title):
    r = rows(name)
    if not r:
        return
    cols = ("policy", "kind", "message_count", "throughput_mbps",
            "p50_us", "p99_us", "p99_9_us", "route_switches",
            "uds_messages", "shm_messages", "health_transitions",
            "mean_queue_occupancy_bytes", "notes")
    out = [f"# {title}\n",
           "| " + " | ".join(cols) + " |",
           "|" + "---|" * len(cols)]
    for row in r:
        out.append("| " + fmt_row(row, cols) + " |")
    write(name.replace(".csv", ".md"), "\n".join(out) + "\n")


def main():
    ablation()
    per_experiment("payload_sweep.csv", "Experiment A: payload sweep")
    per_experiment("queue_pressure.csv", "Experiment B: queue pressure")
    per_experiment("burst_workload.csv", "Experiment C: bursty workload")
    per_experiment("adversarial_switching.csv",
                   "Experiment D: adversarial oscillation")
    per_experiment("degradation.csv", "Experiment E: degradation+recovery")
    per_experiment("setup_cost.csv", "Experiment F: setup cost")
    per_experiment("qos.csv", "Experiment G: QoS")


if __name__ == "__main__":
    sys.exit(main())
