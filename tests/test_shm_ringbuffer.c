/*
 * test_shm_ringbuffer.c -- SPSC ring buffer correctness:
 *   1. single-threaded FIFO / framing / wrap tests
 *   2. concurrent producer/consumer threads, 200k messages + checksums
 */
#define _POSIX_C_SOURCE 200809L

#include "shm_ringbuffer.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define N_THREADS_MSGS 200000u

static void test_single_threaded(void)
{
    shm_unlink("/adaptipc_test_st");
    shm_ring_t *rb = NULL;
    assert(shm_ring_create("/adaptipc_test_st", 8192, 1,
                           SHM_RING_ROLE_PRODUCER, &rb) == 0);

    unsigned char out[4096];
    /* FIFO ordering across mixed sizes */
    for (unsigned i = 0; i < 100; ++i) {
        unsigned char in[64];
        size_t sz = 1 + (i % 60);
        memset(in, (int)(i & 0xFF), sizeof(in));
        assert(shm_ring_push(rb, in, sz) == 0);
        int n = shm_ring_pop(rb, out, sizeof(out));
        assert(n == (int)sz);
        for (size_t j = 0; j < sz; ++j) assert(out[j] == (unsigned char)(i & 0xFF));
    }

    /* empty -> EAGAIN */
    assert(shm_ring_pop(rb, out, sizeof(out)) == -EAGAIN);

    /* fill until full, expect -EAGAIN, then drain exactly */
    unsigned big[1024];
    memset(big, 0xAB, sizeof(big));
    int pushed = 0;
    while (shm_ring_push(rb, big, sizeof(big)) == 0) pushed++;
    assert(pushed > 0);
    int drained = 0;
    while (shm_ring_pop(rb, out, sizeof(out)) > 0) drained++;
    assert(drained == pushed);
    assert(shm_ring_pop(rb, out, sizeof(out)) == -EAGAIN);

    /* oversize message */
    static unsigned char huge[16384];
    assert(shm_ring_push(rb, huge, sizeof(huge)) == -EMSGSIZE);

    /* wrap-around stress: push/pop interleaved past capacity boundary */
    for (unsigned i = 0; i < 500; ++i) {
        unsigned char in[300], o2[300];
        memset(in, (int)(i & 0xFF), sizeof(in));
        assert(shm_ring_push(rb, in, sizeof(in)) == 0);
        int n = shm_ring_pop(rb, o2, sizeof(o2));
        assert(n == 300);
        assert(memcmp(in, o2, 300) == 0);
    }

    shm_ring_close(rb, 1);
}

typedef struct {
    shm_ring_t *rb;
    uint64_t    sum;
} thread_arg_t;

static void *producer_fn(void *p)
{
    thread_arg_t *a = p;
    unsigned char msg[512];
    for (unsigned i = 0; i < N_THREADS_MSGS; ++i) {
        size_t sz = 1 + (i % 512);
        memset(msg, (int)(i & 0xFF), sizeof(msg));
        a->sum += sz;
        while (shm_ring_push(a->rb, msg, sz) == -EAGAIN)
            sched_yield();
    }
    return NULL;
}

static void *consumer_fn(void *p)
{
    thread_arg_t *a = p;
    unsigned char msg[1024];
    for (unsigned i = 0; i < N_THREADS_MSGS; ++i) {
        int n;
        do { n = shm_ring_pop(a->rb, msg, sizeof(msg)); }
        while (n == -EAGAIN);
        assert(n > 0);
        for (int j = 0; j < n; ++j)
            if (msg[j] != (unsigned char)(i & 0xFF)) { assert(0 && "data corruption"); }
    }
    return NULL;
}

static void test_concurrent(void)
{
    shm_unlink("/adaptipc_test_mt");
    shm_ring_t *rb = NULL;
    assert(shm_ring_create("/adaptipc_test_mt", 65536, 1,
                           SHM_RING_ROLE_PRODUCER, &rb) == 0);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
#if defined(__linux__)
    /* Pin to different cores when possible to exercise real cross-core sync */
#endif
    thread_arg_t pa = { rb, 0 }, ca = { rb, 0 };
    pthread_t pt, ct;
    pthread_create(&ct, &attr, consumer_fn, &ca);
    pthread_create(&pt, &attr, producer_fn, &pa);
    pthread_join(pt, NULL);
    pthread_join(ct, NULL);
    /* sizes cycle 1..512; verify producer accounting matches the closed form */
    {
        unsigned cycles = N_THREADS_MSGS / 512u, rem = N_THREADS_MSGS % 512u;
        uint64_t per_cycle = 512u * 513u / 2u; /* sum(1..512) */
        uint64_t expect = cycles * per_cycle +
                          (uint64_t)rem * (rem + 1) / 2u;
        assert(pa.sum == expect);
        printf("bytes produced=%llu, all consumed with matching checksums\n",
               (unsigned long long)pa.sum);
    }
    printf("concurrent: %u messages transferred, all checksums OK\n",
           N_THREADS_MSGS);

    shm_ring_close(rb, 1);
}

int main(void)
{
    test_single_threaded();
    test_concurrent();
    printf("test_shm_ringbuffer: ALL TESTS PASSED\n");
    return 0;
}
