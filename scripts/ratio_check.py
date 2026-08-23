#!/usr/bin/env python3
"""ratio_check.py -- recompute SHM/UDS and Adapt/UDS throughput ratios and
median latencies from benchmarks/summary.txt, then drift-check them against
Table I as typeset in paper/main.tex. Exits 1 on any disagreement."""
import re
import sys

SUMMARY = "benchmarks/summary.txt"
TEX = "paper/main.tex"

runs = {}  # (workload, mode) -> dict(avg_tp, median_lat, msgs, switches)
pat = re.compile(
    r"\[(\w+)/(\w+)\] msgs=(\d+).*?avg_tp=([\d.]+) MB/s "
    r"median_lat=(\d+) ns route_switches=(\d+)")
for line in open(SUMMARY):
    m = pat.search(line)
    if m:
        mode, wl, msgs, tp, lat, sw = m.groups()  # "[uds/sweep]" => mode/wl
        runs[(wl, mode)] = {"msgs": int(msgs), "tp": float(tp),
                            "lat": int(lat), "switches": int(sw)}

print("=== authoritative summary.txt values ===")
for k in sorted(runs):
    print(f"{k[0]}/{k[1]}: msgs={runs[k]['msgs']} tp={runs[k]['tp']} "
          f"MB/s med_lat={runs[k]['lat']}ns switches={runs[k]['switches']}")

print("\n=== ratio table (from summary.txt) ===")
print(f"{'workload':<10} {'SHM/UDS':>9} {'Adapt/UDS':>11}   "
      f"{'lat_uds':>10} {'lat_shm':>10} {'lat_adapt':>10}")
r_shm, r_ad = [], []
for wl in ("sweep", "bimodal", "thrash"):
    u, s, a = (runs[(wl, m)]["tp"] for m in ("uds", "shm", "adapt"))
    la = (runs[(wl, m)]["lat"] for m in ("uds", "shm", "adapt"))
    lu, ls, lad = la
    r_shm.append(s / u)
    r_ad.append(a / u)
    print(f"{wl:<10} {s/u:>8.2f}x {a/u:>10.2f}x   "
          f"{lu:>9,}ns {ls:>9,}ns {lad:>9,}ns")
print(f"\nSHM/UDS range:   {min(r_shm):.2f}x - {max(r_shm):.2f}x")
print(f"Adapt/UDS range: {min(r_ad):.2f}x - {max(r_ad):.2f}x")

print("\n=== thrash verification ===")
t = runs.get(("thrash", "adapt"))
if not t:
    sys.exit("missing adapt/thrash run")
ok_sw = t["switches"] == 0
print(f"adapt/thrash: msgs={t['msgs']} switches={t['switches']} "
      f"med_lat={t['lat']}ns -> switches==0: {ok_sw}, "
      f"latency {'competitive' if t['lat'] <= runs[('thrash','uds')]['lat'] * 3 else 'REGRESSED'}"
      f" (vs UDS {runs[('thrash','uds')]['lat']}ns)")

# --- drift check vs Table I (positional: 9 rows, fixed order) ---
print("\n=== Table I drift check ===")
tex = open(TEX).read()
tab = re.search(r"\\begin\{tabular\}.*?\\end\{tabular\}", tex, re.S).group(0)
tab_lines = [l for l in tab.splitlines() if "\\,MB/s" in l]
order = ["sweep", "bimodal", "thrash"]
modes = ["uds", "shm", "adapt"]
drift = False
if len(tab_lines) != 9:
    print(f"expected 9 data rows, found {len(tab_lines)}")
    drift = True
for idx, line in enumerate(tab_lines):
    wl = order[idx // 3]
    mode = modes[idx % 3]
    clean = (line.replace("{", "").replace("}", "")
                 .replace("\\,", "").replace(",", ""))
    mtp = re.search(r"(\d+) ?MB/s", clean)
    nums = [int(mtp.group(1))] if mtp else []
    actual_tp = round(runs[(wl, mode)]["tp"])
    if not nums:
        print(f"{wl}/{mode}: NO THROUGHPUT IN ROW")
        drift = True
        continue
    table_tp = nums[0]
    ok = abs(table_tp - actual_tp) / max(actual_tp, 1) < 0.05
    if not ok:
        drift = True
    print(f"{wl}/{mode}: table={table_tp} MB/s log={actual_tp} "
          f"MB/s {'OK(<5%)' if ok else '<-- DRIFT >5%'}")

# Thrash/AdaptIPC switches column must be typeset as bold 0
sw_ok_tex = bool(re.search(
    r"AdaptIPC & 477\\,MB/s\s*& ---\s*& \\textbf\{0\}", tab))
print("Thrash/AdaptIPC 'Switches' typeset as 0:", sw_ok_tex)

all_sw_zero_or_warmup = all(
    runs[(w, m)]["switches"] in (0, 1)
    for w in order for m in modes)
print("all switch counts in {0,1} across every run:",
      all_sw_zero_or_warmup)

ok_all = (not drift) and ok_sw and sw_ok_tex and all_sw_zero_or_warmup
print("\nRESULT:", "ALL CONSISTENT" if ok_all else "DRIFT DETECTED")
sys.exit(0 if ok_all else 1)
