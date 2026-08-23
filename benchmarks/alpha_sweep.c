/*
 * alpha_sweep.c -- EWMA smoothing-factor sensitivity experiment.
 *
 * For each alpha in {0.05, 0.1, 0.2, 0.3, 0.5, 0.8} (set via the
 * ADAPTIPC_ALPHA environment variable), runs three single-process
 * scenarios over a live AdaptIPC producer/consumer pair:
 *
 *   A. stability     : 20k in-deadband messages alternating across
 *                      1024..4096 B (thrash pattern). Metric: EWMA route
 *                      switches. Theory (Section III-D): zero for every
 *                      alpha -- inputs never leave the open interval.
 *   B. responsiveness: settle on a 128 B control stream (route = UDS),
 *                      then switch permanently to 5000 B payloads; count
 *                      messages until the EWMA crosses tau_high. Compared
 *                      against the Section III-D flip-time bound.
 *   C. spike immunity: 128 B control stream with one isolated 64 KB
 *                      message every 50 messages (out-of-band spikes).
 *                      Metric: EWMA route switches caused by spikes alone.
 *
 * Results written to benchmarks/alpha_sensitivity.txt.
 */
#define _POSIX_C_SOURCE 200809L

#include "adapt_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

static const double alphas[] = { 0.05, 0.1, 0.2, 0.3, 0.5, 0.8 };
#define N_ALPHAS ((int)(sizeof(alphas) / sizeof(alphas[0])))

static void fresh_transports(adapt_ctx_t **p, adapt_ctx_t **c)
{
    shm_unlink("/asw_shm");
    unlink("/tmp/asw_a.sock");
    unlink("/tmp/asw_b.sock");
    adapt_config_t pcfg = { .shm_name = "/asw_shm",
                            .local_sock = "/tmp/asw_a.sock",
                            .peer_sock = "/tmp/asw_b.sock",
                            .shm_capacity = 256u << 20 };
    adapt_config_t ccfg = pcfg;
    ccfg.local_sock = "/tmp/asw_b.sock";
    ccfg.peer_sock = "/tmp/asw_a.sock";
    if (adapt_init(ADAPT_ROLE_PRODUCER, &pcfg, p) != 0) *p = NULL;
    if (*p && adapt_init(ADAPT_ROLE_CONSUMER, &ccfg, c) != 0) *c = NULL;
}

static int send_retry(adapt_ctx_t *p, const void *d, size_t sz)
{
    int rc;
    do { rc = adapt_send(p, d, sz); } while (rc == -EAGAIN);
    return rc;
}

static void drain_recv(adapt_ctx_t *c, unsigned char *buf, size_t cap,
                       int *rc)
{
    int n;
    do { n = adapt_recv(c, buf, cap); } while (n == -EAGAIN);
    *rc = n;
}

int main(void)
{
    FILE *out = fopen("benchmarks/alpha_sensitivity.txt", "w");
    if (!out) { perror("fopen"); return 1; }
    fprintf(out, "# EWMA alpha sensitivity sweep (post retry-fix code)\n");

    static unsigned char ctl[128], reg[5000], spike[65536];
    memset(reg, 3, sizeof(reg));
    memset(spike, 5, sizeof(spike));
    static unsigned char big[262144];

    for (int ai = 0; ai < N_ALPHAS; ++ai) {
        char astr[16];
        snprintf(astr, sizeof(astr), "%g", alphas[ai]);
        setenv("ADAPTIPC_ALPHA", astr, 1);

        adapt_ctx_t *p = NULL, *c = NULL;
        fresh_transports(&p, &c);
        if (!p || !c) {
            fprintf(out, "alpha=%s: init FAILED\n", astr);
            adapt_shutdown(p);
            adapt_shutdown(c);
            continue;
        }

        /* ---- Scenario A: in-band stability -------------------------- */
        static const size_t pat[] = { 1024, 4096, 1024, 2048,
                                      4096, 1536, 3072, 4096 };
        unsigned long switches = 0;
        int prev = -1, rc = 0;
        for (unsigned long i = 0; i < 20000; ++i) {
            rc = send_retry(p, big, pat[i % 8]);
            if (rc != 0) break;
            drain_recv(c, big, sizeof(big), &rc);
            if (rc <= 0) break;
            int r = (int)adapt_last_route(p);
            if (prev != -1 && r != prev) switches++;
            prev = r;
        }
        fprintf(out, "alpha=%-4s scenario=A(stability,20k in-band msgs):"
                     " switches=%lu\n", astr, switches);


        /* ---- Scenario B: responsiveness to genuine regime change ---- */
        adapt_shutdown(p);
        adapt_shutdown(c);
        fresh_transports(&p, &c);
        if (!p || !c) { fprintf(out, "alpha=%s: reinit FAILED\n", astr);
                        continue; }
        for (int i = 0; i < 300; ++i) { /* settle on control stream */
            rc = send_retry(p, ctl, sizeof(ctl));
            drain_recv(c, big, sizeof(big), &rc);
        }
        int settled_uds = (adapt_last_route(p) == ADAPT_ROUTE_UDS);
        unsigned long msgs_to_flip = 0;
        int flipped = 0;
        for (unsigned long i = 0; i < 500 && !flipped; ++i) {
            rc = send_retry(p, reg, sizeof(reg));
            drain_recv(c, big, sizeof(big), &rc);
            if (adapt_last_route(p) == ADAPT_ROUTE_SHM) {
                msgs_to_flip = i + 1;
                flipped = 1;
            }
        }
        /* predicted flip point from Section III-D bound, with the EWMA
         * settled at e=128 on the control stream before the change:
         * n > ln((5000-128)/(5000-4096)) / ln(1/(1-alpha)) */
        double n_pred_d =
            __builtin_log((5000.0 - 128.0) / (5000.0 - 4096.0)) /
            __builtin_log(1.0 / (1.0 - alphas[ai]));
        fprintf(out,
                "alpha=%-4s scenario=B(responsiveness,control->5000B):"
                " settled_uds=%d msgs_to_SHM_flip=%lu predicted_n>%.1f"
                " (observed %s prediction)\n",
                astr, settled_uds, msgs_to_flip, n_pred_d,
                flipped ? "matches" : "DID NOT match");

        /* ---- Scenario C: isolated-spike immunity -------------------- */
        adapt_shutdown(p);
        adapt_shutdown(c);
        fresh_transports(&p, &c);
        if (!p || !c) { fprintf(out, "alpha=%s: reinit2 FAILED\n", astr);
                        continue; }
        switches = 0;
        prev = -1;
        unsigned long bursts = 0;
        for (unsigned long i = 0; i < 5000; ++i) {
            const unsigned char *msg = ctl;
            size_t msz = sizeof(ctl);
            if (i % 50 == 25) { /* isolated 64KB spike every 50 messages */
                msg = spike;
                msz = sizeof(spike);
                bursts++;
            }
            rc = send_retry(p, msg, msz);
            drain_recv(c, big, sizeof(big), &rc);
            int r = (int)adapt_last_route(p);
            if (prev != -1 && r != prev) switches++;
            prev = r;
        }
        fprintf(out,
                "alpha=%-4s scenario=C(spike immunity,100 spikes/5k msgs):"
                " switches=%lu (bursts=%lu)\n", astr, switches, bursts);

        adapt_shutdown(p);
        adapt_shutdown(c);
    }

    fclose(out);
    printf("wrote benchmarks/alpha_sensitivity.txt\n");
    return 0;
}

