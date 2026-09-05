#!/usr/bin/env python3
"""generate_figures.py -- publication-quality figures from raw results.

Reads experiments/raw/*.csv and writes experiments/figures/fig*.png.
Requires matplotlib (optional dependency; the pipeline works without it).

Usage: python3 scripts/generate_figures.py
"""
import csv
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

RAW = "experiments/raw"
FIG = "experiments/figures"

POLICY_ORDER = ["uds", "shm", "size_only", "size_hysteresis",
                "queue_aware", "cost_aware", "full_adaptive"]
COLORS = {p: c for p, c in zip(
    POLICY_ORDER,
    ["#888888", "#444444", "#1f77b4", "#aec7e8", "#2ca02c",
     "#ff7f0e", "#d62728"])}


def load(name):
    path = os.path.join(RAW, name)
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def save(fig, name):
    os.makedirs(FIG, exist_ok=True)
    path = os.path.join(FIG, name)
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print("wrote", path)


def by_policy(rows):
    """Last row per policy label."""
    out = {}
    for r in rows:
        out[r["policy"]] = r
    return out


def fig1_throughput():
    """Throughput vs payload size per policy."""
    rows = [r for r in load("payload_sweep.csv") if "size" in
            "".join(r.keys())] or load("payload_sweep.csv")
    if not rows:
        return
    # the sweep stores per-run aggregates; if a size column is present
    # (payload_distribution), group by it, else plot per-policy bars.
    fig, ax = plt.subplots(figsize=(7, 4))
    data = by_policy(rows)
    labels = [p for p in POLICY_ORDER if p in data]
    vals = [float(data[p]["throughput_mbps"]) for p in labels]
    ax.bar(labels, vals, color=[COLORS[p] for p in labels])
    ax.set_ylabel("throughput (MB/s)")
    ax.set_title("Fig. 1: aggregate throughput by policy (payload sweep)")
    ax.tick_params(axis="x", rotation=30)
    save(fig, "fig1_throughput_by_policy.png")


def fig2_latency():
    """p50/p99 by policy."""
    rows = load("payload_sweep.csv")
    if not rows:
        return
    data = by_policy(rows)
    labels = [p for p in POLICY_ORDER if p in data]
    p50 = [float(data[p]["p50_us"]) for p in labels]
    p99 = [float(data[p]["p99_us"]) for p in labels]
    x = range(len(labels))
    fig, ax = plt.subplots(figsize=(7, 4))
    ax.bar([i - 0.2 for i in x], p50, 0.4, label="p50")
    ax.bar([i + 0.2 for i in x], p99, 0.4, label="p99")
    ax.set_yscale("log")
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, rotation=30)
    ax.set_ylabel("latency (us, log)")
    ax.set_title("Fig. 2: latency by policy (payload sweep)")
    ax.legend()
    save(fig, "fig2_latency_by_policy.png")


def fig3_occupancy():
    """Mean/max queue occupancy by policy (queue pressure)."""
    rows = load("queue_pressure.csv")
    if not rows:
        return
    data = by_policy(rows)
    labels = [p for p in POLICY_ORDER if p in data]
    mean = [float(data[p]["mean_queue_occupancy_bytes"]) / 1e6
            for p in labels]
    mx = [float(data[p]["max_queue_occupancy_bytes"]) / 1e6 for p in labels]
    x = range(len(labels))
    fig, ax = plt.subplots(figsize=(7, 4))
    ax.bar([i - 0.2 for i in x], mean, 0.4, label="mean")
    ax.bar([i + 0.2 for i in x], mx, 0.4, label="max")
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, rotation=30)
    ax.set_ylabel("SHM ring occupancy (MB)")
    ax.set_title("Fig. 3: queue occupancy by policy (queue pressure)")
    ax.legend()
    save(fig, "fig3_queue_occupancy.png")


def fig4_switches():
    """Route switches: adversarial oscillation across policies."""
    rows = load("adversarial_switching.csv")
    if not rows:
        return
    data = by_policy(rows)
    labels = [p for p in POLICY_ORDER if p in data]
    vals = [int(data[p]["route_switches"]) for p in labels]
    fig, ax = plt.subplots(figsize=(7, 4))
    ax.bar(labels, vals, color=[COLORS.get(p, "#777") for p in labels])
    ax.set_ylabel("route switches (256 adversarial messages)")
    ax.set_title("Fig. 4: switching under adversarial oscillation")
    ax.tick_params(axis="x", rotation=30)
    save(fig, "fig4_adversarial_switches.png")


def fig5_qos():
    """QoS comparison: p99 and UDS/SHM split."""
    rows = load("qos.csv")
    if not rows:
        return
    labels = [r["policy"] for r in rows]
    p99 = [float(r["p99_us"]) for r in rows]
    shm_frac = [float(r["shm_messages"]) /
                max(1, int(r["shm_messages"]) + int(r["uds_messages"]))
                for r in rows]
    x = range(len(labels))
    fig, ax = plt.subplots(figsize=(7, 4))
    ax.bar([i - 0.2 for i in x], p99, 0.4, label="p99 (us)")
    ax2 = ax.twinx()
    ax2.bar([i + 0.2 for i in x], shm_frac, 0.4,
            color="tab:green", label="SHM fraction")
    ax2.set_ylim(0, 1)
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, rotation=30)
    ax.set_ylabel("p99 latency (us)")
    ax2.set_ylabel("fraction of messages on SHM")
    ax.set_title("Fig. 5: QoS posture changes routing")
    ax.legend(loc="upper left")
    ax2.legend(loc="upper right")
    save(fig, "fig5_qos.png")


def fig6_ablation():
    """Ablation: throughput + p99 for the policy ladder."""
    rows = load("payload_sweep.csv") + load("queue_pressure.csv")
    if not rows:
        return
    data = by_policy(rows)
    labels = [p for p in POLICY_ORDER if p in data]
    tp = [float(data[p]["throughput_mbps"]) for p in labels]
    p99 = [float(data[p]["p99_us"]) for p in labels]
    x = range(len(labels))
    fig, ax = plt.subplots(figsize=(7, 4))
    ax.bar([i - 0.2 for i in x], tp, 0.4, label="throughput MB/s")
    ax2 = ax.twinx()
    ax2.bar([i + 0.2 for i in x], p99, 0.4,
            color="tab:red", label="p99 us")
    ax2.set_yscale("log")
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, rotation=30)
    ax.set_ylabel("throughput (MB/s)")
    ax2.set_ylabel("p99 latency (us, log)")
    ax.set_title("Fig. 6: ablation ladder")
    ax.legend(loc="upper left")
    ax2.legend(loc="upper right")
    save(fig, "fig6_ablation.png")


def main():
    fig1_throughput()
    fig2_latency()
    fig3_occupancy()
    fig4_switches()
    fig5_qos()
    fig6_ablation()


if __name__ == "__main__":
    main()
