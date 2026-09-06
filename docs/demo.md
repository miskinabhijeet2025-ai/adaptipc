# AdaptIPC Demo

## 30-second demonstration

```sh
./showcase/run_demo.sh
# open http://localhost:8123/dashboard/index.html
# press ▶ Play -- the dashboard steps through 512 REAL policy decisions
```

What you see: payload, ring occupancy, estimated SHM/UDS costs, queue
wait, switching margin, the selected transport, and the reason for
every decision — captured from the real C implementation
(`benchmarks/decision_demo.c`, `full_adaptive` policy) via the
library's decision log.

The routing timeline (green SHM / blue UDS) shows the workload moving
small -> bulk -> mixed -> small and the policy following it.

## Longer demos

* Terminal lab walkthrough (build + tests + experiments + report):
  `./demo/adaptipc_lab.sh -presentation`
* Live router dashboard in the terminal: `./demo/adaptipc_lab.sh -live`
* Full research campaign: `./demo/adaptipc_lab.sh -runall`

No hosted video exists yet; the dashboard is the live demonstration.
