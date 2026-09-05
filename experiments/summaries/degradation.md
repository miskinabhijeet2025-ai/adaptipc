# Experiment E: degradation+recovery

| policy | kind | message_count | throughput_mbps | p50_us | p99_us | p99_9_us | route_switches | uds_messages | shm_messages | health_transitions | mean_queue_occupancy_bytes | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| size_only | adapt | 1 | 0.0 | 0.0 | 0.0 | 0.0 | 1 | 1 | 2 | 0 | 0 | policy setup_u |
| size_hysteresi | adapt | 1 | 0.0 | 0.0 | 0.0 | 0.0 | 1 | 1 | 2 | 0 | 0 | policy setup_u |
| queue_aware | adapt | 1 | 0.0 | 0.0 | 0.0 | 0.0 | 1 | 1 | 2 | 0 | 0 | policy setup_u |
| cost_aware | adapt | 2001 | 0.2 | 270.0 | 424.0 | 15275.0 | 0 | 1 | 2000 | 1 | 0 | policy setup_u |
| full_adaptive | adapt | 1 | 0.0 | 0.0 | 0.0 | 0.0 | 1 | 1 | 2 | 0 | 0 | policy setup_u |
