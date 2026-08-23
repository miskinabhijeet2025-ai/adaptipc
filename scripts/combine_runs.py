#!/usr/bin/env python3
"""combine_runs.py -- median-combine N benchmark summary logs.

Usage: python3 scripts/combine_runs.py out.txt in1.txt [in2.txt ...]
Writes one line per (workload, mode) with MEDIAN avg_tp and median
median_lat across runs, plus min/max switches sanity info.
"""
import re
import statistics
import sys

pat = re.compile(
    r"\[(\w+)/(\w+)\] msgs=(\d+) wire=[\d.]+ MB dur=[\d.]+s "
    r"avg_tp=([\d.]+) MB/s median_lat=(\d+) ns route_switches=(\d+)")

runs = []
for path in sys.argv[2:]:
    d = {}
    for line in open(path):
        m = pat.search(line)
        if m:
            mode, wl, msgs, tp, lat, sw = m.groups()
            d[(wl, mode)] = {"msgs": int(msgs), "tp": float(tp),
                             "lat": int(lat), "switches": int(sw)}
    runs.append(d)

keys = sorted(runs[0].keys())
out = open(sys.argv[1], "w")
for k in keys:
    tps = [r[k]["tp"] for r in runs]
    lats = [r[k]["lat"] for r in runs]
    sws = {r[k]["switches"] for r in runs}
    msgs = {r[k]["msgs"] for r in runs}
    assert len(msgs) == 1, f"msg count mismatch for {k}: {msgs}"
    print(f"[{k[1]}/{k[0]}] msgs={msgs.pop()} "
          f"avg_tp={statistics.median(tps):.1f} MB/s "
          f"median_lat={int(statistics.median(lats))} ns "
          f"route_switches={min(sws)} "
          f"(per-run switches: {sorted(sws)}; n={len(runs)} runs)",
          file=out)
out.close()
print(f"wrote {sys.argv[1]} ({len(runs)} runs combined)")
