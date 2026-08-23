#!/usr/bin/env python3
"""plot_results.py -- publication-ready figures from AdaptIPC benchmarks.

Reads benchmarks/results_sweep.csv and benchmarks/results_bimodal.csv and
writes fig1..fig3 as .pdf (vector) + .png (300 DPI) into benchmarks/.

Usage:  python3 scripts/plot_results.py [--bench-dir benchmarks]
"""

import argparse
import os

import matplotlib
matplotlib.use("Agg")  # headless
import matplotlib.pyplot as plt
import pandas as pd
import seaborn as sns

MODE_ORDER = ["uds", "shm", "adapt"]
MODE_LABEL = {"uds": "UDS baseline", "shm": "SHM baseline",
              "adapt": "AdaptIPC"}
ROUTE_LABEL = {2: "UDS", 1: "SHM"}

sns.set_theme(style="whitegrid", context="paper")
PALETTE = sns.color_palette("colorblind", n_colors=3)


def load_csv(path):
    if not os.path.exists(path):
        raise SystemExit(f"missing input: {path} (run scripts/run_benchmarks.sh first)")
    df = pd.read_csv(path)
    df["mode"] = pd.Categorical(df["mode"], categories=MODE_ORDER,
                                ordered=True)
    return df


def save(fig, bench_dir, name):
    for ext in ("pdf", "png"):
        out = os.path.join(bench_dir, f"{name}.{ext}")
        fig.savefig(out, dpi=300, bbox_inches="tight")
        print(f"wrote {out}")
    plt.close(fig)


# ---------------------------------------------------------------- fig1
def fig1_throughput_sweep(df, bench_dir):
    d = df[df["workload"] == "sweep"]
    if d.empty:
        print("fig1: no sweep rows; skipped")
        return
    agg = (d.groupby(["mode", "payload_bytes"], observed=True)
             ["throughput_mbps"].mean().reset_index())

    fig, ax = plt.subplots(figsize=(6.4, 4.0))
    for color, mode in zip(PALETTE, MODE_ORDER):
        sub = agg[agg["mode"] == mode].sort_values("payload_bytes")
        if sub.empty:
            continue
        ax.plot(sub["payload_bytes"], sub["throughput_mbps"],
                marker="o", ms=3.5, lw=1.4, color=color,
                label=MODE_LABEL[mode])
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xlabel("Payload size (bytes)")
    ax.set_ylabel(r"Throughput (MB/s, log scale)")
    ax.set_xticks([64 << k for k in range(0, 19, 2)])
    ax.set_xticklabels(["64B", "256B", "1KB", "4KB", "16KB", "64KB",
                        "256KB", "1MB", "4MB", "16MB"])
    ax.legend(frameon=True)
    fig.tight_layout()
    save(fig, bench_dir, "fig1_throughput_sweep")


# ---------------------------------------------------------------- fig2
def fig2_latency_cdf(df, bench_dir):
    d = df[(df["workload"] == "bimodal")]
    if d.empty:
        print("fig2: no bimodal rows; skipped")
        return

    fig, ax = plt.subplots(figsize=(6.4, 4.0))
    # Two panels of classes would clutter; plot CDF per mode over all
    # messages, plus a per-class split for AdaptIPC.
    for color, mode in zip(PALETTE, MODE_ORDER):
        lat = d.loc[d["mode"] == mode, "latency_ns"].sort_values()
        if lat.empty:
            continue
        cdf = lat.rank(method="first") / len(lat)
        ax.plot(lat, cdf, lw=1.4, color=color, label=MODE_LABEL[mode])

    ax.set_xscale("log")
    ax.set_xlabel("One-way latency (ns, log scale)")
    ax.set_ylabel("CDF")
    ax.set_ylim(0, 1.02)
    ax.legend(frameon=True, loc="lower right", title="Bimodal workload")
    fig.tight_layout()
    save(fig, bench_dir, "fig2_latency_cdf")


# ---------------------------------------------------------------- fig3
def _read_summary_truth(bench_dir):
    """Parse benchmarks/summary.txt for the authoritative iteration count
    and switch count of the adapt/thrash run. Returns (msgs, switches)
    or (None, None) if unavailable."""
    import re as _re
    path = os.path.join(bench_dir, "summary.txt")
    try:
        with open(path) as f:
            log = f.read()
    except OSError:
        return None, None
    m = _re.search(
        r"\[adapt/thrash\].*?msgs=(\d+).*?route_switches=(\d+)", log)
    if not m:
        return None, None
    return int(m.group(1)), int(m.group(2))


def fig3_hysteresis_adaptation(df, bench_dir, max_points=4000):
    # Fig. 3 depicts the *thrash* workload specifically: 100k iterations
    # oscillating inside the EWMA deadband. Do NOT concatenate other
    # workloads here -- cross-workload boundaries would inject phantom
    # "switches" into a naive diff() and distort the x-axis scale.
    d = df[(df["workload"] == "thrash") & (df["mode"] == "adapt")].copy()
    if d.empty:
        print("fig3: no adapt/thrash rows; skipped")
        return

    n_iters = len(d)
    # True per-message index within the workload (preserved even when we
    # downsample for rendering).
    d["msg_idx"] = range(n_iters)

    switches = int((d["route_taken"].diff().fillna(0) != 0).sum())

    # The CSV collapses consecutive equal-size samples into single rows
    # (see benchmark_suite.c aggregate writer), so len(d) UNDERCOUNTS the
    # true iteration count. Prefer the authoritative summary.txt values;
    # fall back to row counts only if the log is unavailable.
    truth_msgs, truth_switches = _read_summary_truth(bench_dir)
    if truth_msgs is not None and truth_msgs >= n_iters:
        if switches != truth_switches:
            print(f"fig3: WARNING csv-derived switches={switches} != "
                  f"summary.txt route_switches={truth_switches}; "
                  f"using summary.txt (authoritative)")
        total_iters, title_switches = truth_msgs, truth_switches
    else:
        print("fig3: WARNING summary.txt missing; falling back to "
              "CSV row counts")
        total_iters, title_switches = n_iters, switches

    if len(d) > max_points:
        step = len(d) // max_points
        d_plot = d.iloc[::step]
    else:
        d_plot = d

    fig, ax1 = plt.subplots(figsize=(7.2, 3.8))
    ax1.step(d_plot["msg_idx"], d_plot["payload_bytes"], where="post",
             color=PALETTE[0], lw=0.8, label="payload size (bytes)")
    ax1.set_yscale("log")
    ax1.set_xlabel(f"Message index (thrash workload, "
                   f"{total_iters:,} iterations)")
    ax1.set_ylabel("Payload size (bytes, log)")

    ax2 = ax1.twinx()
    route_y = d_plot["route_taken"].map(ROUTE_LABEL)
    ax2.step(d_plot["msg_idx"], route_y, where="post",
             color=PALETTE[2], lw=1.6, alpha=0.85, label="active EWMA route")
    ax2.set_yticks(["UDS", "SHM"])
    ax2.set_ylabel("Active route")

    lines1, labels1 = ax1.get_legend_handles_labels()
    lines2, labels2 = ax2.get_legend_handles_labels()
    ax1.legend(lines1 + lines2, labels1 + labels2,
               frameon=True, loc="upper left")
    ax1.set_title(f"AdaptIPC deadband stability under thrash workload "
                  f"({title_switches} EWMA route switches in "
                  f"{total_iters:,} iterations)")
    fig.tight_layout()
    save(fig, bench_dir, "fig3_hysteresis_adaptation")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench-dir", default="benchmarks")
    args = ap.parse_args()

    sweep = load_csv(os.path.join(args.bench_dir, "results_sweep.csv"))
    bimodal = load_csv(os.path.join(args.bench_dir, "results_bimodal.csv"))

    fig1_throughput_sweep(sweep, args.bench_dir)
    fig2_latency_cdf(bimodal, args.bench_dir)
    fig3_hysteresis_adaptation(bimodal, args.bench_dir)


if __name__ == "__main__":
    main()
