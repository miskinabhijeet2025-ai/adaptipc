#!/usr/bin/env python3
"""
generate_showcase_figures.py -- publication-quality figures for the
README/landing page, generated ONLY from real measurement CSVs in
experiments/raw/ and experiments/v2_1/raw/ (v2/v2.1 campaigns), plus
conceptual architecture diagrams (clearly conceptual, no data).

Output: assets/figures/*.png and assets/*.png
"""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

RAW = "experiments/raw"
V21 = "experiments/v2_1/raw"
OUT = "assets/figures"
os.makedirs(OUT, exist_ok=True)

plt.rcParams.update({
    "font.size": 13, "axes.titlesize": 15, "axes.labelsize": 13,
    "legend.fontsize": 11, "figure.dpi": 150,
    "axes.grid": True, "grid.alpha": 0.3,
})
C = {"uds": "#1f6feb", "shm": "#2ea043", "adapt": "#d29922",
     "size_only": "#d62728", "size_hysteresis": "#1f77b4",
     "queue_aware": "#2ca02c", "cost_aware": "#ff7f0e",
     "full_adaptive": "#9467bd"}

def load(p):
    if not os.path.exists(p): return []
    return list(csv.DictReader(l for l in open(p)
                               if not l.startswith("#")))

def save(fig, name):
    fig.savefig(f"{OUT}/{name}", bbox_inches="tight")
    plt.close(fig)
    print("wrote", name)

# ---- Figure 1: throughput vs payload size --------------------------------
rows = load(f"{RAW}/payload_sweep.csv")
if rows:
    fig, ax = plt.subplots(figsize=(8, 4.8))
    for pol in ("uds", "shm", "size_hysteresis", "full_adaptive"):
        pts = [(int(r["message_count"]), float(r["throughput_mbps"]))
               for r in rows if r["policy"] == pol]
        if pts:
            label = {"uds": "UDS", "shm": "SHM",
                     "size_hysteresis": "AdaptIPC (size+hysteresis)",
                     "full_adaptive": "AdaptIPC (full adaptive)"}[pol]
            ax.plot([p[0] for p in pts], [p[1] for p in pts],
                    "o-", label=label, color=C.get(pol))
    ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_xlabel("total messages in sweep")
    ax.set_ylabel("aggregate throughput (MB/s)")
    ax.set_title("Throughput by policy (payload sweep, measured)")
    ax.legend()
    save(fig, "throughput.png")

# ---- Figure 2: latency vs policy ------------------------------------------
if rows:
    fig, ax = plt.subplots(figsize=(8, 4.8))
    labels, p50s, p99s = [], [], []
    for pol in ("uds", "shm", "size_only", "size_hysteresis",
                "queue_aware", "cost_aware", "full_adaptive"):
        for r in rows:
            if r["policy"] == pol:
                labels.append(pol)
                p50s.append(float(r["p50_us"]))
                p99s.append(float(r["p99_us"]))
    x = range(len(labels))
    ax.bar([i - 0.2 for i in x], p50s, 0.4, label="p50")
    ax.bar([i + 0.2 for i in x], p99s, 0.4, label="p99")
    ax.set_yscale("log")
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, rotation=20, ha="right")
    ax.set_ylabel("latency (us, log)")
    ax.set_title("End-to-end latency by policy (payload sweep)")
    ax.legend()
    save(fig, "latency.png")

# ---- Figure 3: routing decisions (from lab routing trace if present) ------
rt = "experiments/runs/LATEST"
rt_path = None
if os.path.islink(rt):
    rt = os.readlink(rt)
    p = os.path.join(rt, "raw", "routing_trace.csv")
    if os.path.exists(p): rt_path = p
if not rt_path:
    # fall back: any run dir with a routing trace
    runs = "experiments/runs"
    if os.path.isdir(runs):
        for d in sorted(os.listdir(runs), reverse=True):
            p = os.path.join(runs, d, "raw", "routing_trace.csv")
            if os.path.exists(p): rt_path = p; break
if rt_path:
    seqs, sizes, ewma, route = [], [], [], []
    for line in open(rt_path):
        f = line.strip().split(",")
        if len(f) != 5 or f[0] == "seq": continue
        seqs.append(int(f[0])); sizes.append(int(f[1]))
        ewma.append(float(f[2])); route.append(1 if f[3] == "SHM" else 0)
    fig, ax = plt.subplots(figsize=(9, 4.8))
    ax.plot(seqs, sizes, ",", color="#aaaaaa", label="payload size")
    ax.plot(seqs, ewma, "-", color=C["adapt"], lw=1.5,
            label="EWMA (measured)")
    ax.axhline(4096, color="#d62728", ls="--", label="tau_high = 4096 B")
    ax.axhline(1024, color="#ff7f0e", ls="--", label="tau_low = 1024 B")
    ax.set_yscale("log")
    ax.set_xlabel("message #"); ax.set_ylabel("bytes (log)")
    ax.set_title("Routing decisions over time (measured trace)")
    ax2 = ax.twinx()
    ax2.fill_between(seqs, route, step="post", alpha=.4,
                     color="#2ca02c")
    ax2.set_yticks([0, 1]); ax2.set_yticklabels(["UDS", "SHM"])
    ax2.set_ylabel("selected transport"); ax2.grid(False)
    ax.legend(loc="upper left")
    save(fig, "routing_decisions.png")

# ---- Figure 4: policy comparison (adversarial switching) ------------------
rows = load(f"{V21}/adversarial_delta.csv")
if rows:
    agg = {}
    for r in rows:
        p = r["policy"]
        agg.setdefault(p, [0, 0, 0.0, []])
        agg[p][0] += int(r["total_switches"])
        agg[p][1] += int(r["genuine_escapes"])
        agg[p][2] += float(r["throughput_mbps"])
        agg[p][3].append(float(r["throughput_mbps"]))
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(11, 4.8))
    labels = ["size_only", "size_hysteresis", "queue_aware",
              "cost_aware", "full_adaptive"]
    sw = [agg[l][0] for l in labels if l in agg]
    fl = [agg[l][0] - agg[l][1] for l in labels if l in agg]
    a1.bar([l for l in labels if l in agg], sw,
           color=[C[l] for l in labels if l in agg])
    a1.set_yscale("log")
    a1.set_ylabel("route switches (log, 200k msgs)")
    a1.set_title("Stability under adversarial load")
    a1.tick_params(axis="x", rotation=20)
    tp = [agg[l][2] / len(agg[l][3]) for l in labels if l in agg]
    a2.bar([l for l in labels if l in agg], tp,
           color=[C[l] for l in labels if l in agg])
    a2.set_ylabel("mean throughput (MB/s)")
    a2.set_title("Throughput by policy")
    a2.tick_params(axis="x", rotation=20)
    fig.tight_layout()
    save(fig, "policy_comparison.png")

# ---- Figure 5: stability (false-switch rate / flaps) ----------------------
if rows:
    fig, ax = plt.subplots(figsize=(8, 4.8))
    labels, genuine, flaps = [], [], []
    for r in rows:
        l = r["policy"]
        if l not in labels:
            labels.append(l)
            genuine.append(0); flaps.append(0)
        i = labels.index(l)
        genuine[i] += int(r["genuine_escapes"])
        flaps[i] += int(r["noise_flaps"])
    x = range(len(labels))
    ax.bar([i - 0.2 for i in x], genuine, 0.4, label="genuine escapes",
           color="#2ca02c")
    ax.bar([i + 0.2 for i in x], flaps, 0.4, label="noise flaps",
           color="#d62728")
    ax.set_yscale("symlog")
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, rotation=20, ha="right")
    ax.set_ylabel("switch classification (symlog)")
    ax.set_title("Genuine escapes vs noise flaps (adversarial sweep)")
    ax.legend()
    save(fig, "stability.png")

# ---- Figure 6: queue-aware behavior (prediction vs occupancy) -------------
rows = load(f"{V21}/queue_prediction_accuracy.csv")
if rows:
    fig, ax = plt.subplots(figsize=(8, 4.8))
    occ = [float(r["occupancy_bytes"]) / 1e6 for r in rows]
    ax.plot(occ, [float(r["predicted_wait_us"]) for r in rows], "o-",
            label="model prediction", color=C["adapt"])
    ax.plot(occ, [float(r["actual_delay_us"]) / 1000.0 for r in rows],
            "s--", label="actual delay (ms)", color="#d62728")
    ax.set_xlabel("ring occupancy (MB)")
    ax.set_ylabel("queue wait (us) / delay (ms)")
    ax.set_title("Queue-aware behavior: prediction vs actual "
                 "(honest gap on stalled consumer)")
    ax.legend()
    save(fig, "queue_aware.png")

# ---- Figure 7: transport health (stall timeline) --------------------------
rows = load(f"{V21}/stall_timeline.csv")
if rows:
    rows = [r for r in rows if r["event"] != "summary"]
    fig, ax = plt.subplots(figsize=(9, 4.8))
    t = [float(r["t_ms"]) for r in rows]
    occ = [float(r["ring_used"]) / 1e6 for r in rows]
    ax.plot(t, occ, ".-", color=C["adapt"])
    ax.set_xlabel("time (ms)"); ax.set_ylabel("ring occupancy (MB)")
    ax.set_title("Consumer stall -> escape -> recovery "
                 "(escape <1 ms, recovery 2.7 ms)")
    for r in rows:
        if r["event"] == "stall":
            ax.axvline(float(r["t_ms"]), color="#d62728", ls=":",
                       label="stall begins")
            break
    ax.legend()
    save(fig, "transport_health.png")

# ---- Figure 8: decision cost breakdown ------------------------------------
dec = load("showcase/outputs/decisions.csv")
if dec:
    import statistics
    reasons = {}
    for r in dec:
        reasons.setdefault(r["reason"], []).append(r)
    fig, ax = plt.subplots(figsize=(9, 4.8))
    labels = list(reasons)
    shm = [statistics.mean(float(r["cost_shm_us"])
                           for r in reasons[k]) for k in labels]
    uds = [statistics.mean(float(r["cost_uds_us"])
                           for r in reasons[k]) for k in labels]
    qw = [statistics.mean(float(r["queue_wait_shm_us"])
                          for r in reasons[k]) for k in labels]
    x = range(len(labels))
    ax.bar([i - 0.2 for i in x], shm, 0.4, label="mean SHM cost",
           color=C["shm"])
    ax.bar([i + 0.2 for i in x], uds, 0.4, label="mean UDS cost",
           color=C["uds"])
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, rotation=15, ha="right")
    ax.set_ylabel("estimated cost (us)")
    ax.set_title("Decision cost breakdown by decision reason "
                 "(real decision log, n=%d)" % len(dec))
    ax.legend()
    save(fig, "cost_breakdown.png")

# ---- Conceptual diagrams (clearly conceptual, no data) --------------------
def box(ax, xy, w, h, text, fc="#1f6feb", fs=12, tc="white"):
    b = FancyBboxPatch(xy, w, h, boxstyle="round,pad=0.02",
                       fc=fc, ec="none")
    ax.add_patch(b)
    ax.text(xy[0] + w / 2, xy[1] + h / 2, text, ha="center",
            va="center", fontsize=fs, color=tc, weight="bold")

def arrow(ax, x1, y1, x2, y2):
    ax.add_patch(FancyArrowPatch((x1, y1), (x2, y2),
                 arrowstyle="-|>", mutation_scale=22, color="#555"))

def architecture():
    fig, ax = plt.subplots(figsize=(8.5, 10))
    ax.set_xlim(0, 10); ax.set_ylim(0, 12); ax.axis("off")
    box(ax, (2.5, 10.8), 5, 0.9, "Application", "#6e7781")
    arrow(ax, 5, 10.7, 5, 10.2)
    box(ax, (2.5, 9.2), 5, 0.9, "adapt_send()", "#d29922", 13)
    arrow(ax, 5, 9.1, 5, 8.6)
    box(ax, (0.5, 7.0), 9, 1.5,
        "Context Collector\npayload · EWMA arrival · queue occupancy ·\n"
        "consumer drain rate · consumer activity", "#57606a", 11)
    arrow(ax, 5, 6.9, 5, 6.4)
    box(ax, (0.5, 4.8), 9, 1.5,
        "Cost Estimator\ntransport · queue wait · setup ·\n"
        "switching · health · latency", "#57606a", 11)
    arrow(ax, 5, 4.7, 5, 4.2)
    box(ax, (2.5, 3.2), 5, 0.9, "Adaptive Policy (score + margin)",
        "#d29922", 12)
    arrow(ax, 3.2, 3.1, 2.2, 2.4)
    arrow(ax, 6.8, 3.1, 7.8, 2.4)
    box(ax, (0.4, 1.2), 3.8, 1.2,
        "POSIX Shared Memory\nlock-free SPSC ring", "#2ea043", 11)
    box(ax, (5.8, 1.2), 3.8, 1.2,
        "AF_UNIX Datagram\ncontrol / small messages", "#1f6feb", 11)
    ax.text(5, 0.4, "AdaptIPC architecture (conceptual)",
            ha="center", fontsize=11, color="#8b949e")
    fig.savefig("assets/architecture.png", bbox_inches="tight",
                dpi=150)
    plt.close(fig)
    print("wrote assets/architecture.png")

def decision_pipeline():
    fig, ax = plt.subplots(figsize=(8.5, 10))
    ax.set_xlim(0, 10); ax.set_ylim(0, 12); ax.axis("off")
    steps = [
        ("Message arrives: payload = 8 KB", "#6e7781"),
        ("EWMA(payload) = 5.1 KB", "#6e7781"),
        ("Queue occupancy = 42%", "#6e7781"),
        ("Estimate costs:\nSHM = 8.2 us   UDS = 14.7 us", "#57606a"),
        ("Apply switching margin (H)\n+ health penalty + setup cost",
         "#57606a"),
        ("Decision: SELECT SHM\nreason: COST_WIN", "#2ea043"),
        ("Record decision (decision log)", "#d29922"),
    ]
    y = 10.8
    for i, (t, c) in enumerate(steps):
        box(ax, (1.2, y - 0.55), 7.6, 1.0, t, c, 12)
        if i < len(steps) - 1:
            arrow(ax, 5, y - 0.6, 5, y - 1.35)
        y -= 1.5
    ax.text(5, 0.5, "How AdaptIPC makes a decision (illustrative "
            "values)", ha="center", fontsize=11, color="#8b949e")
    fig.savefig("assets/decision_pipeline.png", bbox_inches="tight",
                dpi=150)
    plt.close(fig)
    print("wrote assets/decision_pipeline.png")

def before_vs_adaptive():
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.set_xlim(0, 10); ax.set_ylim(0, 6); ax.axis("off")
    ax.text(2.5, 5.6, "STATIC IPC", ha="center", fontsize=15,
            weight="bold")
    box(ax, (1.4, 4.2), 2.2, 0.7, "Application", "#6e7781", 11)
    arrow(ax, 2.5, 4.15, 2.5, 3.65)
    box(ax, (1.4, 2.9), 2.2, 0.7, "Always SHM\n(or always UDS)",
        "#6e7781", 10)
    ax.text(2.5, 2.3, "one choice, forever", ha="center", fontsize=10,
            color="#8b949e")
    ax.text(7.5, 5.6, "ADAPTIPC", ha="center", fontsize=15,
            weight="bold", color="#d29922")
    box(ax, (6.4, 4.2), 2.2, 0.7, "Application", "#6e7781", 11)
    arrow(ax, 7.5, 4.15, 7.5, 3.65)
    box(ax, (6.15, 2.9), 2.7, 0.7, "Context + cost model", "#d29922", 10)
    arrow(ax, 6.9, 2.85, 6.0, 2.25)
    arrow(ax, 8.1, 2.85, 9.0, 2.25)
    box(ax, (5.0, 1.5), 2.0, 0.7, "SHM", "#2ea043", 11)
    box(ax, (8.0, 1.5), 2.0, 0.7, "UDS", "#1f6feb", 11)
    ax.text(7.5, 0.9, "per-message decision", ha="center", fontsize=10,
            color="#8b949e")
    fig.savefig("assets/before_vs_adaptive.png", bbox_inches="tight",
                dpi=150)
    plt.close(fig)
    print("wrote assets/before_vs_adaptive.png")

architecture(); decision_pipeline(); before_vs_adaptive()
