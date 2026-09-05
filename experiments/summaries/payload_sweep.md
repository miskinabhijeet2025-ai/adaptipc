# Experiment A: payload sweep

| policy | kind | message_count | throughput_mbps | p50_us | p99_us | p99_9_us | route_switches | uds_messages | shm_messages | health_transitions | mean_queue_occupancy_bytes | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| uds | uds | 15000 | 300.3 | 52.0 | 6838.0 | 30953.0 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| shm | shm | 19500 | 12349.5 | 1201.0 | 6453.0 | 7508.0 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| size_only | adapt | 19500 | 9403.6 | 2157.5 | 7679.0 | 8475.0 | 1 | 9000 | 10500 | 4 | 13157490 | policy setup_u |
| size_hysteresi | adapt | 19500 | 9742.9 | 1955.0 | 5744.0 | 5911.0 | 1 | 9000 | 10500 | 2 | 13005880 | policy setup_u |
| queue_aware | adapt | 19500 | 9598.2 | 2069.5 | 5330.0 | 5882.5 | 1 | 9000 | 10500 | 4 | 12816257 | policy setup_u |
| cost_aware | adapt | 19500 | 9656.2 | 2227.0 | 5925.0 | 7086.0 | 0 | 9000 | 10500 | 44 | 14135169 | policy setup_u |
| full_adaptive | adapt | 19500 | 9444.3 | 2217.0 | 6500.0 | 7959.6 | 1 | 9000 | 10500 | 47 | 14248167 | policy setup_u |
