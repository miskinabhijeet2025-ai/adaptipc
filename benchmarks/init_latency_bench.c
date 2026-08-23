/*
 * init_latency_bench.c -- median latency of adapt_init() endpoint
 * creation (the eager SHM negotiation path: shm_open + ftruncate + mmap
 * + header init, plus UDS socket/bind), measured over repeated
 * init/shutdown cycles on fixed IPC names ("warm" creation: the shm
 * object is unlinked once up front, so every iteration pays the full
 * create-truncate-map-initialize sequence).
 *
 * Build: cc -O2 -std=c11 -Iinclude init_latency_bench.c libadaptipc.a -o ilb
 * Run:   ./ilb [iterations] [output-file]
 */
#define _POSIX_C_SOURCE 200809L

#include "adapt_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    enum { N = 500 };
    int iters = argc > 1 ? atoi(argv[1]) : 500;
    const char *out_path = argc > 2 ? argv[2]
                                    : "benchmarks/init_latency.txt";

    shm_unlink("/initlat_shm");
    unlink("/tmp/initlat_a.sock");
    unlink("/tmp/initlat_b.sock");

    adapt_config_t pcfg = { .shm_name = "/initlat_shm",
                            .local_sock = "/tmp/initlat_a.sock",
                            .peer_sock = "/tmp/initlat_b.sock",
                            .shm_capacity = 64u << 20 };

    uint64_t *t = malloc(iters * sizeof(uint64_t));
    if (!t) return 1;

    /* warmup */
    for (int i = 0; i < 50; ++i) {
        adapt_ctx_t *c;
        if (adapt_init(ADAPT_ROLE_PRODUCER, &pcfg, &c) != 0) return 2;
        adapt_shutdown(c);
    }

    for (int i = 0; i < iters; ++i) {
        uint64_t t0 = now_ns();
        adapt_ctx_t *c;
        int rc = adapt_init(ADAPT_ROLE_PRODUCER, &pcfg, &c);
        uint64_t t1 = now_ns();
        if (rc != 0) return 3;
        t[i] = t1 - t0;
        adapt_shutdown(c); /* unlinks shm object: next iter is full cold */
    }

    qsort(t, iters, sizeof(uint64_t), cmp_u64);
    FILE *f = fopen(out_path, "w");
    fprintf(f, "# adapt_init() endpoint-creation latency\n");
    fprintf(f, "# method: median of %d producer-role init cycles "
               "(shm_open+ftruncate(64MB)+mmap+header init + "
               "socket()+bind), warm names, CLOCK_MONOTONIC\n", iters);
    fprintf(f, "init_median_ns=%llu\n",
            (unsigned long long)t[iters / 2]);
    fprintf(f, "init_p10_ns=%llu\n", (unsigned long long)t[iters / 10]);
    fprintf(f, "init_p90_ns=%llu\n",
            (unsigned long long)t[(iters * 9) / 10]);
    fclose(f);
    printf("wrote %s (median %llu ns over %d iters)\n", out_path,
           (unsigned long long)t[iters / 2], iters);
    free(t);
    return 0;
}
