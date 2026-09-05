# Ablation matrix (payload_sweep + queue_pressure + burst)

| policy | kind | message_count | throughput_mbps | p50_us | p95_us | p99_us | p99_9_us | route_switches | backpressure_events | notes |
|---|---|---|---|---|---|---|---|---|---|---|
| uds | uds | 15000 | 300.3 | 52.0 | 1000.0 | 6838.0 | 30953.0 | 0 | 0 | baseline setup |
| shm | shm | 19500 | 12349.5 | 1201.0 | 4983.0 | 6453.0 | 7508.0 | 0 | 0 | baseline setup |
| size_only | adapt | 19500 | 9403.6 | 2157.5 | 6620.0 | 7679.0 | 8475.0 | 1 | 28 | policy setup_u |
| size_hysteresi | adapt | 19500 | 9742.9 | 1955.0 | 5457.0 | 5744.0 | 5911.0 | 1 | 28 | policy setup_u |
| queue_aware | adapt | 19500 | 9598.2 | 2069.5 | 4564.0 | 5330.0 | 5882.5 | 1 | 28 | policy setup_u |
| cost_aware | adapt | 19500 | 9656.2 | 2227.0 | 4315.1 | 5925.0 | 7086.0 | 0 | 21 | policy setup_u |
| full_adaptive | adapt | 19500 | 9444.3 | 2217.0 | 4842.0 | 6500.0 | 7959.6 | 1 | 21 | policy setup_u |
| uds | uds | 6000 | 9.9 | 22573.0 | 23400.1 | 28006.4 | 28821.0 | 0 | 0 | baseline setup |
| shm | shm | 6000 | 10.1 | 580738.5 | 1105713.6 | 1152380.9 | 1162813.2 | 0 | 0 | baseline setup |
| size_only | adapt | 5881 | 9.9 | 23894.0 | 24755.0 | 26375.2 | 26716.2 | 0 | 0 | dropped=119 (u |
| size_hysteresi | adapt | 5882 | 10.1 | 23574.0 | 24029.0 | 26631.3 | 26992.0 | 0 | 0 | dropped=118 (u |
| queue_aware | adapt | 5882 | 10.1 | 23532.0 | 23884.0 | 24491.2 | 24773.4 | 0 | 0 | dropped=118 (u |
| cost_aware | adapt | 5882 | 10.0 | 23628.0 | 24258.9 | 24856.5 | 25175.6 | 0 | 0 | dropped=118 (u |
| full_adaptive | adapt | 5881 | 10.1 | 23476.0 | 23742.0 | 24118.4 | 24268.1 | 0 | 0 | dropped=119 (u |
| uds | uds | 12000 | 404.8 | 6.0 | 39.0 | 76.0 | 415.0 | 0 | 0 | baseline setup |
| shm | shm | 12000 | 2719.0 | 747.0 | 2709.0 | 2726.0 | 2731.0 | 0 | 0 | baseline setup |
| size_only | adapt | 12000 | 60.2 | 6.0 | 22.0 | 73.0 | 104.0 | 0 | 0 | policy setup_u |
| size_hysteresi | adapt | 12000 | 59.8 | 6.0 | 34.0 | 196.0 | 385.0 | 0 | 0 | policy setup_u |
| queue_aware | adapt | 12000 | 59.9 | 6.0 | 52.0 | 219.0 | 291.0 | 0 | 0 | policy setup_u |
| cost_aware | adapt | 12000 | 60.1 | 5.0 | 17.0 | 68.0 | 114.0 | 0 | 0 | policy setup_u |
| full_adaptive | adapt | 12000 | 59.6 | 6.0 | 36.0 | 90.0 | 132.0 | 0 | 0 | policy setup_u |
