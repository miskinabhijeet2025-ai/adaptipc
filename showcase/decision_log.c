/*
 * decision_log.c -- AdaptIPC per-decision log demonstrator.
 *
 * Calls the REAL AdaptIPC library and writes one JSON object per
 * routing decision to the output file.  adapt_ewma() and
 * adapt_last_route() come from the library itself; nothing is faked.
 *
 * Build (matches the project flags):
 *   cc -std=c11 -O3 -Wall -Wextra -Wpedantic -pthread -Iinclude \
 *      showcase/decision_log.c src/<sources> -o build-lab/decision_log
 *
 * Usage:
 *   decision_log --out FILE.jsonl [--msgs N] [--seed S]
 */
#define _POSIX_C_SOURCE 200809L
#include "adapt_ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_MSGS 600

/* ---- cost estimator (matches benchmarks/cost_model_constants.txt) ----
 *
 *   cost_T(S) = fixed_T + slope_T * S
 *
 *   SHM: fi *   SHM: fi *   SHM: fi *   SHM: fi *   SHM: fi *   SHM: fi *   SHM: fi *   0 us  (datagram syscall + copyout), slope ~ 18 ns/B
 *
 * Same shape as cost_model.c; the demo uses the measured defaults so
 * the dashboard stays reproducible without per-run calibration.
 */
static inline double est_shm_us(size_t s) {
    return 0.020 + 1.43e-2 * (double)s / 1000.0;
}
static inline double est_uds_us(size_t s) {
    return 2.500 + 1.80e-2 * (double)s / 1000.0;
}

static const char *reason_text(int switched, int payload, double ewma) {
    if (switched) {
        if (payload >= 4096) return "ewma crossed TAU_HIGH -> SHM";
        if (payload <= 1024) return "ewma crossed TAU_LOW -> UDS";
        return "margin-triggered switch (cost-aware)";
    }
    if (payload >= 4096 || ewma >= 4096.0) return "bulk payload: stay on SHM";
    if (payload <= 1024 && ewma <= 1020.0)  return "control payload: stay on UDS";
    return "hysteresis band: hold prior route";
}

int main(int argc, char **argv)
{
    const char *out = "showcase/outputs/decision_log.jsonl";
    unsigned long msgs = DEFAULT_MSGS;
    unsigned seed = (unsigned)time(NULL);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--msgs") && i + 1 < argc) msgs = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (unsigned)strtoul(argv[++i], NULL, 10);
        else { fprintf(stderr, "usage: %s [--out F] [--msgs N] [--seed S]\n", argv[0]); return 2; }
    }
    if (msgs < 6) return 2;
    srand(seed);

    char pp[64], cp[64];
    snprintf(pp, sizeof(pp), "/tmp/adaptipc_dlog_p_%ld", (long)getpid());
    snprintf(cp, sizeof(cp), "/tmp/adaptipc_dlog_c_%ld", (long)getpid());
    adapt_config_t pc = { .local_sock = pp, .peer_sock = cp, .shm_capacity = 1u << 20 };
    adapt_config_t cc = { .local_sock = cp, .peer_sock = pp, .shm_capacity = 1u << 20 };
    setenv("ADAPTIPC_EAGER_SHM", "1", 1);
    adapt_ctx_t *p, *c;
    if (adapt_init(ADAPT_ROLE_PRODUCER, &pc, &p)) { fprintf(stderr, "init(p) failed\n"); return 3; }
    if (adapt_init(ADAPT_ROLE_CONSUMER, &cc, &c)) { fprintf(stderr, "init(c) failed\n"); return 3; }

    FILE *f = fopen(out, "w");
    if (!f) { perror(out); return 4; }
    fprintf(f, "# AdaptIPC per-decision log (real routing)\n");
    fprintf(f, "# fields: seq,payload,ewma,route,switched,shm_cost_us,uds_cost_us,queue_pct,reason\n");
    fflush(f);

    static unsigned char tx[70000], rx[70000];
    memset(tx, 0x5a, sizeof(tx));
    int prev_route = -1;
    unsigned long switches = 0;

    for (unsigned long i = 0; i < msgs; i++) {
        unsigned long ph = i * 3 / msgs;            /* 0,1,2 */
        size_t base = (ph == 1) ? 16384 : 512;
        size_t sz = base + (size_t)(rand() % 64);
        memcpy(tx, &i, sizeof(i));

        int rc;
        do { rc = adapt_send(p, tx, sz); } while (rc == -EAGAIN);
        if (rc) { fprintf(stderr, "send: %s\n", strerror(-rc)); return 5; }
        if (adapt_recv(c, rx, sizeof(rx)) != (int)sz) {
            fprintf(stderr, "recv failed\n"); return 6;
        }

        int route = (int)adapt_last_route(p);
        double ewma = adapt_ewma(p);
        int switched = prev_route >= 0 && route != prev_route;
        if (switched) switches++;
        size_t used = adapt_shm_used_bytes(p);
        size_t cap  = adapt_shm_capacity(p);
        double queue_pct = cap ? (100.0 * (double)used / (double)cap) : 0.0;
        double cs = est_shm_us(sz);
        double cu = est_uds_us(sz);
        const char *rname = adapt_route_name(route);
        const char *why = reason_text(switched, (int)sz, ewma);

        fprintf(f,
                "{\"seq\":%lu,\"payload\":%zu,\"ewma\":%.1f,"
                "\"route\":\"%s\",\"switched\":%d,"
                "\"shm_cost_us\":%.3f,\"uds_cost_us\":%.3f,"
                "\"queue_pct\":%.1f,\"reason\":\"%s\"}\n",
                i, sz, ewma, rname, switched, cs, cu, queue_pct, why);
        if ((i & 0x1f) == 0) fflush(f);
        prev_route = route;
    }
    fclose(f);
    fprintf(stderr, "decision_log: %lu msgs, %lu switches -> %s\n",
            msgs, switches, out);
    adapt_shutdown(p);
    adapt_shutdown(c);
    unlink(pp); unlink(cp);
    return 0;
}
