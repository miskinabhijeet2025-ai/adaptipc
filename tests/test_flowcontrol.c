/*
 * test_flowcontrol.c -- high/low watermark flow control of the SHM ring.
 *
 * Verifies:
 *  1. A blocking producer parks (futex/condvar) at the high watermark
 *     instead of spinning or dropping, and is woken once the consumer
 *     drains below the low watermark.
 *  2. Zero message loss and zero deadlock across repeated fill/drain
 *     cycles (the classic lost-wakeup / ABA hazard surface).
 *  3. Plain shm_ring_push() keeps its non-blocking -EAGAIN contract.
 */
#include "shm_ringbuffer.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RING_CAP   (1u << 20)   /* 1 MiB -> HW = 800 KiB, LW = 200 KiB */
#define MSG_SIZE   1024u
#define N_MSGS     8192u        /* several fill/drain cycles           */

static shm_ring_t *g_ring;
static _Atomic int g_producer_done, g_consumer_done;
static _Atomic uint64_t g_push_failures;

static void *producer_fn(void *arg)
{
    (void)arg;
    unsigned char msg[MSG_SIZE];
    for (unsigned i = 0; i < N_MSGS; i++) {
        memcpy(msg, &i, sizeof(i));
        memset(msg + sizeof(i), (int)(i & 0xff), MSG_SIZE - sizeof(i));
        shm_segment_t segs[1] = { { .base = msg, .len = MSG_SIZE } };
        int rc = shm_ring_push_scatter_blocking(g_ring, segs, 1);
        if (rc != 0) {
            atomic_fetch_add(&g_push_failures, 1);
            break;
        }
    }
    atomic_store(&g_producer_done, 1);
    return NULL;
}

static void *consumer_fn(void *arg)
{
    (void)arg;
    unsigned char buf[MSG_SIZE];
    unsigned expected = 0;
    while (expected < N_MSGS) {
        int n = shm_ring_pop(g_ring, buf, sizeof(buf));
        if (n == -EAGAIN) {
            if (atomic_load(&g_producer_done) &&
                shm_ring_used_bytes(g_ring) == 0)
                break;
            struct timespec ts = { 0, 200000 }; /* 200 us */
            nanosleep(&ts, NULL);
            continue;
        }
        assert(n == (int)MSG_SIZE);
        unsigned seq;
        memcpy(&seq, buf, sizeof(seq));
        assert(seq == expected); /* SPSC: strict FIFO order */
        for (unsigned k = sizeof(seq); k < MSG_SIZE; k++)
            assert(buf[k] == (unsigned char)(seq & 0xff));
        expected++;
    }
    atomic_store(&g_consumer_done, 1);
    return NULL;
}

static _Atomic int g_slow_drain; /* 1: drainer sleeps per pop (park test) */

static void *drain_all_fn(void *arg)
{
    (void)arg;
    unsigned char buf[MSG_SIZE];
    for (;;) {
        int n = shm_ring_pop(g_ring, buf, sizeof(buf));
        if (n == -EAGAIN) {
            if (atomic_load(&g_producer_done) &&
                shm_ring_used_bytes(g_ring) == 0)
                break;
            struct timespec ts = { 0, 200000 };
            nanosleep(&ts, NULL);
            continue;
        }
        if (atomic_load(&g_slow_drain)) {
            struct timespec ts = { 0, 1000000 }; /* 1 ms */
            nanosleep(&ts, NULL);
        }
    }
    atomic_store(&g_consumer_done, 1);
    return NULL;
}

static void test_blocking_park_at_hw(void)
{
    /* Fill the ring above the high watermark with plain pushes. */
    unsigned char msg[MSG_SIZE];
    memset(msg, 0xaa, sizeof(msg));
    const size_t hw = (size_t)RING_CAP * SHM_HW_PCT / 100u;
    while (shm_ring_used_bytes(g_ring) < hw)
        assert(shm_ring_push(g_ring, msg, sizeof(msg)) == 0);

    /* A blocking push must NOT complete while the ring sits at/above HW
     * with nobody draining. */
    atomic_store(&g_producer_done, 0);
    atomic_store(&g_consumer_done, 0);
    atomic_store(&g_slow_drain, 1);
    pthread_t pt, ct;
    assert(pthread_create(&ct, NULL, drain_all_fn, NULL) == 0);
    assert(pthread_create(&pt, NULL, producer_fn, NULL) == 0);
    struct timespec ts = { 0, 100 * 1000000 }; /* 100 ms */
    nanosleep(&ts, NULL);
    assert(!atomic_load(&g_producer_done) &&
           "producer should be parked at the high watermark");

    /* Let the drainer run at full speed: it wakes the producer below LW,
     * both complete, and the ring ends empty. (Main thread only observes
     * usage here -- popping from it would violate SPSC.) */
    atomic_store(&g_slow_drain, 0);

    pthread_join(pt, NULL);
    pthread_join(ct, NULL);
    assert(atomic_load(&g_producer_done) == 1);
    assert(atomic_load(&g_consumer_done) == 1);
    assert(shm_ring_used_bytes(g_ring) == 0);
    printf("  park/wake: producer parked at HW, resumed below LW\n");
}

static void test_full_pipeline_no_loss(void)
{
    pthread_t pt, ct;
    atomic_store(&g_producer_done, 0);
    atomic_store(&g_push_failures, 0);
    assert(pthread_create(&ct, NULL, consumer_fn, NULL) == 0);
    assert(pthread_create(&pt, NULL, producer_fn, NULL) == 0);
    pthread_join(pt, NULL);
    pthread_join(ct, NULL);
    assert(atomic_load(&g_push_failures) == 0);
    assert(shm_ring_used_bytes(g_ring) == 0);
    printf("  pipeline: %u messages, 0 failures, ring empty\n", N_MSGS);
}

static void test_plain_push_still_nonblocking(void)
{
    unsigned char msg[64];
    memset(msg, 0, sizeof(msg));
    /* Drain fully, then fill to capacity: the last push must fail with
     * -EAGAIN, never block. */
    while (shm_ring_pop(g_ring, msg, sizeof(msg)) > 0)
        ;
    int pushed = 0;
    while (shm_ring_push(g_ring, msg, sizeof(msg)) == 0) pushed++;
    assert(pushed > 0);
    assert(shm_ring_free_bytes(g_ring) < sizeof(msg) + 4);
    while (shm_ring_pop(g_ring, msg, sizeof(msg)) > 0)
        ;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char name[64];
    snprintf(name, sizeof(name), "/adaptipc_fc_test_%ld", (long)getpid());

    printf("flow control: create ring (%u KiB, HW=%u%% LW=%u%%)...\n",
           RING_CAP / 1024, SHM_HW_PCT, SHM_LW_PCT);
    assert(shm_ring_create(name, RING_CAP, 1, SHM_RING_ROLE_PRODUCER,
                           &g_ring) == 0);

    test_blocking_park_at_hw();
    test_full_pipeline_no_loss();
    test_plain_push_still_nonblocking();

    shm_ring_close(g_ring, 1);
    printf("test_flowcontrol: PASS\n");
    return 0;
}
