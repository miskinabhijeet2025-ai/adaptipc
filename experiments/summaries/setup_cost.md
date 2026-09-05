# Experiment F: setup cost

| policy | kind | message_count | throughput_mbps | p50_us | p99_us | p99_9_us | route_switches | uds_messages | shm_messages | health_transitions | mean_queue_occupancy_bytes | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| eager_shm | adapt | 3000 | 357.0 | 55.0 | 589.0 | 633.0 | 0 | 2000 | 1000 | 49 | 0 | setup setup_us |
| lazy_shm | adapt | 3000 | 558.2 | 22.0 | 362.0 | 427.0 | 0 | 2000 | 1000 | 33 | 0 | setup setup_us |
| lazy_cost_awar | adapt | 3000 | 847.5 | 7.0 | 295.0 | 305.0 | 0 | 2000 | 1000 | 5 | 0 | setup setup_us |
