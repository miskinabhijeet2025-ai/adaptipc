# Experiment C: bursty workload

| policy | kind | message_count | throughput_mbps | p50_us | p99_us | p99_9_us | route_switches | uds_messages | shm_messages | health_transitions | mean_queue_occupancy_bytes | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| uds | uds | 12000 | 404.8 | 6.0 | 76.0 | 415.0 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| shm | shm | 12000 | 2719.0 | 747.0 | 2726.0 | 2731.0 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| size_only | adapt | 12000 | 60.2 | 6.0 | 73.0 | 104.0 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
| size_hysteresi | adapt | 12000 | 59.8 | 6.0 | 196.0 | 385.0 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
| queue_aware | adapt | 12000 | 59.9 | 6.0 | 219.0 | 291.0 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
| cost_aware | adapt | 12000 | 60.1 | 5.0 | 68.0 | 114.0 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
| full_adaptive | adapt | 12000 | 59.6 | 6.0 | 90.0 | 132.0 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
