# Single-process test exercising both transports through adapt_ipc's API:
# producer/consumer roles are simulated in one process via two contexts.
add_executable(test_adapt_ipc test_adapt_ipc.c)
target_link_libraries(test_adapt_ipc PRIVATE adaptipc)
add_test(NAME adapt_ipc COMMAND test_adapt_ipc)

add_executable(test_shm_ringbuffer test_shm_ringbuffer.c)
target_link_libraries(test_shm_ringbuffer PRIVATE adaptipc)
add_test(NAME shm_ringbuffer COMMAND test_shm_ringbuffer)

# Benchmark suite (multi-process; run via scripts/run_benchmarks.sh or ctest)
add_executable(benchmark_suite benchmark_suite.c)
target_link_libraries(benchmark_suite PRIVATE adaptipc)
target_compile_options(benchmark_suite PRIVATE -O3)
add_test(NAME benchmark_smoke
         COMMAND benchmark_suite --mode adapt --workload sweep --out smoke.csv)

# High/low watermark flow control (futex park/wake, no loss, no deadlock)
add_executable(test_flowcontrol test_flowcontrol.c)
target_link_libraries(test_flowcontrol PRIVATE adaptipc)
add_test(NAME flowcontrol COMMAND test_flowcontrol)

# Lazy SHM endpoint negotiation over the UDS control channel
add_executable(test_lazy_negotiation test_lazy_negotiation.c)
target_link_libraries(test_lazy_negotiation PRIVATE adaptipc)
add_test(NAME lazy_negotiation COMMAND test_lazy_negotiation)

# Tail-latency gate: watermark-bounded backlog under backpressure
add_executable(test_backpressure_latency test_backpressure_latency.c)
target_link_libraries(test_backpressure_latency PRIVATE adaptipc)
add_test(NAME backpressure_latency COMMAND test_backpressure_latency)

# Context-aware policy engine: estimator, margin, crossover, health, QoS
add_executable(test_cost_model test_cost_model.c)
target_link_libraries(test_cost_model PRIVATE adaptipc)
add_test(NAME cost_model COMMAND test_cost_model)

# Policy-mode integration: adversarial ladder, degradation, decision log
add_executable(test_policy_modes test_policy_modes.c)
target_link_libraries(test_policy_modes PRIVATE adaptipc)
add_test(NAME policy_modes COMMAND test_policy_modes)

# v2.1: decision-log validation + route-transition accounting
add_executable(test_decision_log_consistency test_decision_log_consistency.c)
target_link_libraries(test_decision_log_consistency PRIVATE adaptipc)
add_test(NAME decision_log_consistency COMMAND test_decision_log_consistency)

add_executable(test_route_transition_accounting test_route_transition_accounting.c)
target_link_libraries(test_route_transition_accounting PRIVATE adaptipc)
add_test(NAME route_transition_accounting COMMAND test_route_transition_accounting)

add_executable(test_queue_occupancy_instrumentation test_queue_occupancy_instrumentation.c)
target_link_libraries(test_queue_occupancy_instrumentation PRIVATE adaptipc)
add_test(NAME queue_occupancy_instrumentation COMMAND test_queue_occupancy_instrumentation)
