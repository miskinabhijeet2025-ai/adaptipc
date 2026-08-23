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
