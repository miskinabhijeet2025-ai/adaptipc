# Regression: v2 vs v2.1

| metric | v2 (b62329b) | v2.1 | difference | interpretation |
|---|---|---|---|---|
| tests passing | 7 | 10 | +3 | occupancy instrumentation, decision-log consistency, route-transition accounting |
| adversarial switches (queue_aware) | 77 | 11 (d-mix) / 0 flaps | -86% | escape hysteresis + slower occupancy EWMA (v2.1) |
| adversarial switches (full_adaptive) | 49 | 19 -> 8 genuine / 0 flaps | -84% | same |
| false-switch rate | not measured | 0.00 for all hysteresis policies | new metric | switch classification added |
| epsilon (estimator noise, steady) | not measured | 0.00 us | new metric | cost estimates deterministic after convergence |
| queue prediction error | not measured | under-predicts 10-1000x on stopped consumer | new metric | documented failure case; staleness guard added |
| occupancy instrumentation | read 0 in experiment B | == cursor math at all fill levels | fixed | workload design bug: 2 KB payload kept EWMA below tau_high; B now uses 16 KB |
| experiment B adapt route mix | 100% UDS (SHM never mapped) | SHM + measured occupancy | fixed | same root cause |
| stress 100k/500k/1M | 0 loss | 0 loss | unchanged | no regression |
| ASan/LSan/TSan | clean | clean | unchanged | no regression |
| data location | experiments/raw | experiments/v2_1/raw (v2 preserved) | layout | historical results retained |

Algorithm changes in v2.1 (evidence-driven, minimal):
1. Drain-estimate staleness window (5 ms) -- fixes stale-fast-rate
   under-prediction of queue wait.
2. Queue-aware escape requires wait > UDS cost + switch cost -- fixes
   occupancy-oscillation flapping.
3. Occupancy EWMA smoothed 4x slower than the size EWMA.
4. FULL_ADAPTIVE cold-start path applies health penalties.
5. Eager consumer attach no longer spins on header init (deadlock fix
   for consumer-initializes-first harnesses).

No benchmark numbers from v2 were replaced; all v2 files remain under
experiments/raw, experiments/summaries, experiments/figures.
