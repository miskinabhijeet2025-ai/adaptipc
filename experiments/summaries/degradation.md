# Experiment E: degradation+recovery

| policy | kind | message_count | throughput_mbps | p50_us | p99_us | p99_9_us | route_switches | uds_messages | shm_messages | health_transitions | mean_queue_occupancy_bytes | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| size_only | adapt | 2001 | 0.0 | 322.0 | 520.0 | 49450.0 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
| size_hysteresi | adapt | 2001 | 0.0 | 317.0 | 786.0 | 51841.0 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
| queue_aware | adapt | 2001 | 0.0 | 316.0 | 461.0 | 49583.0 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
| cost_aware | adapt | 2001 | 0.0 | 322.0 | 691.0 | 51989.0 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
| full_adaptive | adapt | 2001 | 0.0 | 319.0 | 651.0 | 51887.0 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
