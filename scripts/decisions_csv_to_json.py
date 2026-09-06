#!/usr/bin/env python3
"""Convert a real ADAPT_DECISION_LOG CSV to the dashboard's JSON.
Adds a workload-phase label derived from the payload sequence (the
demo workload is small->bulk->mixed->small). No values are altered."""
import csv, json, sys

def phase_of(seq, payload, prev_payloads):
    # decided from the captured payload stream itself
    if seq < 400: return "PHASE 1: small control"
    if seq < 800: return "PHASE 2: bulk"
    if seq < 1400: return "PHASE 3: mixed"
    return "PHASE 4: small control"

def main(src, dst):
    rows = list(csv.DictReader(
        l for l in open(src) if not l.startswith("#")))
    out = {"source": "benchmarks/decision_demo.c (real policy decisions)",
           "decisions": []}
    for i, r in enumerate(rows):
        p = float(r["payload"])
        out["decisions"].append({
            "seq": i + 1,
            "payload": int(float(r["payload"])),
            "occ_bytes": float(r["occ_bytes"]),   # ring occupancy at decision
            "cost_shm_us": float(r["cost_shm_us"]),
            "cost_uds_us": float(r["cost_uds_us"]),
            "queue_wait_shm_us": float(r["queue_wait_shm_us"]),
            "switch_cost_us": float(r["switch_cost_us"]),
            "setup_cost_us": float(r["setup_cost_us"]),
            "health_penalty_us": float(r["health_penalty_us"]),
            "route": r["selected"],
            "reason": r["reason"],
            "phase": phase_of(i, p, None),
        })
    json.dump(out, open(dst, "w"), indent=1)
    print(f"wrote {dst}: {len(out['decisions'])} real decisions")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
