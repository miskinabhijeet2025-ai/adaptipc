# Experiment D: adversarial oscillation

| policy | kind | message_count | throughput_mbps | p50_us | p99_us | p99_9_us | route_switches | uds_messages | shm_messages | health_transitions | mean_queue_occupancy_bytes | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| uds | uds | 256 | 299.5 | 7.0 | 31.4 | 39.2 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| shm | shm | 256 | 182.8 | 35.0 | 46.0 | 46.7 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| size_only | adapt | 256 | 2601.8 | 31.0 | 35.0 | 108.5 | 1 | 2 | 254 | 2 | 0 | policy setup_u |
| size_hysteresi | adapt | 256 | 2891.3 | 27.0 | 30.4 | 92.8 | 1 | 2 | 254 | 2 | 0 | policy setup_u |
| queue_aware | adapt | 256 | 2837.4 | 5.0 | 212.6 | 296.0 | 11 | 7 | 249 | 2 | 0 | policy setup_u |
| cost_aware | adapt | 256 | 1989.6 | 10.0 | 70.4 | 105.5 | 0 | 128 | 128 | 2 | 0 | policy setup_u |
| full_adaptive | adapt | 256 | 2818.6 | 4.0 | 212.9 | 261.4 | 19 | 11 | 245 | 2 | 0 | policy setup_u |
