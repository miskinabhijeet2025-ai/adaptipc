# Experiment B: queue pressure

| policy | kind | message_count | throughput_mbps | p50_us | p99_us | p99_9_us | route_switches | uds_messages | shm_messages | health_transitions | mean_queue_occupancy_bytes | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| uds | uds | 6000 | 8.5 | 26573.0 | 26871.0 | 26910.0 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| shm | shm | 6000 | 8.0 | 710030.0 | 1458661.3 | 1471333.2 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| size_only | adapt | 5883 | 8.2 | 29080.0 | 31843.0 | 32090.1 | 0 | 0 | 0 | 0 | 0 | dropped=117 (u |
| size_hysteresi | adapt | 5882 | 8.2 | 29206.0 | 29651.0 | 29780.2 | 0 | 0 | 0 | 0 | 0 | dropped=118 (u |
| queue_aware | adapt | 5883 | 8.2 | 29046.0 | 29530.2 | 29612.1 | 0 | 0 | 0 | 0 | 0 | dropped=117 (u |
| cost_aware | adapt | 5883 | 8.2 | 29049.0 | 29596.4 | 29669.0 | 0 | 0 | 0 | 0 | 0 | dropped=117 (u |
| full_adaptive | adapt | 5883 | 8.1 | 29151.0 | 29625.2 | 29688.2 | 0 | 0 | 0 | 0 | 0 | dropped=117 (u |
