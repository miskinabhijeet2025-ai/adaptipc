# Failure Cases (v2.1) — where AdaptIPC does not win, and why

1. **Idle consumer, small messages.** Symptom: small message on SHM waits
   up to the 2 ms recv-poll floor. Cause: consumer is blocked in poll(2)
   on UDS; ring arrivals are noticed only on timeout. Impact: latency.
   Mitigation: notification-floor term routes small messages to UDS.
   Remaining: a truly idle consumer on SHM-only traffic still pays the poll.
2. **Boundary thrashing under size_only.** Symptom: 4,969 switches /
   5,000 messages at delta=1% payload alternation. Cause: no deadband.
   Impact: CPU + lost batching. Mitigation: hysteresis (8 switches, all
   persistent). Remaining: size_only remains available as a baseline.
3. **Measurement-noise switching.** Symptom (v2.0): queue_aware flapped 77
   times under adversarial load. Cause: occupancy EWMA too fast; escape had
   no switching-cost threshold. Impact: route churn. Mitigation (v2.1):
   escape requires wait > UDS cost + switch cost; occupancy EWMA 4x slower;
   8 switches/10k msgs, 0 flaps. Remaining: none measured.
4. **UDS receiver-queue overflow.** Symptom: silent datagram loss under
   sustained offered load > drain rate (measured dropped=119..147 rows).
   Cause: SOCK_DGRAM drops on rx-queue overflow; sender ENOBUFS retry does
   not cover receiver-side loss. Impact: pure-UDS baselines capped at
   64 KB payloads in the sweep. Mitigation: adapt traffic escalates bulk
   to SHM. Remaining: pure UDS transport is lossy under overload by design.
5. **Setup overhead for marginal benefit.** Symptom: short bulk bursts pay
   the 52 us mapping + handshake. Impact: amortization needs enough bulk
   traffic. Mitigation: setup_cost term (52 us) in the SHM score; lazy
   negotiation defers until first SHM-classified message.
6. **Slow consumer / stopped consumer.** Symptom: queue-wait prediction
   under-estimated by 10-1000x (queue_prediction_accuracy.csv). Cause: the
   drain-rate EWMA retains a stale fast rate while the consumer is not
   scheduled. Impact: escape decisions can fire late or not at all.
   Mitigation (v2.1): 5 ms staleness window + 50 MB/s conservative floor.
   Remaining: the model is reactive; it cannot predict consumer scheduling.
7. **Cost-model calibration drift.** Symptom: two-class linear fit loses
   accuracy under extreme bimodality within one size class. Impact: route
   mix can drift until recalibration. Mitigation: EWMA recalibration per
   message. Remaining: no structural (non-linear) model.
8. **Latency of escape probes under sender backpressure.** Symptom: the
   v2.0 E-experiment reported ~76 s "detection". Cause: the probe stream
   blocked in ENOBUFS against a paused consumer; the ROUTING decision
   itself fired in <1 ms. Impact: metric mislabeled in v2.0. Mitigation
   (v2.1): stall experiment separates decision latency (<1 ms) and
   recovery (2.7 ms) from probe delivery.
