# Experiment F: setup cost

| policy | kind | message_count | throughput_mbps | p50_us | p99_us | p99_9_us | route_switches | uds_messages | shm_messages | health_transitions | mean_queue_occupancy_bytes | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| eager_shm | adapt | 3000 | 464.4 | 12.0 | 45.0 | 76.0 | 0 | 0 | 0 | 0 | 0 | setup setup_us |
| lazy_shm | adapt | 3000 | 677.4 | 9.0 | 88.0 | 135.0 | 0 | 0 | 0 | 0 | 0 | setup setup_us |
| lazy_cost_awar | adapt | 3000 | 733.0 | 7.0 | 58.0 | 91.0 | 0 | 0 | 0 | 0 | 0 | setup setup_us |
