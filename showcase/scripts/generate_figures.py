#!/usr/bin/env python3
"""
generate_figures.py -- generate paper-quality figures from real benchmark data.
"""
from __future__ import annotations

import csv
import json
import os
import re
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.patches as mpatches
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "assets" / "figures"
OUT.mkdir(parents=True, exist_ok=True)

plt.rcParams.update({
    "figure.dpi": 110,
    "savefig.dpi": 200,
    "font.size": 12,
    "axes.titlesize": 13,
    "axes.labelsize": 12,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "legend.frameon": False,
    "lines.linewidth": 2.0,
    "grid.alpha": 0.25,
    "grid.linestyle": ":",
})

WROTE: list[str] = []
SKIPPED: list[str] = []

def save(fig, name: str) -> None:
    p = OUT / name
    fig.tight_layout()
    fig.savefig(p)
    plt.close(fig)
    WROTE.append(f"assets/figures/{name}")

def warn(name: str, why: str) -> None:
    SKIPPED.append(f"{name}: {why}")

def load_sweep(path: Path):
    rows = []
    if not path.exists(): return rows
    with path.open() as f:
        r = csv.DictReader(f)
        for row in r:
            try:
                rows.append({
                    "mode": row["mode"],
                    "payload_bytes": int(row["payload_bytes"]),
                    "latency_us": float(row["latency_ns"]) / 1000.0,
                    "throughput_mbps": float(row["throughput_mbps"]),
                    "route": int(row["route_taken"]),
                })
            except (KeyError, ValueError):
                continue
    return rows

def load_jsonl(path: Path):
    out = []
    if not path.exists(): return out
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"): continue
            try: out.append(json.loads(line))
            except json.JSONDecodeError: continue
    return out

def fig_throughput_vs_size(rows):
    if not rows:
        warn("fig01_throughput_vs_size", "benchmarks/results_sweep.csv missing")
        return
    fig, ax = plt.subplots(figsize=(7.2, 4.2))
    for mode, color, marker in (("uds", "#5b8def", "o"),
                                ("shm", "#f08a24", "s"),
                                ("adapt", "#2ca02c", "^")):
        xs = [r["payload_bytes"] for r in rows if r["mode"] == mode]
        ys = [r["throughput_mbps"] for r in rows if r["mode"] == mode]
        if not xs: continue
        order = np.argsort(xs)
        ax.plot(np.array(xs)[order], np.array(ys)[order],
                marker=marker, color=color, label=mode.upper())
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xlabel("Message size (bytes)")
    ax.set_ylabel("Throughput (MB/s)")
    ax.set_title("Fig. 1 Throughput vs Message Size (Sweep Workload)")
    ax.grid(True, which="both")
    ax.legend()
    save(fig, "fig01_throughput_vs_size.png")

def fig_latency_vs_size(rows):
    if not rows:
        warn("fig02_latency_vs_size", "benchmarks/results_sweep.csv missing")
        return
    fig, ax = plt.subplots(figsize=(7.2, 4.2))
    for mode, color, marker in (("uds", "#5b8def", "o"),
                                ("shm", "#f08a24", "s"),
                                ("adapt", "#2ca02c", "^")):
        xs = [r["payload_bytes"] for r in rows if r["mode"] == mode]
        ys = [r["latency_us"] for r in rows if r["mode"] == mode]
        if not xs: continue
        order = np.argsort(xs)
        ax.plot(np.array(xs)[order], np.array(ys)[order],
                marker=marker, color=color, label=mode.upper())
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xlabel("Message size (bytes)")
    ax.set_ylabel("Per-message latency (us)")
    ax.set_title("Fig. 2 Latency vs Message Size (Sweep Workload)")
    ax.grid(True, which="both")
    ax.legend()
    save(fig, "fig02_latency_vs_size.png")

def fig_routing_decisions(trace_csv: Path):
    if not trace_csv.exists():
        warn("fig03_routing_decisions", "showcase/outputs/routing_trace.csv missing")
        return
    seq, pl, ew, rt, sw = [], [], [], [], []
    with trace_csv.open() as f:
        rd = csv.DictReader(f)
        for r in rd:
            try:
                seq.append(int(r["seq"]))
                pl.append(int(r["payload_bytes"]))
                ew.append(float(r["ewma_bytes"]))
                rt.append(r["route"])
                sw.append(int(r["switched"]))
            except (KeyError, ValueError): continue
    if not seq: return
    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(8.2, 5.6), sharex=True,
                                          gridspec_kw={"height_ratios": [2, 2, 1]})
    ax1.fill_between(seq, 0, pl, color="#cccccc", alpha=0.6, step="mid")
    ax1.set_ylabel("payload (B)")
    ax1.set_yscale("log", base=2)
    ax1.set_title("Fig. 3 Routing Decisions on Mixed Workload")
    ax2.plot(seq, ew, color="#2ca02c", lw=1.8, label="EWMA")
    ax2.axhline(1024, color="#5b8def", ls="--", lw=1.0, label="tau_low (1024 B)")
    ax2.axhline(4096, color="#f08a24", ls="--", lw=1.0, label="tau_high (4096 B)")
    ax2.set_ylabel("EWMA (B)")
    ax2.set_yscale("log", base=2)
    ax2.legend(loc="upper right", ncol=3, fontsize=10)
    ribbon = np.where(np.array(rt) == "SHM", 1.0, 0.0)
    ax3.fill_between(seq, 0, ribbon, color="#2ca02c", step="mid", alpha=0.85)
    for s, swv in zip(seq, sw):
        if swv:
            ax3.axvline(s, color="#d62728", lw=0.6, alpha=0.6)
    ax3.set_yticks([0, 1]); ax3.set_yticklabels(["UDS", "SHM"])
    ax3.set_ylim(-0.05, 1.1)
    ax3.set_xlabel("message #")
    ax3.set_ylabel("route")
    save(fig, "fig03_routing_decisions.png")

def fig_policy_comparison():
    path = ROOT / "experiments" / "raw" / "payload_sweep.csv"
    if not path.exists():
        warn("fig04_policy_comparison", "experiments/raw/payload_sweep.csv missing")
        return
    buckets: dict[str, list[float]] = {}
    with path.open() as f:
        rd = csv.DictReader(f)
        for r in rd:
            if r.get("experiment") != "payload_sweep": continue
            try: tp = float(r["throughput_mbps"])
            except (KeyError, ValueError): continue
            buckets.setdefault(r["policy"], []).append(tp)
    if not buckets: return
    fig, ax = plt.subplots(figsize=(7.2, 4.2))
    labels = sorted(buckets)
    means = [np.mean(buckets[k]) for k in labels]
    stds = [np.std(buckets[k]) for k in labels]
    ax.bar(labels, means, yerr=stds, color="#5b8def", edgecolor="black", alpha=0.85, capsize=4)
    ax.set_ylabel("Throughput (MB/s) [mean +- std]")
    ax.set_title("Fig. 4 Adaptive Policy Comparison (payload_sweep)")
    ax.tick_params(axis="x", rotation=20)
    save(fig, "fig04_policy_comparison.png")

def fig_stability():
    path = ROOT / "experiments" / "raw" / "adversarial_switching.csv"
    if not path.exists():
        warn("fig05_stability", "experiments/raw/adversarial_switching.csv missing")
        return
    pols, switches = [], []
    with path.open() as f:
        rd = csv.DictReader(f)
        for r in rd:
            if r.get("experiment") != "adversarial_switching": continue
            try: sw = int(r["route_switches"])
            except (KeyError, ValueError): continue
            pols.append(r["policy"])
            switches.append(sw)
    if not pols: return
    fig, ax = plt.subplots(figsize=(7.2, 4.2))
    order = np.argsort(pols)
    pols = np.array(pols)[order]
    switches = np.array(switches)[order]
    colors = ["#2ca02c" if s <= 1 else "#d62728" for s in switches]
    ax.bar(pols, switches, color=colors, edgecolor="black", alpha=0.85)
    ax.set_ylabel("Route switches (adversarial workload)")
    ax.set_title("Fig. 5 Anti-Thrashing: Switches Under Adversarial Load")
    ax.tick_params(axis="x", rotation=20)
    for i, s in enumerate(switches):
        ax.text(i, s + max(switches) * 0.02 + 0.1, str(s), ha="center", fontsize=11)
    save(fig, "fig05_stability.png")

def fig_queue_aware():
    path = ROOT / "experiments" / "raw" / "queue_pressure.csv"
    if not path.exists():
        warn("fig06_queue_aware", "experiments/raw/queue_pressure.csv missing")
        return
    policies, tp, occ = [], [], []
    with path.open() as f:
        rd = csv.DictReader(f)
        for r in rd:
            if r.get("experiment") != "queue_pressure": continue
            try:
                policies.append(r["policy"])
                tp.append(float(r["throughput_mbps"]))
                occ.append(float(r["max_queue_occupancy_bytes"]) / (1 << 20))
            except (KeyError, ValueError): continue
    if not policies: return
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(8.4, 4.0))
    ax1.bar(policies, tp, color="#5b8def", edgecolor="black", alpha=0.85)
    ax1.set_ylabel("Throughput (MB/s)")
    ax1.tick_params(axis="x", rotation=20)
    ax2.bar(policies, occ, color="#f08a24", edgecolor="black", alpha=0.85)
    ax2.set_ylabel("Max SHM Ring Occupancy (MiB)")
    ax2.tick_params(axis="x", rotation=20)
    fig.suptitle("Fig. 6 Queue-Aware Behavior Under Ring Pressure")
    save(fig, "fig06_queue_aware.png")

def fig_transport_health():
    states = [("HEALTHY", "#2ca02c"),
              ("DEGRADED", "#f0ad4e"),
              ("BLOCKED", "#d62728"),
              ("RECOVERING", "#5b8def")]
    fig, ax = plt.subplots(figsize=(8.0, 4.0))
    pos = {n: (i, 0) for i, (n, _) in enumerate(states)}
    for name, color in states:
        x, y = pos[name]
        ax.add_patch(mpatches.FancyBboxPatch(
            (x - 0.55, y - 0.35), 1.1, 0.7,
            boxstyle="round,pad=0.06", linewidth=1.4,
            edgecolor="black", facecolor=color, alpha=0.85))
        ax.text(x, y, name, ha="center", va="center", color="white",
                fontsize=12, fontweight="bold")
    arrows = [(0, 1, "3+ timeouts"),
              (1, 2, "consec. fails"),
              (2, 3, "send ok"),
              (3, 0, "debounce")]
    for src, dst, lab in arrows:
        x0, _ = pos[states[src][0]]
        x1, _ = pos[states[dst][0]]
        ax.annotate("", xy=(x1 - 0.55, 0), xytext=(x0 + 0.55, 0),
                    arrowprops=dict(arrowstyle="->", lw=1.6, color="black"))
        ax.text((x0 + x1) / 2.0, 0.50, lab, ha="center", fontsize=10, color="black")
    ax.set_xlim(-0.8, len(states) - 0.2)
    ax.set_ylim(-1.0, 1.0)
    ax.set_axis_off()
    ax.set_title("Fig. 7 Transport Health FSM (from transport_health.c)")
    save(fig, "fig07_transport_health.png")

def fig_cost_breakdown():
    txt = ROOT / "benchmarks" / "cost_model_constants.txt"
    if not txt.exists():
        warn("fig08_cost_breakdown", "benchmarks/cost_model_constants.txt missing")
        return
    sizes, bcopy = [], []
    for line in txt.read_text().splitlines():
        m = re.match(r"\s*B_copy\[\s*(\d+)\s*B\]\s*=\s*([\d.]+)\s*GB/s", line)
        if m:
            sizes.append(int(m.group(1)))
            bcopy.append(float(m.group(2)))
    if not sizes: return
    sizes = np.array(sizes)
    shm_cost = 0.020 + sizes / (np.array(bcopy) * 1000.0)
    uds_cost = 2.500 + 0.018 * sizes
    fig, ax = plt.subplots(figsize=(7.2, 4.4))
    ax.plot(sizes, shm_cost, marker="s", color="#2ca02c", label="SHM Cost")
    ax.plot(sizes, uds_cost, marker="o", color="#5b8def", label="UDS Cost")
    ax.axvline(1024, color="#888", ls=":", lw=1.0, label="tau_low (1024 B)")
    ax.axvline(4096, color="#888", ls="--", lw=1.0, label="tau_high (4096 B)")
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xlabel("Payload size (bytes)")
    ax.set_ylabel("Per-message transport cost (us, log)")
    ax.set_title("Fig. 8 Cost Model: SHM vs UDS (S* = 246,841 B)")
    ax.legend()
    ax.grid(True, which="both")
    save(fig, "fig08_cost_breakdown.png")

def fig_alpha_sensitivity():
    path = ROOT / "benchmarks" / "alpha_sensitivity.txt"
    if not path.exists():
        warn("fig09_alpha_sensitivity", "benchmarks/alpha_sensitivity.txt missing")
        return
    alphas, flips = [], []
    for line in path.read_text().splitlines():
        m = re.match(r"alpha=([\d.]+)\s+scenario=B.*msgs_to_SHM_flip=(\d+)", line)
        if m:
            alphas.append(float(m.group(1)))
            flips.append(int(m.group(2)))
    if not alphas: return
    fig, ax = plt.subplots(figsize=(7.2, 4.0))
    ax.plot(alphas, flips, marker="o", color="#2ca02c", lw=2.0)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("EWMA alpha")
    ax.set_ylabel("Messages to first SHM flip")
    ax.set_title("Fig. 9 EWMA-alpha Sensitivity (Measured)")
    ax.grid(True, which="both")
    save(fig, "fig09_alpha_sensitivity.png")

def fig_decision_timeline():
    log = ROOT / "showcase" / "outputs" / "decision_log.jsonl"
    rows = load_jsonl(log)
    if not rows:
        warn("fig10_decision_timeline", "showcase/outputs/decision_log.jsonl missing")
        return
    seq = [r["seq"] for r in rows]
    pl = [r["payload"] for r in rows]
    ew = [r["ewma"] for r in rows]
    rt = [r["route"] for r in rows]
    sw = [r["switched"] for r in rows]
    fig, (a1, a2, a3) = plt.subplots(3, 1, figsize=(8.4, 5.6), sharex=True,
                                     gridspec_kw={"height_ratios": [2, 2, 1]})
    a1.fill_between(seq, 0, pl, color="#cccccc", alpha=0.6, step="mid")
    a1.set_ylabel("payload (B)")
    a1.set_yscale("log", base=2)
    a2.plot(seq, ew, color="#2ca02c", lw=1.6, label="EWMA")
    a2.axhline(1024, color="#5b8def", ls="--", lw=1.0, label="tau_low")
    a2.axhline(4096, color="#f08a24", ls="--", lw=1.0, label="tau_high")
    a2.set_ylabel("EWMA (B)")
    a2.set_yscale("log", base=2)
    a2.legend(loc="upper right", ncol=3, fontsize=10)
    ribbon = np.where(np.array(rt) == "SHM", 1.0, 0.0)
    a3.fill_between(seq, 0, ribbon, color="#2ca02c", step="mid", alpha=0.85)
    for s, swv in zip(seq, sw):
        if swv:
            a3.axvline(s, color="#d62728", lw=0.6, alpha=0.6)
    a3.set_yticks([0, 1])
    a3.set_yticklabels(["UDS", "SHM"])
    a3.set_ylim(-0.05, 1.1)
    a3.set_xlabel("message #")
    a3.set_ylabel("route")
    fig.suptitle("Fig. 10 Live Decision Timeline (Per-Decision JSON Log)")
    save(fig, "fig10_decision_timeline.png")

def main():
    sweep = load_sweep(ROOT / "benchmarks" / "results_sweep.csv")
    fig_throughput_vs_size(sweep)
    fig_latency_vs_size(sweep)
    fig_routing_decisions(ROOT / "showcase" / "outputs" / "routing_trace.csv")
    fig_policy_comparison()
    fig_stability()
    fig_queue_aware()
    fig_transport_health()
    fig_cost_breakdown()
    fig_alpha_sensitivity()
    fig_decision_timeline()

    print("========================================")
    print(f"AdaptIPC Showcase Figures: {len(WROTE)} written")
    print("========================================")
    for w in WROTE: print(f"  OK   {w}")
    if SKIPPED:
        for s in SKIPPED: print(f"  SKIP {s}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
