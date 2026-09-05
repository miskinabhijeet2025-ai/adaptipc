# Experiment B: queue pressure

| policy | kind | message_count | throughput_mbps | p50_us | p99_us | p99_9_us | route_switches | uds_messages | shm_messages | health_transitions | mean_queue_occupancy_bytes | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| uds | uds | 6000 | 9.9 | 22573.0 | 28006.4 | 28821.0 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| shm | shm | 6000 | 10.1 | 580738.5 | 1152380.9 | 1162813.2 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| size_only | adapt | 5881 | 9.9 | 23894.0 | 26375.2 | 26716.2 | 0 | 6000 | 0 | 0 | 0 | dropped=119 (u |
| size_hysteresi | adapt | 5882 | 10.1 | 23574.0 | 26631.3 | 26992.0 | 0 | 6000 | 0 | 0 | 0 | dropped=118 (u |
| queue_aware | adapt | 5882 | 10.1 | 23532.0 | 24491.2 | 24773.4 | 0 | 6000 | 0 | 0 | 0 | dropped=118 (u |
| cost_aware | adapt | 5882 | 10.0 | 23628.0 | 24856.5 | 25175.6 | 0 | 6000 | 0 | 0 | 0 | dropped=118 (u |
| full_adaptive | adapt | 5881 | 10.1 | 23476.0 | 24118.4 | 24268.1 | 0 | 6000 | 0 | 0 | 0 | dropped=119 (u |
