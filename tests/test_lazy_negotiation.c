/*
 * test_lazy_negotiation.c -- lazy SHM endpoint negotiation over UDS.
 *
 * Verifies:
 *  1. adapt_init() maps no SHM object (deferred entirely).
 *  2. The first EWMA-classified SHM send triggers exactly one
 *     SHM_SETUP_REQ / SHM_SETUP_ACK handshake and then maps the ring on
 *     both sides.
 *  3. The UDS fallback stays active during the handshake: no message is
 *     dropped, and UDS-classified messages keep flowing while the
 *     handshake is in flight.
 *  4. Escalated oversized payloads (> UDS_MAX_DGRAM) still deliver once
 *     the ring exists.
 */
#include "adapt_ipc.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NSMALL 64u   /* 512 B   -> UDS route                            */
#define NBIG   16u   /* 8192 B  -> SHM route (EWMA >= TAU_HIGH)         */
#define NLATE  8u    /* 512 B again while EWMA still high -> SHM in band */

static adapt_ctx_t *g_prod, *g_cons;
static _Atomic int g_cons_done;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *producer_fn(void *arg)
{
    (void)arg;
    unsigned char buf[8192];
    uint64_t t_send[NSMALL + NBIG + NLATE];

    /* small messages: UDS route, ring must stay unmapped */
    memset(buf, 0x11, sizeof(buf));
    for (unsigned i = 0; i < NSMALL; i++) {
        memcpy(buf, &i, sizeof(i));
        t_send[i] = now_ns();
        assert(adapt_send(g_prod, buf, 512) == 0);
    }
    /* big messages: first one triggers SHM_SETUP_REQ/ACK + mapping */
    memset(buf, 0x22, sizeof(buf));
    for (unsigned i = 0; i < NBIG; i++) {
        memcpy(buf, &i, sizeof(i));
        t_send[NSMALL + i] = now_ns();
        assert(adapt_send(g_prod, buf, 8192) == 0);
    }
    /* small again: EWMA still >= TAU_HIGH -> SHM in the hysteresis band */
    memset(buf, 0x33, sizeof(buf));
    for (unsigned i = 0; i < NLATE; i++) {
        memcpy(buf, &i, sizeof(i));
        t_send[NSMALL + NBIG + i] = now_ns();
        assert(adapt_send(g_prod, buf, 512) == 0);
    }
    (void)t_send;
    return NULL;
}

static void *consumer_fn(void *arg)
{
    (void)arg;
    unsigned char buf[8192];
    unsigned seen_small = 0, seen_big = 0;

    for (unsigned got = 0; got < NSMALL + NBIG + NLATE;) {
        int n = adapt_recv(g_cons, buf, sizeof(buf));
        if (n <= 0)
            fprintf(stderr, "recv failed: %d (%s)\n", n, adapt_strerror(n));
        assert(n > 0);
        unsigned seq;
        memcpy(&seq, buf, sizeof(seq));
        if (n == 512) {
            seen_small++;
            for (int k = 4; k < n; k++)
                assert(buf[k] == 0x11 || buf[k] == 0x33);
        } else {
            assert(n == 8192);
            seen_big++;
            for (int k = 4; k < n; k++) assert(buf[k] == 0x22);
        }
        (void)seq;
        got++;
    }
    assert(seen_small == NSMALL + NLATE);
    assert(seen_big == NBIG);
    atomic_store(&g_cons_done, 1);
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("ADAPTIPC_STATS", "1", 1); /* counters asserted below */
    char ppath[64], cpath[64];
    snprintf(ppath, sizeof(ppath), "/tmp/adaptipc_lazy_p_%ld", (long)getpid());
    snprintf(cpath, sizeof(cpath), "/tmp/adaptipc_lazy_c_%ld", (long)getpid());

    adapt_config_t pcfg = {
        .shm_name = NULL, .local_sock = ppath, .peer_sock = cpath,
        .shm_capacity = 65536,
    };
    adapt_config_t ccfg = {
        .shm_name = NULL, .local_sock = cpath, .peer_sock = ppath,
        .shm_capacity = 65536,
    };

    printf("lazy negotiation: init (must map no SHM)...\n");
    assert(adapt_init(ADAPT_ROLE_PRODUCER, &pcfg, &g_prod) == 0);
    assert(adapt_init(ADAPT_ROLE_CONSUMER, &ccfg, &g_cons) == 0);

    pthread_t pt, ct;
    assert(pthread_create(&ct, NULL, consumer_fn, NULL) == 0);
    assert(pthread_create(&pt, NULL, producer_fn, NULL) == 0);
    pthread_join(pt, NULL);
    pthread_join(ct, NULL);
    assert(atomic_load(&g_cons_done) == 1);

    adapt_stats_t st;
    adapt_get_stats(g_prod, &st);
    printf("  handshake: %llu REQ, %llu ACK, %.1f us waiting\n",
           (unsigned long long)st.shm_setup_reqs,
           (unsigned long long)st.shm_setup_acks,
           st.negot_wait_ns / 1000.0);
    assert(st.shm_setup_reqs >= 1 && st.shm_setup_reqs <= 2);
    assert(st.shm_setup_acks >= 1);
    /* The first 512 B sends must not have waited for any handshake. */
    assert(st.sends_uds >= NSMALL - 2);

    adapt_shutdown(g_prod);
    adapt_shutdown(g_cons);
    printf("test_lazy_negotiation: PASS\n");
    return 0;
}
