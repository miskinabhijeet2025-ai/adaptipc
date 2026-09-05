# AdaptIPC v2.1 — Experimental Hardening Documentation

See paper/EXPERIMENTS_V2.md for the v2 methodology; this document
records the v2.1 hardening: instrumentation fixes, switch
classification, stability validation, queue-prediction validation and
the failure-case inventory. Raw data: experiments/v2_1/raw/.

## 1. Instrumentation

* Queue occupancy: `adapt_shm_used_bytes()` verified against the
  canonical cursor difference (head - tail) through an independent
  mapping, at 10..90% fill plus empty and full states
  (occupancy_validation.csv; test_queue_occupancy_instrumentation).
  Reported == cursor exactly at every level.
* Decision log: column order, finiteness, route validity, reason
  validity and timestamp monotonicity verified by
  test_decision_log_consistency (timestamps are non-decreasing; the
  platform clock quantizes at ~2 us).
* Route transition accounting: the route_switches counter equals the
  transitions observed via adapt_last_route()
  (test_route_transition_accounting).
* Root cause of the v2 zero-occupancy readings: experiment B used a
  2 KB payload, keeping the EWMA below tau_high so every policy
  legitimately stayed on UDS. Workload fixed to 16 KB.

## 2. Estimator noise and the stability margin

Measured |score_uds - score_shm| spread under a stable workload:
steady-state range 0.00 us -- the converged cost estimates are
deterministic. The stability condition H > 2*epsilon is therefore
trivially satisfied in steady state; the engineering risk is input
fluctuation (occupancy), addressed by the escape hysteresis. The
margin sweep (0..50 us absolute) produced 0 switches at every H under
the adversarial size workload -- no noise-induced switching exists to
suppress in this regime. Claim status: the H > 2*epsilon condition is
a model-level engineering heuristic, not a theorem; epsilon could not
be bounded away from zero because measurement noise is below the
clock/estimator resolution.

## 3. Switch classification and false-switch rate

Rules (mechanical): a switch is a *genuine escape* when the new route
persists >= 8 messages (or the reason is QUEUE_PRESSURE /
HEALTH_ESCAPE); a *recovery* is the return to SHM after a genuine
escape with persistence; everything else is a *noise_flap*.
false_switch_rate = noise_flaps / total_switches.

Adversarial delta sweep (4 deltas x 5 policies x 2 reps x 5000 msgs =
200k messages, S* near 4096):

| policy | switches | genuine | flaps | false_switch_rate |
|---|---|---|---|---|
| size_only | 39,796 | 0 | 0 (all boundary thrash) | n/a (thrash regime) |
| size_hysteresis | 8 | 8 | 0 | 0.00 |
| queue_aware | 8 | 8 | 0 | 0.00 |
| cost_aware | 0 | 0 | 0 | 0.00 (never switched) |
| full_adaptive | 8 | 8 | 0 | 0.00 |

size_only at delta=1% switched on 4,969 of 5,000 messages.

## 4. Queue-prediction validation

queue_prediction_accuracy.csv: the model tracks occupancy linearly
under active drain; with a stopped consumer it under-predicts by
10-1000x. v2.1 adds a 5 ms staleness window (a drain estimate older
than the window falls back to the 50 MB/s floor). The floor
sensitivity question: the floor exists to make the fallback
CONSERVATIVE (over-estimate the wait); 50 MB/s is well below any
measured active drain rate on the reference platform, so escapes fire
early rather than late. Sensitivity sweep across floors is future
work; the floor is already configurable via the cost config.

## 5. Consumer stall / recovery

stall_timeline.csv (full_adaptive, 512 KB ring): routing decision
leaves SHM within <1 ms of the stall (probe-stream delivery is
separate and dominated by UDS sender backpressure -- not reported as
detection); recovery to SHM 2.7 ms after the consumer resumes.

## 6. Crossover learning

crossover_sweep.csv: learned S* equals the synthetic ground truth
exactly for 512, 1024, 1333, 2048, 4096 and 8192 B (0 absolute error),
converging within the 400-sample window. The mechanism learns the
crossover from measured costs; it does not reproduce a hard-coded
value.

## 7. Lazy negotiation

lazy_setup.csv: init cost 1.82 ms (eager) vs 0.40 ms (lazy) when the
workload never needs SHM; first bulk message carries the handshake;
steady-state throughput equal or better for lazy in all three workload
mixes (uds_only 108 vs 13 MB/s, mixed 9138 vs 6461, shm_heavy 8942 vs
8017 -- single runs, variance not established; the init-time result is
the robust one).

## 8. Backpressure re-verification

The 64 KB-ring gate reproduces under v2.1: peak backlog 57,334 B
(14 frames) within the HW+1 envelope (60,628 B); queueing component
~15-30 us. Queue-pressure experiment: pure unbounded SHM p50 580 ms /
p99 1.15 s vs watermark-bounded policies p99 24-27 ms at identical
throughput.

## 9. Watermark sensitivity

watermark_sensitivity.csv (1 MB ring, 1 KB messages, 200k saturated
messages, single run per configuration -- statistical significance NOT
established):

| HW/LW | throughput | parks | p99 |
|---|---|---|---|
| 80/20 (default) | 4774 MB/s | 2 | 60 us |
| 70/20 | 4102 MB/s | 0 | 3 us |
| 60/20 | 6088 MB/s | 4 | 37 us |
| 90/20 | 5129 MB/s | 2 | 62 us |
| 80/30 | 3080 MB/s | 3 | 48 us |
| 90/30 | 2647 MB/s | 0 | 31 us |

Honest interpretation: single runs; the spread (2.6-6.1 GB/s) is
within run-to-run variance, so no configuration is demonstrated
superior. 80/20 performs comparably and is retained as the default;
the constants are now compile-time overridable
(-DSHM_HW_PCT / -DSHM_LW_PCT) for future multi-run campaigns.

## 10. Tests added in v2.1

* test_queue_occupancy_instrumentation: occupancy == cursor math at
  20/40/60/80% fill, empty and full states, plus drain-to-empty.
* test_decision_log_consistency: log schema, monotonic timestamps,
  finite non-negative scores, valid routes/reasons.
* test_route_transition_accounting: route_switches counter equals
  observed transitions.
