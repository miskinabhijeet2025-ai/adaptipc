/*
 * routing_trace.c -- capture a real per-message routing trace from the
 * running AdaptIPC implementation.
 *
 * Sends a deterministic mixed workload (small control messages, bulk
 * bursts, back to small) through adapt_send()/adapt_recv() and records
 * one CSV row per message:
 *
 *   seq,payload_bytes,ewma_bytes,route,switched
 *
 * The EWMA and route are read back from the library itself
 * (adapt_ewma()/adapt_last_route()) -- nothing is simulated.
 *
 * Build:
 *   cc -std=c11 -O3 -Wall -Wextra -pthread -Iinclude \
 *      benchmarks/routing_trace.c src/[sources] -o routing_trace
 * Usage:
 *   routing_trace --out FILE.csv [--iters N]
 */
#define _POSIX_C_SOURCE 200809L
#include "adapt_ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PHASE_MSGS 400

int main(int argc, char **argv)
{
    const char *out = "routing_trace.csv";
    unsigned long iters = 3 * PHASE_MSGS;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc)
            iters = strtoul(argv[++i], NULL, 10);
        else { fprintf(stderr, "usage: %s [--out F] [--iters N]\n",
                       argv[0]); return 2; }
    }
    if (iters < 3) return 2;

    char pp[64], cp[64];
    snprintf(pp, sizeof(pp), "/tmp/adaptipc_trace_p_%ld", (long)getpid());
    snprintf(cp, sizeof(cp), "/tmp/adaptipc_trace_c_%ld", (long)getpid());
    adapt_config_t pc = { .local_sock = pp, .peer_sock = cp,
                          .shm_capacity = 1u << 20 };
    adapt_config_t cc = { .local_sock = cp, .peer_sock = pp,
                          .shm_capacity = 1u << 20 };
    /* Eager SHM: the trace is sequential (send then recv in one
     * thread), so the asynchronous lazy handshake cannot complete
     * mid-trace. Eager mapping does not change routing decisions. */
    setenv("ADAPTIPC_EAGER_SHM", "1", 1);
    adapt_ctx_t *p, *c;
    if (adapt_init(ADAPT_ROLE_PRODUCER, &pc, &p)) return 3;
    if (adapt_init(ADAPT_ROLE_CONSUMER, &cc, &c)) return 3;

    FILE *f = fopen(out, "w");
    if (!f) { perror(out); return 4; }
    fprintf(f, "seq,payload_bytes,ewma_bytes,route,switched\n");

    static unsigned char tx[70000], rx[70000];
    memset(tx, 0x5a, sizeof(tx));
    int prev_route = -1;
    unsigned long switches = 0;

    /* Three phases with distinct traffic classes so the trace shows a
     * genuine UDS -> SHM -> UDS adaptation driven by the EWMA:
     *   phase 0: 512 B control messages          -> UDS
     *   phase 1: 16 KB bulk messages             -> SHM
     *   phase 2: 512 B control messages again    -> back through the
     *                                             deadband to UDS
     */
    for (unsigned long i = 0; i < iters; i++) {
        size_t sz;
        unsigned long phase = i * 3 / iters;   /* 0, 1, 2 */
        sz = (phase == 1) ? 16384 : 512;
        memcpy(tx, &i, sizeof(i));

        int rc;
        do { rc = adapt_send(p, tx, sz); } while (rc == -EAGAIN);
        if (rc) { fprintf(stderr, "send failed: %s\n", strerror(-rc));
                  return 5; }
        if (adapt_recv(c, rx, sizeof(rx)) != (int)sz) {
            fprintf(stderr, "recv failed\n");
            return 6;
        }

        int route = (int)adapt_last_route(p);
        int switched = prev_route >= 0 && route != prev_route;
        if (switched) switches++;
        fprintf(f, "%lu,%zu,%.1f,%s,%d\n", i, sz, adapt_ewma(p),
                adapt_route_name(route),
                switched);
        prev_route = route;
    }

    fprintf(stderr, "routing trace: %lu messages, %lu switches -> %s\n",
            iters, switches, out);
    fclose(f);
    adapt_shutdown(p);
    adapt_shutdown(c);
    unlink(pp); unlink(cp);
    return 0;
}
