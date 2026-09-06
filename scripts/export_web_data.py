#!/usr/bin/env python3
"""export_web_data.py -- convert real repository measurement CSVs into
website/data/*.json for the static website. No values are altered.

Usage: python3 scripts/export_web_data.py
"""
import csv
import json
import os

RAW = "experiments/raw"
V21 = "experiments/v2_1/raw"
OUT = "website/data"
os.makedirs(OUT, exist_ok=True)


def load(p):
    if not os.path.exists(p):
        return []
    return list(csv.DictReader(l for l in open(p)
                               if not l.startswith("#")))


def dump(name, obj):
    json.dump(obj, open(f"{OUT}/{name}", "w"), indent=1)
    print("wrote", f"{OUT}/{name}")


# benchmark.json: payload sweep, per policy
rows = load(f"{RAW}/payload_sweep.csv")
bench = [{"policy": r["policy"], "messages": int(r["message_count"]),
          "throughput_mbps": float(r["throughput_mbps"]),
          "p50_us": float(r["p50_us"]), "p95_us": float(r["p95_us"]),
          "p99_us": float(r["p99_us"]),
          "p99_9_us": float(r["p99_9_us"])} for r in rows]
dump("benchmark.json", {"source": "experiments/raw/payload_sweep.csv",
                        "rows": bench})

# policies.json: adversarial switching classification (v2.1)
rows = load(f"{V21}/adversarial_delta.csv")
agg = {}
for r in rows:
    p = r["policy"]
    a = agg.setdefault(p, {"switches": 0, "genuine": 0, "flaps": 0,
                           "tp": 0.0, "n": 0})
    a["switches"] += int(r["total_switches"])
    a["genuine"] += int(r["genuine_escapes"])
    a["flaps"] += int(r["noise_flaps"])
    a["tp"] += float(r["throughput_mbps"])
    a["n"] += 1
for a in agg.values():
    a["mean_tp_mbps"] = round(a["tp"] / max(1, a["n"]), 1)
    del a["tp"]
    del a["n"]
dump("policies.json", {"source": "experiments/v2_1/raw/adversarial_delta.csv",
                       "total_messages": 200000, "policies": agg})

# routing.json: real decision log (costs + reasons per decision)
rows = load("showcase/outputs/decisions.csv")
dec = [{"seq": i + 1, "payload": int(float(r["payload"])),
        "occ_bytes": float(r["occ_bytes"]),
        "cost_shm_us": float(r["cost_shm_us"]),
        "cost_uds_us": float(r["cost_uds_us"]),
        "queue_wait_us": float(r["queue_wait_shm_us"]),
        "route": r["selected"], "reason": r["reason"]}
       for i, r in enumerate(rows)]
dump("routing.json", {"source": "benchmarks/decision_demo.c decision log "
     "(full_adaptive policy, real)", "decisions": dec})

# queue.json: queue-prediction accuracy (honest gap included)
rows = load(f"{V21}/queue_prediction_accuracy.csv")
q = [{"occ_pct": int(r["occ_pct"]),
      "occupancy_bytes": int(float(r["occupancy_bytes"])),
      "predicted_wait_us": float(r["predicted_wait_us"]),
      "actual_delay_us": float(r["actual_delay_us"])} for r in rows]
dump("queue.json", {"source": "experiments/v2_1/raw/"
     "queue_prediction_accuracy.csv", "rows": q})

# health.json: stall timeline
rows = load(f"{V21}/stall_timeline.csv")
h = [{"t_ms": float(r["t_ms"]), "event": r["event"], "route": r["route"],
      "ring_used_bytes": float(r["ring_used"])} for r in rows
     if r["event"] != "summary"]
dump("health.json", {"source": "experiments/v2_1/raw/stall_timeline.csv",
                     "timeline": h})

# experiments.json: explorer index
dump("experiments.json", {"experiments": [
    {"name": "EWMA Sensitivity",
     "blurb": "How the smoothing factor changes adaptation speed vs "
              "route stability.",
     "cmd": "./demo/adaptipc_lab.sh -experiment ewma",
     "source": "benchmarks/alpha_sweep.c"},
    {"name": "Hysteresis Stability",
     "blurb": "The deadband prevents route oscillation under "
              "adversarial payloads.",
     "cmd": "./demo/adaptipc_lab.sh -experiment hysteresis",
     "source": "tests/benchmark_suite.c (thrash)"},
    {"name": "Queue Awareness",
     "blurb": "Ring occupancy and drain rate drive escape decisions "
              "under pressure.",
     "cmd": "benchmarks/hardening_suite prediction",
     "source": "src/runtime_context.c"},
    {"name": "Cost-Aware Routing",
     "blurb": "Estimated transport + queue + switching costs compared "
              "per message.",
     "cmd": "./demo/adaptipc_lab.sh -experiment adaptive",
     "source": "src/cost_model.c"},
    {"name": "Transport Health",
     "blurb": "HEALTHY -> DEGRADED -> BLOCKED -> RECOVERING with "
              "escape and recovery.",
     "cmd": "benchmarks/hardening_suite stall",
     "source": "src/transport_health.c"},
    {"name": "Self-Calibrating Crossover",
     "blurb": "The learned transport crossover S* vs synthetic ground "
              "truth (exact for 6 values).",
     "cmd": "benchmarks/hardening_suite crossover",
     "source": "src/cost_model.c"},
]})


if __name__ == "__main__":
    pass  # module body runs the export
