# Experiment C: bursty workload

| policy | kind | message_count | throughput_mbps | p50_us | p99_us | p99_9_us | route_switches | uds_messages | shm_messages | health_transitions | mean_queue_occupancy_bytes | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| uds | uds | 12000 | 279.1 | 8.0 | 27.0 | 51.0 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| shm | shm | 12000 | 3030.3 | 707.5 | 1312.0 | 1316.0 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| size_only | adapt | 12000 | 55.6 | 10.0 | 52.0 | 183.0 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
| size_hysteresi | adapt | 12000 | 51.2 | 14.0 | 106.0 | 591.1 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
| queue_aware | adapt | 12000 | 50.6 | 14.0 | 60.0 | 116.0 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
| cost_aware | adapt | 12000 | 49.3 | 16.0 | 57.0 | 111.0 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
| full_adaptive | adapt | 12000 | 52.0 | 13.0 | 64.0 | 124.0 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
