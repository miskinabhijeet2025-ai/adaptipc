# Experimental Documentation — Context-Aware Extension (v2)

This document records the NEW experimental methodology, algorithms and
results for the context-aware extension. It is deliberately separate
from the paper's ORIGINAL VALIDATED RESULTS (see `paper/main.tex`):
nothing in the original text has been modified, and no original number
has been replaced.

## 1. Methodology

* Platform metadata (platform, compiler, seed, message counts) is a
  column of every raw CSV in `experiments/raw/`.
* Each experiment runs the IDENTICAL workload across all policies;
  policy selection is the only independent variable.
* Latency = end-to-end (producer CLOCK_MONOTONIC stamp inside the
  frame → consumer receipt), including transport queueing.
* Producer-side counters (route switches, backpressure parks, health
  transitions, setup messages) come from the library's own stats.

## 2. Algorithms

### 2.1 Runtime context (Phase 1)

Per `adapt_send()`, producer-side, single-threaded:

* `A ← EWMA_α(payload/Δt)` — offered rate (bytes/s)
* `D ← EWMA_α((occ_prev_after_push − occ_now)/Δt)` when positive —
  drain rate (bytes/s); a growing queue yields no sample
* `queue_wait = occ / max(D, D_floor)` with conservative floor
  D_floor = 50 MB/s. Empty queue → 0. Unreliable D (< min samples or
  stalled) → the floor, which OVERestimates the wait (safe direction).

### 2.2 Cost model and decision rule (Phases 2, 5)

All quantities in microseconds:

```
Score(T) = transport_cost(T,S)
         + queue_cost(SHM)          = occ / D
         + setup_cost(SHM, unmapped)= 52 µs (measured eager-map cost)
         + switching_cost(T ≠ current)
         + health_penalty(SHM)      (FULL_ADAPTIVE)
         + latency_penalty          = w · max(0, lat_T − budget)
lat_SHM = queue_wait + notify_floor(2 ms idle / 5 µs active) + copy
lat_UDS = transport_cost(UDS, S)
```

Decision: `T* = argmin Score(T)`; switch only if
`Score(T*) + H < Score(current)` with H = 5 µs. **Stability claim:**
with per-side estimation error bounded by ε, noise alone cannot flip a
route while H > 2ε — the cost-space analogue of the size-only
hysteresis proof. The notification-floor term is what makes small
messages legitimately belong on UDS when the consumer is idle, without
any hardcoded size rule.

### 2.3 Self-calibrating crossover (Phase 3)

Send-side costs are measured per message (SHM pushes are measured on
the non-parked attempt). Per transport, a two-class (≤1 KB, >1 KB)
linear fit `cost(S) = a + b·S` is refit from EWMA class means once
≥32 samples per class exist. The crossover
`S* = (a_shm − a_uds)/(b_uds − b_shm)` is clamped to [512 B, 1 MB] and
rate-limited by an EWMA (α=0.1). Until warm, FULL_ADAPTIVE falls back
to the validated size-hysteresis policy with the queue-pressure escape.

### 2.4 Transport health (Phase 4)

States: UNAVAILABLE, HEALTHY, DEGRADED, BLOCKED, RECOVERING.
Transitions (debounce = 8 consecutive samples):
occupancy ≥ 95 % → BLOCKED; ≥ 80 % or drain deficit (D < 0.5·A with
non-empty queue) → DEGRADED; ≤ 20 % for the streak → RECOVERING →
HEALTHY. Penalties: DEGRADED +100 µs, BLOCKED +1000 µs,
RECOVERING +25 µs on the SHM score.

### 2.5 QoS (Phase 5)

`ADAPT_QOS_BALANCED` (w=1), `ADAPT_QOS_LATENCY` (w=2), and
`ADAPT_QOS_THROUGHPUT` (budget ignored). Budget default 0 = unlimited.

## 3. Results

See `experiments/summaries/*.md` for the machine-derived tables and
`experiments/figures/fig*.png` for plots. Highlights (macOS arm64,
clang 21, -O3; exact numbers in the CSVs):

* **Adversarial oscillation (D):** 256 alternating messages produce
  more route switches under `size_only` than under
  `size_hysteresis`; `full_adaptive` is never less stable.
* **Degradation/recovery (E):** a stalled consumer drives SHM to
  DEGRADED/BLOCKED; queue-aware and cost-aware policies escape to UDS
  for small messages; after resumption the health model returns to
  HEALTHY and routing follows. Zero message loss in every run.
* **Setup cost (F):** lazy establishment defers the 52 µs mapping and
  the first SHM message carries the handshake; steady-state
  throughput matches eager.
* **QoS (G):** a 500 µs budget shifts small-message routing toward
  UDS under LATENCY QoS versus BALANCED (decision-log evidence).

## 4. Honest negative findings

* On an idle consumer, the measured cost model prefers SHM even for
  small messages (the ring push is genuinely cheaper than a datagram
  send); only the notification-floor latency term justifies UDS. The
  ablation therefore shows COST_AWARE diverging from SIZE_ONLY in
  route mix, not always in raw throughput.
* Under sustained bulk backlog with a 64 MB ring, all policies that
  keep SHM show the queueing tail documented in the original paper;
  the watermark flow control bounds it at 80 % of capacity. Tail
  latency below 100 µs requires a right-sized (latency-profile) ring —
  demonstrated by `tests/test_backpressure_latency.c`.
* UDS SOCK_DGRAM drops datagrams silently when the receiver queue
  overflows; measured and reported as `dropped=N` rather than hidden.

## 5. Reproduction

```sh
cc -std=c11 -O3 -Wall -Wextra -pthread -Iinclude \
   benchmarks/adaptive_ablation.c src/*.c -o /tmp/abl
/tmp/abl --out experiments/raw            # experiments A-G
bash scripts/generate_figures.sh          # summaries + figures
cc -std=c11 -O3 -Wall -Wextra -pthread -Iinclude \
   benchmarks/production_comparison.c src/*.c -o /tmp/pcmp
/tmp/pcmp --out benchmarks                # transport comparison + CDFs
ctest --test-dir build                    # or run tests/test_*.c
```

Sanitizers: rebuild with `-fsanitize=address` / `-fsanitize=thread`;
the full test suite is clean under ASan+LSan and TSan.
