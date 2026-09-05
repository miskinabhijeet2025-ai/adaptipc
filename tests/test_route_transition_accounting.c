/*
 * test_route_transition_accounting.c -- the library's route_switches
 * counter must equal the number of route transitions observed through
 * adapt_last_route() across a workload that crosses the decision
 * boundary repeatedly (with the escape hysteresis damping noise).
 */
#define _POSIX_C_SOURCE 200809L
#include "adapt_ipc.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    setenv("ADAPTIPC_STATS", "1", 1);
    setenv("ADAPTIPC_EAGER_SHM", "1", 1);
    char pp[64], cp[64];
    snprintf(pp, 64, "/tmp/aip_rta_p_%ld", (long)getpid());
    snprintf(cp, 64, "/tmp/aip_rta_c_%ld", (long)getpid());
    adapt_config_t pc = { .local_sock = pp, .peer_sock = cp,
        .shm_capacity = 65536, .policy = ADAPT_POLICY_QUEUE_AWARE };
    adapt_config_t cc = { .local_sock = cp, .peer_sock = pp,
        .shm_capacity = 65536 };
    adapt_ctx_t *p, *c;
    assert(adapt_init(ADAPT_ROLE_PRODUCER, &pc, &p) == 0);
    assert(adapt_init(ADAPT_ROLE_CONSUMER, &cc, &c) == 0);

    static unsigned char tx[20000], rx[20000];
    memset(tx, 0x5a, sizeof(tx));
    int prev = -1, observed = 0;
    for (int i = 0; i < 200; i++) {
        size_t sz = (i % 4 < 2) ? 512 : 16384;
        int rc;
        do { rc = adapt_send(p, tx, sz); } while (rc == -EAGAIN);
        assert(rc == 0);
        int route = (int)adapt_last_route(p);
        if (prev >= 0 && route != prev) observed++;
        prev = route;
        assert(adapt_recv(c, rx, sizeof(rx)) == (int)sz);
    }
    adapt_stats_t st;
    adapt_get_stats(p, &st);
    printf("observed=%d counted=%llu\n", observed,
           (unsigned long long)st.route_switches);
    assert((unsigned long long)observed == st.route_switches);
    adapt_shutdown(p); adapt_shutdown(c);
    printf("test_route_transition_accounting: PASS\n");
    return 0;
}
