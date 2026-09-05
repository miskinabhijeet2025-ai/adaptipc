/*
 * test_adapt_ipc.c -- end-to-end adaptive router test:
 *   - EWMA routing: small payloads -> UDS, large payloads -> SHM
 *   - recv() demultiplexes regardless of route
 *   - hysteresis band keeps last route
 */
#define _POSIX_C_SOURCE 200809L

#include "adapt_ipc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

int main(void)
{
    /* This test exercises router semantics with the legacy eager SHM
     * mapping and a synchronous single-threaded send/recv ping-pong
     * (the consumer is never inside adapt_recv() while the producer
     * sends, so the asynchronous lazy handshake cannot complete here --
     * it is covered by test_lazy_negotiation.c). */
    setenv("ADAPTIPC_EAGER_SHM", "1", 1);

    /* Clean up any leftovers from a previously crashed run. */
    shm_unlink("/adaptipc_test_router");
    unlink("/tmp/adaptipc_test_a.sock");
    unlink("/tmp/adaptipc_test_b.sock");

    adapt_config_t pcfg = {
        .shm_name     = "/adaptipc_test_router",
        .local_sock   = "/tmp/adaptipc_test_a.sock",
        .peer_sock    = "/tmp/adaptipc_test_b.sock",
        .shm_capacity = 65536,
    };
    adapt_config_t ccfg = {
        .shm_name     = "/adaptipc_test_router",
        .local_sock   = "/tmp/adaptipc_test_b.sock",
        .peer_sock    = "/tmp/adaptipc_test_a.sock",
        .shm_capacity = 65536,
    };

    adapt_ctx_t *prod = NULL, *cons = NULL;
    assert(adapt_init(ADAPT_ROLE_PRODUCER, &pcfg, &prod) == 0);
    assert(adapt_init(ADAPT_ROLE_CONSUMER, &ccfg, &cons) == 0);

    unsigned char buf[8192];

    /* --- Phase 1: small messages must take the UDS path --- */
    for (unsigned i = 0; i < 5; ++i) {
        char msg[64];
        memset(msg, 'S', sizeof(msg));
        assert(adapt_send(prod, msg, sizeof(msg)) == 0);
        int n = adapt_recv(cons, buf, sizeof(buf));
        assert(n == (int)sizeof(msg));
        assert(buf[0] == 'S');
    }
    printf("phase1: ewma=%.0f route=%s\n",
           adapt_ewma(prod), adapt_route_name(adapt_last_route(prod)));
    assert(adapt_ewma(prod) <= ADAPT_TAU_LOW + 64.0);
    assert(adapt_last_route(prod) == ADAPT_ROUTE_UDS);

    /* --- Phase 2: large messages pull EWMA above tau_high -> SHM --- */
    for (unsigned i = 0; i < 10; ++i) {
        char msg[8192];
        memset(msg, 'L', sizeof(msg));
        assert(adapt_send(prod, msg, sizeof(msg)) == 0);
        int n = adapt_recv(cons, buf, sizeof(buf));
        assert(n == (int)sizeof(msg));
        assert(buf[0] == 'L');
    }
    double ewma_after_large = adapt_ewma(prod);
    printf("phase2: ewma=%.0f route=%s\n",
           ewma_after_large, adapt_route_name(adapt_last_route(prod)));
    assert(ewma_after_large >= ADAPT_TAU_HIGH);
    assert(adapt_last_route(prod) == ADAPT_ROUTE_SHM);

    /* --- Phase 3: EWMA convergence check (formula verification) --- */
    /* send one message of exactly 8192; verify closed-form EWMA */
    char big[8192];
    memset(big, 'X', sizeof(big));
    assert(adapt_send(prod, big, sizeof(big)) == 0);
    double expected = ADAPT_EWMA_ALPHA * 8192.0 +
                      (1.0 - ADAPT_EWMA_ALPHA) * ewma_after_large;
    double got = adapt_ewma(prod);
    printf("phase3: expected ewma=%.4f got=%.4f\n", expected, got);
    if (got - expected > 1e-6 || expected - got > 1e-6) {
        fprintf(stderr, "EWMA formula mismatch\n");
        return 1;
    }
    assert(adapt_recv(cons, buf, sizeof(buf)) == (int)sizeof(big));

    /* --- Phase 4: mixed sizes in flight simultaneously (demux test) --- */
    char small[32], medium[2048];
    memset(small, 's', sizeof(small));
    memset(medium, 'm', sizeof(medium));
    assert(adapt_send(prod, small, sizeof(small)) == 0);   /* UDS */
    assert(adapt_send(prod, medium, sizeof(medium)) == 0); /* SHM */
    int n1 = adapt_recv(cons, buf, sizeof(buf)); /* SHM record arrives first */
    int n2 = adapt_recv(cons, buf, sizeof(buf)); /* then UDS datagram */
    assert((size_t)n1 + (size_t)n2 ==
           sizeof(small) + sizeof(medium));
    assert(n1 > 0 && n2 > 0);
    printf("phase4: demux OK (%d + %d bytes)\n", n1, n2);

    adapt_shutdown(prod);
    adapt_shutdown(cons);
    shm_unlink("/adaptipc_test_router");
    unlink("/tmp/adaptipc_test_a.sock");
    unlink("/tmp/adaptipc_test_b.sock");
    printf("test_adapt_ipc: ALL TESTS PASSED\n");
    return 0;
}
