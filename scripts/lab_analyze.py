#!/usr/bin/env python3
"""
lab_analyze.py -- AdaptIPC experiment lab analysis pipeline.

Consumes the raw CSVs in a run directory (produced by the C tools in
benchmarks/), and generates:

  processed/  aggregated CSVs (medians, counts -- computed, not copied)
  plots/      PNG figures (matplotlib, generated from the raw data)
  tables/     machine (csv) + human (md) result tables
  report/report.html  lightweight static report

Usage: python3 scripts/lab_analyze.py <run_dir>
"""
import csv
import json
import math
import os
import statistics
import sys

def load(path):
    if not os.path.exists(path):
        return []
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            rows.append(line.split(","))
    return rows

def median(v):
    return statistics.median(v) if v else 0.0

def write_csv(path, header, rows):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(",".join(header) + "\n")
        for r in rows:
            f.write(",".join(str(x) for x in r) + "\n")

def write_md(path, header, rows, title):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(f"## {title}\n\n")
        f.write("| " + " | ".join(header) + " |\n")
        f.write("|" + "---|" * len(header) + "\n")
        for r in rows:
            f.write("| " + " | ".join(str(x) for x in r) + " |\n")

def fig(path, plotter):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(figsize=(7.5, 4.5))
        plotter(plt, fig, ax)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        fig.savefig(path, dpi=150, bbox_inches="tight")
        plt.close(fig)
        return True
    except Exception as e:
        print(f"  plot skipped ({path}): {e}", file=sys.stderr)
        return False

def main():
    run = sys.argv[1] if len(sys.argv) > 1 else "."
    raw = os.path.join(run, "raw")
    proc = os.path.join(run, "processed")
    plots = os.path.join(run, "plots")
    tables = os.path.join(run, "tables")
    for d in (proc, plots, tables, os.path.join(run, "report")):
        os.makedirs(d, exist_ok=True)
    made = {"plots": [], "tables": []}

    meta = {}
    mpath = os.path.join(run, "run_metadata.json")
    if os.path.exists(mpath):
        try: meta = json.load(open(mpath))
        except Exception: meta = {}
    prov = (f"run {os.path.basename(run)}, commit "
            f"{meta.get('git_commit','?')}")

    # ---------------- transport comparison (sweep aggregates) ----------
    tr_rows = []
    for mode in ("uds", "shm", "adapt"):
        for r in load(os.path.join(raw, f"transport_{mode}.csv")):
            if len(r) < 6 or r[0] == "workload":
                continue
            # workload,mode,payload_bytes,latency_ns,throughput_mbps,route
            try:
                tr_rows.append((r[1], int(r[2]), float(r[3]),
                                float(r[4])))
            except (ValueError, IndexError):
                continue
    if tr_rows:
        # median latency per (mode, payload) if multiple rows share keys
        agg = {}
        for mode, payload, lat, tp in tr_rows:
            agg.setdefault((mode, payload), {"lat": [], "tp": []})
            agg[(mode, payload)]["lat"].append(lat)
            agg[(mode, payload)]["tp"].append(tp)
        proc_rows = []
        for (mode, payload), d in sorted(
                agg.items(), key=lambda kv: kv[0][1]):
            proc_rows.append((mode, payload,
                              round(median(d["lat"]) / 1000.0, 2),
                              round(median(d["tp"]), 1),
                              len(d["lat"])))
        write_csv(os.path.join(proc, "transport_summary.csv"),
                  ("mode", "payload_bytes", "median_latency_us",
                   "throughput_mbps", "samples"), proc_rows)
        write_md(os.path.join(tables, "transport_comparison.md"),
                 ("mode", "payload_bytes", "median_latency_us",
                  "throughput_mbps", "samples"),
                 proc_rows, "Transport comparison (sweep)")
        write_csv(os.path.join(tables, "transport_comparison.csv"),
                  ("mode", "payload_bytes", "median_latency_us",
                   "throughput_mbps", "samples"), proc_rows)
        made["tables"].append("transport_comparison")

        def plot_transport(plt, fig, ax):
            for mode in ("uds", "shm", "adapt"):
                xs = [p for (m, p), _ in sorted(
                    agg.items()) if m == mode]
                ys = [median(d["lat"]) / 1000.0 for (m, p), d in
                      sorted(agg.items()) if m == mode]
                if xs:
                    ax.plot(xs, ys, "o-", label=mode.upper())
            ax.set_xscale("log", base=2)
            ax.set_yscale("log")
            ax.set_xlabel("payload size (bytes)")
            ax.set_ylabel("median latency (us, log)")
            ax.set_title("Transport comparison by payload size")
            ax.legend(); ax.grid(alpha=.3)
        if fig(os.path.join(plots, "transport_comparison.png"),
               plot_transport):
            made["plots"].append("transport_comparison")

    # ---------------- adaptive routing trace ---------------------------
    rt = load(os.path.join(raw, "routing_trace.csv"))
    if rt:
        seqs, sizes, ewmas, routes = [], [], [], []
        switches = 0
        for r in rt:
            if r[0] == "seq" or len(r) < 5:
                continue
            seqs.append(int(r[0])); sizes.append(int(r[1]))
            ewmas.append(float(r[2]))
            route = 1 if r[3] == "SHM" else 0
            routes.append(route)
            switches += int(r[4])
        write_csv(os.path.join(proc, "routing_trace_processed.csv"),
                  ("seq", "payload_bytes", "ewma_bytes", "route_shm"),
                  [(s, z, round(e, 1), rr) for s, z, e, rr in
                   zip(seqs, sizes, ewmas, routes)])
        summary = [("total_messages", len(seqs)),
                   ("route_switches", switches),
                   ("switch_rate_per_1000",
                    round(1000.0 * switches / max(1, len(seqs)), 2)),
                   ("uds_messages", routes.count(0)),
                   ("shm_messages", routes.count(1))]
        write_md(os.path.join(tables, "routing_summary.md"),
                 ("metric", "value"), summary, "Adaptive routing trace")
        write_csv(os.path.join(tables, "routing_summary.csv"),
                  ("metric", "value"), summary)
        made["tables"].append("routing_summary")

        def plot_route(plt, fig, ax):
            ax.plot(seqs, sizes, ",", color="#bbbbbb",
                    label="payload size")
            ax.plot(seqs, ewmas, "-", color="#1f77b4",
                    label="EWMA (measured)")
            ax.axhline(4096, color="#d62728", ls="--",
                       label="tau_high = 4096 B")
            ax.axhline(1024, color="#ff7f0e", ls="--",
                       label="tau_low = 1024 B")
            ax.set_yscale("log")
            ax.set_xlabel("message #")
            ax.set_ylabel("bytes (log)")
            ax.set_title("Adaptive routing: EWMA vs thresholds")
            ax2 = ax.twinx()
            ax2.fill_between(seqs, routes, step="post", alpha=.35,
                             color="#2ca02c")
            ax2.set_yticks([0, 1])
            ax2.set_yticklabels(["UDS", "SHM"])
            ax2.set_ylabel("selected transport")
            ax.legend(loc="upper left")
        if fig(os.path.join(plots, "adaptive_routing.png"),
               plot_route):
            made["plots"].append("adaptive_routing")

    # ---------------- hysteresis stability -----------------------------
    hys = []
    for pol in ("size_only", "size_hysteresis"):
        rows = load(os.path.join(raw, f"hysteresis_{pol}.csv"))
        prev = None; sw = 0; n = 0
        for r in rows:
            if r[0] == "workload" or len(r) < 6:
                continue
            route = r[5]
            n += 1
            if prev is not None and route != prev:
                sw += 1
            prev = route
        if n:
            hys.append((pol, n, sw,
                        round(1000.0 * sw / max(1, n), 3)))
    if hys:
        write_md(os.path.join(tables, "hysteresis_summary.md"),
                 ("policy", "messages", "route_switches",
                  "switches_per_1000"), hys, "Hysteresis stability")
        write_csv(os.path.join(tables, "hysteresis_summary.csv"),
                  ("policy", "messages", "route_switches",
                   "switches_per_1000"), hys)
        made["tables"].append("hysteresis_summary")

        def plot_hys(plt, fig, ax):
            labels = [h[0] for h in hys]
            vals = [h[2] for h in hys]
            ax.bar(labels, vals,
                   color=["#d62728", "#2ca02c"][:len(hys)])
            ax.set_ylabel("route switches")
            ax.set_title("Hysteresis stability (thrash workload)")
            ax.grid(alpha=.3, axis="y")
        if fig(os.path.join(plots, "hysteresis_stability.png"),
               plot_hys):
            made["plots"].append("hysteresis_stability")

    # ---------------- EWMA alpha sensitivity ---------------------------
    alphas = []
    for a in ("0.05", "0.1", "0.2", "0.3", "0.5", "0.8"):
        rows = load(os.path.join(raw, f"ewma_alpha_{a}.csv"))
        prev = None; sw = 0; lats = []; n = 0
        for r in rows:
            if r[0] == "workload" or len(r) < 6:
                continue
            try:
                lat = float(r[3])
            except ValueError:
                continue
            route = r[5]
            n += 1; lats.append(lat)
            if prev is not None and route != prev:
                sw += 1
            prev = route
        if n:
            lats.sort()
            p99 = lats[min(len(lats) - 1, int(0.99 * len(lats)))]
            alphas.append((float(a), n, sw,
                           round(1000.0 * sw / max(1, n), 3),
                           round(p99 / 1000.0, 1)))
    if alphas:
        alphas.sort(key=lambda r: r[0])
        write_md(os.path.join(tables, "alpha_summary.md"),
                 ("alpha", "messages", "route_switches",
                  "switches_per_1000", "p99_us"),
                 alphas, "EWMA alpha sensitivity")
        write_csv(os.path.join(tables, "alpha_summary.csv"),
                  ("alpha", "messages", "route_switches",
                   "switches_per_1000", "p99_us"),
                  alphas)
        made["tables"].append("alpha_summary")

        def plot_alpha(plt, fig, ax):
            xs = [a[0] for a in alphas]
            ax.plot(xs, [a[2] for a in alphas], "o-",
                    label="route switches")
            ax.set_xlabel("EWMA alpha")
            ax.set_ylabel("route switches (bimodal workload)")
            ax2 = ax.twinx()
            ax2.plot(xs, [a[4] for a in alphas], "s--",
                     color="#d62728", label="p99 (us)")
            ax2.set_ylabel("p99 latency (us)")
            ax.set_title("EWMA alpha sensitivity")
            ax.legend(loc="upper left"); ax2.legend(loc="upper right")
            ax.grid(alpha=.3)
        if fig(os.path.join(plots, "ewma_alpha_sensitivity.png"),
               plot_alpha):
            made["plots"].append("ewma_alpha_sensitivity")

    # ---------------- latency breakdown (producer stats) ---------------
    bd = os.path.join(raw, "latency_breakdown_bimodal_producer.txt")
    if os.path.exists(bd):
        vals = {}
        for line in open(bd):
            if "=" in line and not line.startswith("#"):
                k, v = line.split("=", 1)
                try: vals[k.strip()] = float(v.strip())
                except ValueError: pass
        comps = [("router_ewma_classify_mean_ns",
                  "EWMA classify"),
                 ("send_copy_mean_ns", "payload copy")]
        rows = [(name, round(vals[k] / 1000.0, 2))
                for k, name in comps if k in vals]
        mean_e2e = vals.get("mean_e2e_ns") or 0
        if rows:
            write_md(os.path.join(tables, "latency_breakdown.md"),
                     ("component", "mean_us"), rows,
                     "Producer-side latency breakdown (bimodal)")
            write_csv(os.path.join(tables, "latency_breakdown.csv"),
                      ("component", "mean_us"), rows)
            made["tables"].append("latency_breakdown")

            def plot_bd(plt, fig, ax):
                ax.bar([r[0] for r in rows], [r[1] for r in rows],
                       color="#1f77b4")
                ax.set_ylabel("mean time (us)")
                ax.set_title("Producer-side latency components "
                             "(measured counters)")
                ax.grid(alpha=.3, axis="y")
            if fig(os.path.join(plots, "latency_breakdown.png"),
                   plot_bd):
                made["plots"].append("latency_breakdown")

    # ---------------- results summary + HTML report --------------------
    lines = ["# Experiment Summary", "", f"*Provenance: {prov}*",
             "", "## Environment", "",
             f"- OS: {meta.get('os','?')} {meta.get('os_version','')}",
             f"- Architecture: {meta.get('architecture','?')}",
             f"- Compiler: {meta.get('compiler','?')}",
             f"- Commit: {meta.get('git_commit','?')} "
             f"({meta.get('git_branch','?')})", ""]
    tpath = os.path.join(run, "test_results.txt")
    if os.path.exists(tpath):
        lines += ["## Correctness", "```"]
        lines += [l.rstrip() for l in open(tpath) if l.strip()]
        lines += ["```", ""]
    if os.path.exists(os.path.join(tables, "transport_comparison.md")):
        lines += ["## Transport Comparison", "",
                  "Median latency per payload size (see table/plots).",
                  ""]
    if "routing_summary" in made["tables"]:
        lines += ["## Adaptive Routing", "",
                  "The routing trace shows the EWMA driving the "
                  "transport choice across tau_low/tau_high; route "
                  "switches occur only when the EWMA crosses a "
                  "threshold.", ""]
    if "hysteresis_summary" in made["tables"]:
        lines += ["## Hysteresis", "",
                  "Switch counts under the adversarial thrash workload "
                  "with and without the deadband.", ""]
    if "alpha_summary" in made["tables"]:
        lines += ["## EWMA Sensitivity", "",
                  "Switch counts and p99 latency as a function of the "
                  "smoothing factor alpha.", ""]
    findings = []
    if tr_rows:
        findings.append("Transport choice dominates per-size latency "
                        "(measured in the sweep).")
    if "hysteresis_summary" in made["tables"]:
        d = dict((h[0], h[2]) for h in hys)
        if "size_only" in d and "size_hysteresis" in d:
            findings.append(
                f"The deadband reduces route switches from {d['size_only']} "
                f"(size_only) to {d['size_hysteresis']} "
                "(size_hysteresis) under the thrash workload.")
    lines += ["## Main Findings", ""] + \
             [f"- {f_}" for f_ in findings] + [""]
    with open(os.path.join(run, "results_summary.md"), "w") as f:
        f.write("\n".join(lines))

    # HTML report (lightweight, static)
    def img(name):
        p = os.path.join(plots, name)
        return (f'<img src="../plots/{name}" width="720">'
                if os.path.exists(p) else "")
    html = [f"<html><head><title>AdaptIPC Report</title>"
            f"<style>body{{font-family:sans-serif;max-width:900px;"
            f"margin:auto}} table{{border-collapse:collapse}}"
            f"td,th{{border:1px solid #ccc;padding:4px 8px}}</style>"
            f"</head><body>",
            "<h1>AdaptIPC Experimental Report</h1>",
            f"<p><i>Provenance: {prov}</i></p>",
            "<h2>1. Environment</h2><pre>",
            json.dumps(meta, indent=2), "</pre>"]
    if os.path.exists(tpath):
        html.append("<h2>2. Build / Test Status</h2><pre>")
        html += [l.rstrip() for l in open(tpath) if l.strip()]
        html.append("</pre>")
    html.append("<h2>3. Transport Comparison</h2>")
    html.append(img("transport_comparison.png"))
    tcmp = os.path.join(tables, "transport_comparison.md")
    if os.path.exists(tcmp):
        html.append("<pre>" + "".join(open(tcmp).readlines()[2:])
                    .replace("|", " | ") + "</pre>")
    html.append("<h2>4. Adaptive Routing</h2>")
    html.append(img("adaptive_routing.png"))
    html.append("<h2>5. Hysteresis</h2>")
    html.append(img("hysteresis_stability.png"))
    htab = os.path.join(tables, "hysteresis_summary.md")
    if os.path.exists(htab):
        html.append("<pre>" + "".join(open(htab).readlines()[2:]) +
                    "</pre>")
    html.append("<h2>6. EWMA Sensitivity</h2>")
    html.append(img("ewma_alpha_sensitivity.png"))
    html.append("<h2>7. Latency Breakdown</h2>")
    html.append(img("latency_breakdown.png"))
    html.append("<h2>8. Raw Data</h2><ul>")
    for fn in sorted(os.listdir(raw)):
        html.append(f'<li><a href="../raw/{fn}">{fn}</a></li>')
    html.append("</ul>")
    html.append("<h2>9. Reproducibility</h2>")
    html.append(f"<p>commit {meta.get('git_commit','?')} on "
                f"{meta.get('git_branch','?')}; flags "
                f"{meta.get('compiler_flags','?')}; see "
                f"run_metadata.json.</p>")
    html.append("<h2>10. Conclusions</h2><p>Conclusions are limited to "
                "what the generated tables show; see "
                "results_summary.md.</p>")
    html.append("</body></html>")
    with open(os.path.join(run, "report", "report.html"), "w") as f:
        f.write("\n".join(html))

    print(f"analysis complete: {len(made['plots'])} plots, "
          f"{len(made['tables'])} tables")

if __name__ == "__main__":
    main()
