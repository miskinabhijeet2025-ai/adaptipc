#!/usr/bin/env python3
"""v2.1 figures -- generated from experiments/v2_1/raw/*.csv only."""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

RAW = "experiments/v2_1/raw"
FIG = "experiments/v2_1/figures"

def load(name):
    p = os.path.join(RAW, name)
    if not os.path.exists(p): return []
    return list(csv.DictReader(l for l in open(p) if not l.startswith("#")))

def save(fig, name):
    os.makedirs(FIG, exist_ok=True)
    fig.savefig(os.path.join(FIG, name), dpi=150, bbox_inches="tight")
    plt.close(fig)
    print("wrote", name)

# fig3: occupancy validation (reported vs nominal)
rows = load("occupancy_validation.csv")
if rows:
    x = [int(r["pct"]) for r in rows]
    fig, ax = plt.subplots(figsize=(6,4))
    ax.plot(x, [int(r["nominal_bytes"])/1e6 for r in rows], "o-",
            label="nominal target")
    ax.plot(x, [int(r["reported_bytes"])/1e6 for r in rows], "s--",
            label="instrumented")
    ax.plot(x, [int(r["cursor_bytes"])/1e6 for r in rows], "^:",
            label="cursor math (independent)")
    ax.set_xlabel("fill target (%)"); ax.set_ylabel("occupancy (MB)")
    ax.set_title("Queue-occupancy instrumentation vs independent cursor math")
    ax.legend(); ax.grid(alpha=.3)
    save(fig, "fig3_queue_occupancy_validation.png")

# fig4: stability margin
rows = load("stability_margin.csv")
if rows:
    x = [float(r["H_us"]) for r in rows]
    fig, ax = plt.subplots(figsize=(6,4))
    ax.plot(x, [int(r["total_switches"]) for r in rows], "o-",
            label="total switches")
    ax.plot(x, [int(r["noise_flaps"]) for r in rows], "x--",
            label="noise flaps")
    ax.set_xlabel("hysteresis margin H (us)")
    ax.set_ylabel("switch count (2000 adversarial msgs)")
    ax.set_title("Stability margin sweep: 0 switches at every H")
    ax.legend(); ax.grid(alpha=.3)
    save(fig, "fig4_stability_margin.png")

# fig5: stall timeline
rows = load("stall_timeline.csv")
if rows:
    rows = [r for r in rows if r["event"] != "summary"]
    xs = [float(r["t_ms"]) for r in rows]
    ys = [float(r["ring_used"])/1e6 for r in rows]
    fig, ax = plt.subplots(figsize=(7,4))
    ax2 = ax.twinx()
    ev = [(float(r["t_ms"]), r["event"]) for r in rows if r["event"] in
          ("normal","stall","recovery","summary")]
    ax.plot(xs, ys, ".-", label="ring occupancy (MB)")
    ax.set_xlabel("time (ms)"); ax.set_ylabel("ring occupancy (MB)")
    ax2.set_ylabel("phase")
    phases = {"normal":0,"stall":1,"recovery":2}
    if ev: ax2.plot([e[0] for e in ev], [phases.get(e[1],3) for e in ev],
                    "r+", label="phase")
    ax2.set_yticks([0,1,2]); ax2.set_yticklabels(["normal","stall","recovery"])
    ax.set_title("Consumer stall: escape and recovery timeline")
    ax.legend(loc="upper left"); ax2.legend(loc="lower right")
    save(fig, "fig5_stall_escape_recovery.png")

# fig7: adversarial switching by policy
rows = load("adversarial_delta.csv")
if rows:
    pol = {}
    for r in rows:
        pol.setdefault(r["policy"], [0,0])
        pol[r["policy"]][0] += int(r["total_switches"])
        pol[r["policy"]][1] += int(r["noise_flaps"])
    labels = list(pol)
    fig, ax = plt.subplots(figsize=(7,4))
    ax.bar(labels, [pol[l][0] for l in labels],
           color=["#999","#1f77b4","#2ca02c","#ff7f0e","#d62728"])
    ax.set_yscale("log")
    ax.set_ylabel("switches (log; 200k adversarial msgs)")
    ax.set_title("Adversarial switching: size_only thrashes, policies stable")
    ax.tick_params(axis="x", rotation=20); ax.grid(alpha=.3, axis="y")
    save(fig, "fig7_adversarial_switching.png")

# fig9: crossover learning
rows = load("crossover_sweep.csv")
if rows:
    x = [float(r["true_crossover_b"]) for r in rows]
    y = [float(r["learned_b"]) for r in rows]
    fig, ax = plt.subplots(figsize=(5.5,4.5))
    ax.plot(x, x, "k--", label="ideal")
    ax.plot(x, y, "o", label="learned S*")
    ax.set_xlabel("true crossover (B)"); ax.set_ylabel("learned S* (B)")
    ax.set_title("Self-calibrating crossover: learned vs ground truth")
    ax.legend(); ax.grid(alpha=.3)
    save(fig, "fig9_crossover_learning.png")

# fig11: pareto (from v2 raw queue_pressure)
p = "experiments/raw/queue_pressure.csv"
if os.path.exists(p):
    rows = list(csv.DictReader(open(p)))
    fig, ax = plt.subplots(figsize=(7,4.5))
    for r in rows:
        ax.scatter(float(r["p99_us"]), float(r["throughput_mbps"]),
                   s=40 + float(r["max_queue_occupancy_bytes"])/4096,
                   label=r["policy"])
    ax.set_xscale("log")
    ax.set_xlabel("p99 latency (us, log)")
    ax.set_ylabel("throughput (MB/s)")
    ax.set_title("Pareto: throughput vs p99 (point size = max backlog)")
    ax.legend(fontsize=8); ax.grid(alpha=.3)
    save(fig, "fig11_pareto.png")

# fig12: qos (from v2 raw qos.csv)
p = "experiments/raw/qos.csv"
if os.path.exists(p):
    rows = list(csv.DictReader(open(p)))
    labels = [r["policy"] for r in rows]
    p99 = [float(r["p99_us"]) for r in rows]
    shmf = [float(r["shm_messages"])/max(1,int(r["shm_messages"])+int(r["uds_messages"])) for r in rows]
    fig, ax = plt.subplots(figsize=(7,4))
    ax.bar(labels, p99, color="#1f77b4")
    ax.set_ylabel("p99 (us)"); ax.set_yscale("log")
    ax.set_title("QoS postures change routing (v2 qos experiment)")
    ax.tick_params(axis="x", rotation=20); ax.grid(alpha=.3, axis="y")
    save(fig, "fig12_qos.png")
