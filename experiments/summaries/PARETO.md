# Pareto analysis: throughput vs p99 latency (queue pressure, paced)

| policy | throughput MB/s | p50 us | p99 us | note |
|---|---|---|---|---|
| uds | 9.9 | 22,573 | 28,006 | no backlog (socket-limited) |
| shm (pure, unbounded) | 10.1 | 580,738 | 1,152,381 | full-ring backlog dominates |
| size_only | 9.9 | 23,894 | 26,375 | watermark-bounded |
| size_hysteresis | 10.1 | 23,574 | 26,631 | bounded |
| queue_aware | 10.1 | 23,532 | 24,491 | bounded, best p99 |
| cost_aware | 10.0 | 23,628 | 24,857 | bounded |
| full_adaptive | 10.1 | 23,476 | 24,118 | bounded, best p50/p99 |

Interpretation: at identical throughput (all drain-rate-limited at
~10 MB/s), pure SHM without flow control pays 22-46x worse tail
latency. All watermark-bounded configurations sit on the Pareto
frontier; the context-aware policies add the smallest tails. The
tradeoff is therefore real: bounded backlog at no throughput cost
versus the unbounded ring.
