# Experiment D: adversarial oscillation

| policy | kind | message_count | throughput_mbps | p50_us | p99_us | p99_9_us | route_switches | uds_messages | shm_messages | health_transitions | mean_queue_occupancy_bytes | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| uds | uds | 256 | 103.4 | 20.0 | 66.6 | 83.0 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| shm | shm | 256 | 157.5 | 21.0 | 30.4 | 31.0 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| size_only | adapt | 256 | 2796.4 | 56.0 | 64.0 | 136.3 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
| size_hysteresi | adapt | 256 | 3110.7 | 57.0 | 60.4 | 107.9 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
| queue_aware | adapt | 256 | 3303.7 | 4.0 | 146.7 | 233.6 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
| cost_aware | adapt | 256 | 2222.0 | 20.0 | 72.9 | 85.9 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
| full_adaptive | adapt | 256 | 3233.5 | 4.0 | 208.2 | 254.2 | 0 | 0 | 0 | 0 | 0 | policy setup_u |
