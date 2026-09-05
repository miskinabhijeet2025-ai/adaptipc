# Patent Evidence Matrix (v2.1) — measured technical effects only

| Technical mechanism | Baseline limitation | AdaptIPC mechanism | Experiment | Measured effect |
|---|---|---|---|---|
| Size-threshold routing (baseline) | boundary thrashing | EWMA + fixed tau thresholds | adversarial_delta (d=1%) | size_only: 4,969 switches / 5,000 msgs (thrash every message) |
| Hysteresis / stability margin | noise-induced switching | switching-cost margin H + deadband | adversarial_delta | hysteresis-bearing policies: 8 switches / 10k msgs, all persistent, 0 flaps |
| Context-aware queue escape | size-only cannot react to backlog | occupancy + drain-rate estimator | stall_timeline | route leaves SHM within <1 ms of consumer stall (escape=0.0 ms logged) |
| Queue prediction | no backlog model | queue_wait = occ / max(D, floor) + staleness guard | queue_prediction_accuracy | prediction tracks occupancy linearly; fails (under-predicts 10-1000x) when consumer stops draining -- documented limitation |
| Switching-cost accounting | cost-blind transitions | switch_cost + margin in score | margin sweep | 0 noise switches at any H (0-50 us); cost inputs deterministic in steady state |
| Lazy capability setup | eager mapping overhead | deferred SHM_SETUP_REQ/ACK | lazy_setup | init 1.82 ms (eager) vs 0.40 ms (lazy) when SHM never used; steady state equal or better |
| Transport health | static transport choice | debounced HEALTHY/DEGRADED/BLOCKED state | policy_modes + degradation | health transitions observed; escape to UDS under backlog, return to SHM after recovery |
| HW/LW flow control | unbounded backlog | 80/20 watermark, futex park/wake | backpressure gate | peak backlog 57,334 B (14 frames) bounded at HW envelope; pure ring fills unbounded |
| QoS | single objective | latency/throughput scoring | qos.csv | LATENCY vs BALANCED vs THROUGHPUT change route mix and p99 (p99 96.8/240.9/112.1 ms under paced load) |
| Self-calibrating crossover | hard-coded threshold | two-class linear fit + clamped EWMA S* | crossover_sweep | learned S* == true S* for all 6 ground truths (512..8192 B), 0 error |
