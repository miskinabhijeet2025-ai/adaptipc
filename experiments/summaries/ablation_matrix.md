# Ablation matrix (payload_sweep + queue_pressure + burst)

| policy | kind | message_count | throughput_mbps | p50_us | p95_us | p99_us | p99_9_us | route_switches | backpressure_events | notes |
|---|---|---|---|---|---|---|---|---|---|---|
| uds | uds | 15000 | 557.2 | 48.0 | 2167.4 | 4659.0 | 4859.0 | 0 | 0 | baseline setup |
| shm | shm | 19500 | 13948.6 | 3346.0 | 7547.0 | 7786.0 | 7922.0 | 0 | 0 | baseline setup |
| size_only | adapt | 19500 | 9424.8 | 1921.0 | 6954.0 | 8145.0 | 8791.0 | 0 | 0 | policy setup_u |
| size_hysteresi | adapt | 19500 | 8667.4 | 1997.0 | 5159.0 | 6119.0 | 9228.8 | 0 | 0 | policy setup_u |
| queue_aware | adapt | 19500 | 10636.6 | 1786.0 | 3900.0 | 4560.0 | 5341.0 | 0 | 0 | policy setup_u |
| cost_aware | adapt | 19500 | 10917.1 | 2185.5 | 5859.0 | 6433.0 | 6580.5 | 0 | 0 | policy setup_u |
| full_adaptive | adapt | 19500 | 10163.6 | 2519.0 | 5744.0 | 6347.0 | 7739.0 | 0 | 0 | policy setup_u |
| uds | uds | 6000 | 8.5 | 26573.0 | 26798.0 | 26871.0 | 26910.0 | 0 | 0 | baseline setup |
| shm | shm | 6000 | 8.0 | 710030.0 | 1394835.6 | 1458661.3 | 1471333.2 | 0 | 0 | baseline setup |
| size_only | adapt | 5883 | 8.2 | 29080.0 | 29540.0 | 31843.0 | 32090.1 | 0 | 0 | dropped=117 (u |
| size_hysteresi | adapt | 5882 | 8.2 | 29206.0 | 29561.0 | 29651.0 | 29780.2 | 0 | 0 | dropped=118 (u |
| queue_aware | adapt | 5883 | 8.2 | 29046.0 | 29387.9 | 29530.2 | 29612.1 | 0 | 0 | dropped=117 (u |
| cost_aware | adapt | 5883 | 8.2 | 29049.0 | 29509.9 | 29596.4 | 29669.0 | 0 | 0 | dropped=117 (u |
| full_adaptive | adapt | 5883 | 8.1 | 29151.0 | 29549.0 | 29625.2 | 29688.2 | 0 | 0 | dropped=117 (u |
| uds | uds | 12000 | 279.1 | 8.0 | 18.0 | 27.0 | 51.0 | 0 | 0 | baseline setup |
| shm | shm | 12000 | 3030.3 | 707.5 | 1294.0 | 1312.0 | 1316.0 | 0 | 0 | baseline setup |
| size_only | adapt | 12000 | 55.6 | 10.0 | 20.0 | 52.0 | 183.0 | 0 | 0 | policy setup_u |
| size_hysteresi | adapt | 12000 | 51.2 | 14.0 | 35.0 | 106.0 | 591.1 | 0 | 0 | policy setup_u |
| queue_aware | adapt | 12000 | 50.6 | 14.0 | 33.0 | 60.0 | 116.0 | 0 | 0 | policy setup_u |
| cost_aware | adapt | 12000 | 49.3 | 16.0 | 37.0 | 57.0 | 111.0 | 0 | 0 | policy setup_u |
| full_adaptive | adapt | 12000 | 52.0 | 13.0 | 31.0 | 64.0 | 124.0 | 0 | 0 | policy setup_u |
