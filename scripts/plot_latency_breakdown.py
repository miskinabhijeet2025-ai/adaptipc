#!/usr/bin/env python3
"""plot_latency_breakdown.py -- Fig. 'where the time goes' for AdaptIPC.

Reads:  benchmarks/latency_breakdown_{sweep,bimodal}.txt          (receiver)
        benchmarks/latency_breakdown_{sweep,bimodal}_producer.txt (sender)
        benchmarks/summary.txt                                    (medians)
Writes: benchmarks/fig5_latency_breakdown.{pdf,png}

Grouped log-scale bars per workload:
  - router cost per message   (producer: router_ewma_classify_mean_ns)
  - poll-wait per delivery    (consumer: poll_wait_total_ns / recv_msgs)
  - end-to-end median latency (summary.txt median_lat)
Shows that component costs are tiny relative to end-to-end median on the
bimodal workload -- i.e. queueing delay dominates, not per-message overhead.
"""
import os
import re

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.join(HERE, "..", "benchmarks")
OUT = os.path.join(BENCH, "fig5_latency_breakdown")


def parse_consumer(path):
    d = {}
    for line in open(path):
        for key in ("poll_wait_total_ns", "recv_msgs", "poll_timeouts_2ms",
                    "delivered_shm_first_poll", "delivered_uds_prompt"):
            m = re.match(key + r"=(\d+)", line.strip())
            if m:
                d[key] = int(m.group(1))
    return d


def parse_producer(path):
    d = {}
    for line in open(path):
        m = re.match(r"router_ewma_classify_mean_ns=(\d+)", line.strip())
        if m:
            d["router_mean_ns"] = int(m.group(1))
    return d


def summary_median(path, wl, mode):
    pat = re.compile(r"\[%s/%s\].*?median_lat=(\d+) ns" % (mode, wl))
    for line in open(path):
        m = pat.search(line)
        if m:
            return int(m.group(1))
    return None


def main():
    data = {}
    for wl in ("sweep", "bimodal"):
        cons = parse_consumer(os.path.join(
            BENCH, f"latency_breakdown_{wl}.txt"))
        prod = parse_producer(os.path.join(
            BENCH, f"latency_breakdown_{wl}_producer.txt"))
        delivered = cons.get("delivered_shm_first_poll", 0) + \
            cons.get("delivered_shm_later", 0) + \
            cons.get("delivered_uds_prompt", 0) + \
            cons.get("delivered_uds_later", 0)
        poll_per_msg = (cons.get("poll_wait_total_ns", 0) /
                        max(delivered, 1))
        data[wl] = {
            "router": prod.get("router_mean_ns", 0),
            "poll": poll_per_msg,
            "e2e": summary_median(os.path.join(BENCH, "summary.txt"),
                                  wl, "adapt"),
        }

    fig, ax = plt.subplots(figsize=(5.2, 2.9))
    wls = ["sweep", "bimodal"]
    x = range(len(wls))
    width = 0.25
    series = [("router cost/msg", [data[w]["router"] for w in wls], "#1f77b4"),
              ("poll wait/delivery", [data[w]["poll"] for w in wls],
               "#ff7f0e"),
              ("end-to-end median", [data[w]["e2e"] for w in wls], "#d62728")]
    for i, (label, vals, color) in enumerate(series):
        pos = [xi + (i - 1) * width for xi in x]
        ax.bar(pos, vals, width * 0.9, color=color, label=label)
        for xi, v in zip(pos, vals):
            ax.text(xi, v * 1.15, f"{v:,.0f}", ha="center", fontsize=6.5)

    ax.set_yscale("log")
    ax.set_ylim(1, 3e7)
    ax.set_xticks(list(x))
    ax.set_xticklabels([w.capitalize() for w in wls])
    ax.set_ylabel("nanoseconds (log scale)")
    ax.legend(fontsize=7.5, loc="upper left")
    ax.tick_params(labelsize=8)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(f"{OUT}.{ext}", dpi=300)
        print(f"wrote {OUT}.{ext}")
    plt.close(fig)


if __name__ == "__main__":
    main()
