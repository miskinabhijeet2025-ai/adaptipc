# Experiment G: QoS

| policy | kind | message_count | throughput_mbps | p50_us | p99_us | p99_9_us | route_switches | uds_messages | shm_messages | health_transitions | mean_queue_occupancy_bytes | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| uds | uds | 6000 | 14.5 | 10697.5 | 84104.1 | 96814.2 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| shm | shm | 6000 | 16.3 | 390609.0 | 743173.9 | 747716.1 | 0 | 0 | 0 | 0 | 0 | baseline setup |
| full_adaptive_ | adapt | 5707 | 9.4 | 11440.0 | 468169.8 | 789003.7 | 551 | 981 | 5019 | 34 | 536323 | dropped=293 (u |
| full_adaptive_ | adapt | 5719 | 13.8 | 10694.0 | 484269.2 | 512611.3 | 355 | 832 | 5168 | 52 | 259247 | dropped=281 (u |
| full_adaptive_ | adapt | 5719 | 18.9 | 5011.0 | 230873.7 | 232212.1 | 605 | 1264 | 4736 | 80 | 164575 | dropped=281 (u |
| size_hysteresi | adapt | 5794 | 18.6 | 11743.0 | 129021.9 | 131736.0 | 85 | 2623 | 3377 | 84 | 90599 | dropped=206 (u |
