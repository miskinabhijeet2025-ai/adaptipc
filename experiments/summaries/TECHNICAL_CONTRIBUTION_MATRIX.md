# Technical Contribution Matrix (v2.1) — feature decomposition for prior-art preparation

Not a legal document. Decomposes the implementation into features with
dependencies and measured effects, to support a later claim-oriented
prior-art search.

| # | Feature | What it does | Why it exists | Depends on | Measured effect | Known limitation |
|---|---|---|---|---|---|---|
| F1 | Heterogeneous transports | one API over SHM ring + UDS datagram | correctness across payload regimes | - | 12.3 GB/s SHM vs 0.3 GB/s UDS aggregate | io_uring Linux-only |
| F2 | Per-message runtime selection | route chosen per send | mixed workloads | F1 | route mix follows workload (qos.csv) | - |
| F3/F4 | Payload-size EWMA | smoothed size estimate | single-message noise | F2 | stable classification | lags regime shifts |
| F5 | Hysteresis deadband | sticky route in [1024,4096] | anti-thrash | F3 | 1 switch vs 4,969 (size_only) at d=1% | - |
| F6 | Queue-occupancy awareness | ring fill level as decision input | backlog dominates latency | F1 | occupancy == cursor math (occupancy_validation) | sampling between messages |
| F7 | Producer/drain-rate prediction | queue_wait = occ / max(D, floor) | predicts queueing before it happens | F6 | linear tracking; under-predicts on stalled consumer (failure case) | reactive, not predictive |
| F8 | Switching-cost accounting | margin H in cost space | noise immunity | F2 | 0 noise switches in margin sweep | - |
| F9 | Lazy SHM negotiation | deferred mapping via UDS handshake | avoid unused setup | F1 | init 0.40 vs 1.82 ms when SHM unused | first bulk msg carries handshake |
| F10/F11 | Transport health + escape/recovery | debounced state machine | survive slow consumer | F6,F7 | escape <1 ms, recovery ~2.7 ms | health sampling is send-driven |
| F12 | QoS-aware routing | latency/throughput weighting | application objectives | F2 | route mix shifts with posture (qos.csv) | weights empirical |
| F13 | HW/LW flow control | bounded watermark park/wake | bound queueing delay | F1 | 14-frame peak backlog | tradeoff: throughput vs ring size |
| F14 | Integrated cost score | single decision function | coherent tradeoffs | F5-F8,F10,F12 | policies rank consistently in ablation | linear model |
