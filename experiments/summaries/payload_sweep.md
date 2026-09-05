# Experiment A: payload sweep

| policy | kind | message_count | throughput_mbps | p50_us | p99_us | p99_9_us | route_switches | uds_messages | shm_messages | health_transitions | mean_queue_occupancy_bytes | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| uds | uds | 15000 | 557.2 | 48.0 | 4659.0 | 4859.0 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| shm | shm | 19500 | 13948.6 | 3346.0 | 7786.0 | 7922.0 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| size_only | adapt | 19500 | 9424.8 | 1921.0 | 8145.0 | 8791.0 | 0 | 0 | 0 | 0 | 13250005 | policy setup_u |
| size_hysteresi | adapt | 19500 | 8667.4 | 1997.0 | 6119.0 | 9228.8 | 0 | 0 | 0 | 0 | 12870815 | policy setup_u |
| queue_aware | adapt | 19500 | 10636.6 | 1786.0 | 4560.0 | 5341.0 | 0 | 0 | 0 | 0 | 13541525 | policy setup_u |
| cost_aware | adapt | 19500 | 10917.1 | 2185.5 | 6433.0 | 6580.5 | 0 | 0 | 0 | 0 | 15637340 | policy setup_u |
| full_adaptive | adapt | 19500 | 10163.6 | 2519.0 | 6347.0 | 7739.0 | 0 | 0 | 0 | 0 | 15703035 | policy setup_u |
