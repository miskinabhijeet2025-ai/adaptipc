# Professor Demonstration Script (8-10 minutes)

Preparation (before class): `git pull`, then run
`./demo/adaptipc_lab.sh -presentation` once to warm the build cache.

| Time | What you run | What is shown | What you say | What they learn |
|---|---|---|---|---|
| 0:00 | (slide) | the problem | "One IPC mechanism never fits all payload sizes; sockets are simple but slow for bulk, shared memory is fast but complex." | the motivation |
| 0:45 | (slide) | AdaptIPC idea | "One API; each message is routed by an EWMA of its payload size with a hysteresis deadband." | the core algorithm |
| 1:30 | `cat include/adapt_ipc.h` | the public API | "The whole router is six calls; the transport choice is invisible to the app." | architectural simplicity |
| 2:00 | (browser) GitHub repo | repo layout | "Source, tests, benchmarks, raw experiment data, the paper -- all traceable." | scientific structure |
| 2:30 | `./demo/adaptipc_lab.sh -presentation` | live pipeline | "Build, ten test suites, then a fresh experiment -- everything from this run." | correctness first, then measurement |
| 4:00 | `cat experiments/runs/<latest>/raw/transport_adapt.csv` | raw CSV | "No number exists before this file is produced." | raw data precedes all claims |
| 4:30 | `open experiments/runs/<latest>/report/report.html` | report + graph | "The graph is generated from the CSV you just saw." | data -> graph traceability |
| 5:30 | report: Adaptive Routing figure | EWMA + thresholds + route strip | "The transport flips only when the EWMA crosses a threshold -- the raw 16 KB messages alone do not flip it." | the routing mechanism |
| 6:30 | report: Hysteresis figure + table | switch counts | "Without the deadband the route thrashes; with it, switches collapse." | why hysteresis matters |
| 7:30 | report: tables | transport table | "Median latency per payload size, measured on this laptop, this morning." | reproducibility |
| 8:30 | report: Conclusions | findings | "Findings are limited to what the tables show." | scientific honesty |
| 9:30 | (optional) `./demo/adaptipc_lab.sh -live` | live dashboard | "Watch the EWMA cross the threshold and the transport switch in real time." | it is genuinely adaptive |

Questions to expect:
- "Is the data real?" -- every figure is generated from raw CSVs in the
  run directory; regenerate with `scripts/lab_analyze.py`.
- "Why does AdaptIPC not always win?" -- it trades a small margin to
  gain the best transport per size; see the transport comparison
  crossover.
- "How do I reproduce this?" -- README 'One-command Experiment Lab';
  each run gets a unique timestamped directory.
