# Stress tests (full_adaptive policy, 1 MB ring, 8 KB messages)

| messages | data | duration | throughput | loss | order |
|---|---|---|---|---|---|
| 100,000 | 781 MB | 1.85 s | 423 MB/s | 0 | strict FIFO OK |
| 500,000 | 3906 MB | 8.90 s | 439 MB/s | 0 | strict FIFO OK |
| 1,000,000 | 7812 MB | 17.38 s | 449 MB/s | 0 | strict FIFO OK |

Sanitizers: ASan+LSan clean on all 7 test binaries; TSan clean on all
concurrency tests (flowcontrol, backpressure_latency, policy_modes,
lazy_negotiation).
