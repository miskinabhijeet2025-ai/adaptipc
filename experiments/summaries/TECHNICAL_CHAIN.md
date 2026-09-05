# Final technical story (v2.1) — does the evidence support the chain?

Application -> single IPC API -> runtime context observation ->
transport-specific cost estimation -> queue/backpressure prediction ->
switch/setup/health penalties -> stability-constrained decision ->
UDS/SHM -> bounded flow control -> runtime feedback -> next decision.

Verdict per link (evidence file in parentheses):
- runtime context observation: SUPPORTED (occupancy_validation.csv --
  instrumentation == cursor math at all fill levels).
- cost estimation: SUPPORTED with a caveat -- deterministic after
  convergence (epsilon 0.00 us steady); linear model (cost_model unit
  tests; crossover_sweep.csv -- learned S* exact for 6 ground truths).
- queue/backpressure prediction: PARTIALLY SUPPORTED -- tracks
  occupancy linearly under active drain; under-predicts 10-1000x when
  the consumer stops draining (queue_prediction_accuracy.csv,
  FAILURE_CASES #6). The staleness guard bounds the damage by falling
  back to the conservative floor.
- switch/setup/health penalties: SUPPORTED (adversarial_delta.csv --
  0 flaps; lazy_setup.csv -- 0.40 vs 1.82 ms init; degradation runs).
- stability-constrained decision: SUPPORTED (margin sweep -- 0 noise
  switches at any H; adversarial delta -- false_switch_rate 0.00).
- bounded flow control: SUPPORTED (backpressure gate -- 14-frame
  bounded backlog vs unbounded pure ring).

Weakest link: queue prediction under consumer scheduling changes
(reactive, not predictive). Strongest links: stability margin and
bounded flow control (exact, deterministic mechanisms).
