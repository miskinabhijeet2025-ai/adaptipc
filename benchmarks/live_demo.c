/*
 * live_demo.c -- live adaptive-routing demonstration for presentation.
 *
 * Runs the REAL AdaptIPC implementation through three workload phases
 * (small control -> bulk -> small again) and prints one snapshot line
 * every N messages:
 *
 *   SNAPSHOT phase=<0|1|2> size=<B> ewma=<B> route=<UDS|SHM>
 *             msgs=<n> switches=<n>
 *
 * The shell wrapper (demo/adaptipc_lab.sh --live) renders these as a
 * dashboard. Every number comes from the library itself
 * (adapt_ewma / adapt_last_route) -- nothing is simulated.
 */
#define _POSIX_C_SOURCE 200809L
#include "adapt_ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PHASE_MSGS 300
#define SNAP_EVERY 25

int main(int argc, char **argv)
{
    unsigned phase_msgs = PHASE_MSGS, snap = SNAP_EVERY;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--phase-msgs") && i + 1 < argc)
            phase_msgs = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--snap-every") && i + 1 < argc)
            snap = (unsigned)strtoul(argv[++i], NULL, 10);
    }
    setenv("ADAPTIPC_EAGER_SHM", "1", 1); /* sequential demo thread */

    char pp[64], cp[64];
    snprintf(pp, sizeof(pp), "/tmp/adaptipc_live_p_%ld", (long)getpid());
    snprintf(cp, sizeof(cp), "/tmp/adaptipc_live_c_%ld", (long)getpid());
    adapt_config_t pc = { .local_sock = pp, .peer_sock = cp,
                          .shm_capacity = 1u << 20 };
    adapt_config_t cc = { .local_sock = cp, .peer_sock = pp,
                          .shm_capacity = 1u << 20 };
    adapt_ctx_t *p, *c;
    if (adapt_init(ADAPT_ROLE_PRODUCER, &pc, &p)) return 3;
    if (adapt_init(ADAPT_ROLE_CONSUMER, &cc, &c)) return 3;

    static unsigned char tx[70000], rx[70000];
    memset(tx, 0x5a, sizeof(tx));
    int prev_route = -1;
    unsigned long switches = 0, total = 0;
    /* drain any stale EWMA history: start from a clean context */

    for (int phase = 0; phase < 3; phase++) {
        size_t sz = (phase == 1) ? 16384 : 512;
        const char *pname = phase == 0 ? "SMALL" :
                            phase == 1 ? "BULK" : "SMALL-AGAIN";
        for (unsigned i = 0; i < phase_msgs; i++) {
            memcpy(tx, &total, sizeof(total));
            int rc;
            do { rc = adapt_send(p, tx, sz); } while (rc == -EAGAIN);
            if (rc) return 5;
            if (adapt_recv(c, rx, sizeof(rx)) != (int)sz) return 6;
            total++;
            int route = (int)adapt_last_route(p);
            if (prev_route >= 0 && route != prev_route) switches++;
            prev_route = route;
            if (i % snap == 0 || i == phase_msgs - 1) {
                printf("SNAPSHOT phase=%s size=%zu ewma=%.0f route=%s "
                       "msgs=%lu switches=%lu\n",
                       pname, sz, adapt_ewma(p),
                       adapt_route_name(route), total, switches);
                fflush(stdout);
            }
        }
    }
    printf("DONE msgs=%lu switches=%lu\n", total, switches);
    adapt_shutdown(p);
    adapt_shutdown(c);
    unlink(pp); unlink(cp);
    return 0;
}
