# Experiment G: QoS

| policy | kind | message_count | throughput_mbps | p50_us | p99_us | p99_9_us | route_switches | uds_messages | shm_messages | health_transitions | mean_queue_occupancy_bytes | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| uds | uds | 6000 | 21.3 | 10396.0 | 11569.1 | 11874.0 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| shm | shm | 6000 | 21.5 | 282535.5 | 559359.9 | 564467.1 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| full_adaptive_ | adapt | 5724 | 22.9 | 7490.5 | 83066.4 | 434486.5 | 0 | 0 | 0 | 0 | 283373 | dropped=276 (u |
| full_adaptive_ | adapt | 5753 | 22.9 | 58401.0 | 256920.1 | 261490.1 | 0 | 0 | 0 | 0 | 1861423 | dropped=247 (u |
| full_adaptive_ | adapt | 5741 | 22.8 | 25787.0 | 142563.4 | 482581.0 | 0 | 0 | 0 | 0 | 870170 | dropped=259 (u |
| size_hysteresi | adapt | 5794 | 22.3 | 10330.5 | 73520.2 | 79993.8 | 0 | 0 | 0 | 0 | 94885 | dropped=206 (u |
