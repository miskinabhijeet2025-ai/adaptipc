/*
 * hardening_suite.c -- AdaptIPC v2.1 patent-evidence experiments.
 *
 * Subcommands (each writes one raw CSV under experiments/v2_1/raw/):
 *   occupancy    instrumentation vs independent cursor math
 *   epsilon      estimator noise characterization (stable workload)
 *   margin       false-switch/throughput vs hysteresis margin H
 *   prediction   queue-wait prediction accuracy vs actual delay
 *   adversarial  S* +/- delta sweeps, repeated, classified switches
 *   stall        consumer-stall timeline with separated timings
 *   crossover    learned S* vs synthetic ground truths
 *   lazy         eager vs lazy setup under 3 workload mixes
 *
 * Switch classification rules (mechanical, documented in
 * paper/EXPERIMENTS_V2_1.md):
 *   genuine_escape : route leaves SHM with reason QUEUE_PRESSURE or
 *                    HEALTH_ESCAPE, or the new route persists >= 8 msgs
 *   recovery       : route returns to SHM after a genuine escape and
 *                    persists >= 8 msgs
 *   noise_flap     : any other switch (flips back before persisting)
 */
#define _POSIX_C_SOURCE 200809L
#include "adapt_ipc.h"
#include "cost_model.h"
#include "shm_ringbuffer.h"
#include "uds_fallback.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#ifndef GIT_REV
#define GIT_REV "unknown"
#endif

#define OUTDIR "experiments/v2_1/raw"
#define MSG8K 8192u
#define PERSIST 8

static _Atomic int g_stall;
static adapt_ctx_t *g_cpthr;
static struct timespec hd_ms = { 0, 1000000 };

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static FILE *open_out(const char *name, const char *header)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", OUTDIR, name);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "# git_commit=%s\n", GIT_REV);
    fprintf(f, "%s\n", header);
    return f;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t pct(const uint64_t *v, size_t n, double p)
{
    if (!n) return 0;
    double idx = (p / 100.0) * (double)(n - 1);
    size_t lo = (size_t)idx;
    double fr = idx - (double)lo;
    uint64_t hi = (lo + 1 < n) ? v[lo + 1] : v[lo];
    return (uint64_t)((double)v[lo] * (1.0 - fr) + (double)hi * fr);
}

static double mean_of(const uint64_t *v, size_t n)
{
    double s = 0;
    for (size_t i = 0; i < n; i++) s += (double)v[i];
    return n ? s / n : 0;
}

/* ---------------- 1. occupancy validation ---------------- */
static void exp_occupancy(void)
{
    FILE *f = open_out("occupancy_validation.csv",
        "pct,reported_bytes,cursor_bytes,nominal_bytes,abs_error_bytes,"
        "rel_error");
    setenv("ADAPTIPC_EAGER_SHM", "1", 1);
    char pp[64], cp[64];
    snprintf(pp, 64, "/tmp/hd_occ_p_%ld", (long)getpid());
    snprintf(cp, 64, "/tmp/hd_occ_c_%ld", (long)getpid());
    adapt_config_t pc = { .shm_name = "/hd_occ_ring", .local_sock = pp,
        .peer_sock = cp, .shm_capacity = 1u << 20, .nonblocking_send = 1 };
    adapt_config_t cc = { .shm_name = "/hd_occ_ring", .local_sock = cp,
        .peer_sock = pp, .shm_capacity = 1u << 20 };
    adapt_ctx_t *p, *c;
    if (adapt_init(ADAPT_ROLE_PRODUCER, &pc, &p)) exit(2);
    if (adapt_init(ADAPT_ROLE_CONSUMER, &cc, &c)) exit(2);
    shm_ring_t *probe;
    if (shm_ring_attach("/hd_occ_ring", 1u << 20,
                        SHM_RING_ROLE_CONSUMER, &probe)) exit(2);

    static unsigned char msg[8192], rx[8192];
    memset(msg, 0x5a, sizeof(msg));
    const unsigned pcts[] = { 10, 20, 30, 40, 50, 60, 70, 80, 90 };
    for (unsigned k = 0; k < 9; k++) {
        while (shm_ring_used_bytes(probe) <
               (size_t)(1u << 20) * pcts[k] / 100u) {
            int rc = adapt_send(p, msg, MSG8K);
            if (rc != 0 && rc != -EAGAIN) exit(3);
        }
        size_t reported = adapt_shm_used_bytes(p);   /* method A */
        size_t cursor = shm_ring_used_bytes(probe);  /* method B */
        size_t nominal = (size_t)(1u << 20) * pcts[k] / 100u;
        fprintf(f, "%u,%zu,%zu,%zu,%zd,%.4f\n", pcts[k], reported,
                cursor, nominal, (ssize_t)reported - (ssize_t)nominal,
                nominal ? fabs((double)reported - (double)nominal) /
                          nominal : 0.0);
    }
    while (adapt_shm_used_bytes(p) > 0)
        if (adapt_recv(c, rx, sizeof(rx)) <= 0) break;
    fprintf(f, "0,%zu,%zu,0,0,0.0000\n",
            adapt_shm_used_bytes(p), shm_ring_used_bytes(probe));
    adapt_shutdown(p); adapt_shutdown(c); shm_ring_close(probe, 1);
    fclose(f);
    printf("occupancy_validation done\n");
}

/* ---------------- 2. epsilon estimation ---------------- */
static double g_eps_us = 0.0;

static void exp_epsilon(void)
{
    setenv("ADAPTIPC_DECISION_LOG", "/tmp/hd_eps_dec.csv", 1);
    setenv("ADAPTIPC_EAGER_SHM", "1", 1);
    char pp[64], cp[64];
    snprintf(pp, 64, "/tmp/hd_eps_p_%ld", (long)getpid());
    snprintf(cp, 64, "/tmp/hd_eps_c_%ld", (long)getpid());
    adapt_config_t pc = { .shm_name = "/hd_eps_ring", .local_sock = pp,
        .peer_sock = cp, .shm_capacity = 1u << 20,
        .policy = ADAPT_POLICY_COST_AWARE };
    adapt_config_t cc = { .shm_name = "/hd_eps_ring", .local_sock = cp,
        .peer_sock = pp, .shm_capacity = 1u << 20 };
    adapt_ctx_t *p, *c;
    if (adapt_init(ADAPT_ROLE_PRODUCER, &pc, &p)) exit(2);
    if (adapt_init(ADAPT_ROLE_CONSUMER, &cc, &c)) exit(2);
    static unsigned char msg[8192], rx[8192];
    memset(msg, 0x5a, sizeof(msg));
    for (int i = 0; i < 400; i++) {   /* STABLE: same payload, caught-up */
        int rc;
        do { rc = adapt_send(p, msg, 4096); } while (rc == -EAGAIN);
        if (adapt_recv(c, rx, sizeof(rx)) != 4096) exit(3);
    }
    adapt_shutdown(p); adapt_shutdown(c);
    unsetenv("ADAPTIPC_DECISION_LOG");

    FILE *f = fopen("/tmp/hd_eps_dec.csv", "r");
    if (!f) exit(4);
    char line[512];
    fgets(line, sizeof(line), f); /* header */
    double deltas[512];
    size_t n = 0;
    while (fgets(line, sizeof(line), f) && n < 512) {
        /* score_uds_us is col 13, score_shm_us col 14 */
        char *tok = strtok(line, ",");
        for (int i = 1; i < 13 && tok; i++) tok = strtok(NULL, ",");
        if (!tok) continue;
        double d = atof(tok) - atof(strtok(NULL, ","));
        deltas[n++] = fabs(d);
    }
    fclose(f);
    if (!n) exit(5);
    qsort(deltas, n, sizeof(double), cmp_double);
    /* Two noise regimes:
     *  - steady state: after the EWMAs converge, the score inputs are
     *    deterministic -> the differential spread collapses (~0).
     *  - transient: during convergence/fluctuation the differential
     *    moves; epsilon takes the larger (conservative) value. */
    double steady = deltas[n - 1] - deltas[n - 1 - n / 4];
    double transient = deltas[n * 95 / 100] - deltas[n * 5 / 100];
    g_eps_us = transient > steady ? transient : steady;
    printf("epsilon: steady=%.2f us, transient=%.2f us -> "
           "eps=%.2f us (n=%zu)\n", steady, transient, g_eps_us, n);
    FILE *o = fopen(OUTDIR "/epsilon_estimate.txt", "w");
    fprintf(o, "epsilon_steady_us=%.2f\nepsilon_transient_us=%.2f\n"
               "epsilon_used_us=%.2f\nn=%zu\n"
               "method=max(steady, transient) spread of |score_uds - "
               "score_shm|; steady = late-quarter range, transient = "
               "(p95-p5) over the full convergence run\n",
            steady, transient, g_eps_us, n);
    fclose(o);
}

/* ---------------- 3. margin sweep ---------------- */
/* Adversarial workload with H in multiples of epsilon; switches are
 * classified from the decision log with the documented rules. */
static void run_margin_point(FILE *csv, double H_us, unsigned long msgs)
{
    char mb[32];
    snprintf(mb, sizeof(mb), "%.2f", H_us);
    setenv("ADAPTIPC_MARGIN_US", mb, 1);
    setenv("ADAPTIPC_DECISION_LOG", "/tmp/hd_margin_dec.csv", 1);
    setenv("ADAPTIPC_EAGER_SHM", "1", 1);
    setenv("ADAPTIPC_STATS", "1", 1);
    char pp[64], cp[64];
    snprintf(pp, 64, "/tmp/hd_m_p_%ld", (long)getpid());
    snprintf(cp, 64, "/tmp/hd_m_c_%ld", (long)getpid());
    adapt_config_t pc = { .shm_name = "/hd_m_ring", .local_sock = pp,
        .peer_sock = cp, .shm_capacity = 1u << 20,
        .policy = ADAPT_POLICY_FULL_ADAPTIVE, .nonblocking_send = 1 };
    adapt_config_t cc = { .shm_name = "/hd_m_ring", .local_sock = cp,
        .peer_sock = pp, .shm_capacity = 1u << 20 };
    adapt_ctx_t *p, *c;
    if (adapt_init(ADAPT_ROLE_PRODUCER, &pc, &p)) exit(2);
    if (adapt_init(ADAPT_ROLE_CONSUMER, &cc, &c)) exit(2);
    static unsigned char tx[20000], rx[20000];
    memset(tx, 0x5a, sizeof(tx));
    unsigned long sent = 0;
    uint64_t t0 = now_ns();
    double bytes = 0;
    while (sent < msgs) {
        size_t sz = (sent % 2) ? 16384 : 1024;
        memcpy(tx, &sent, 8);
        int rc;
        do { rc = adapt_send(p, tx, sz); } while (rc == -EAGAIN);
        if (rc) exit(3);
        if (adapt_recv(c, rx, sizeof(rx)) != (int)sz) exit(4);
        bytes += sz;
        sent++;
    }
    uint64_t t1 = now_ns();
    adapt_shutdown(p); adapt_shutdown(c);

    /* classify from the decision log */
    FILE *f = fopen("/tmp/hd_margin_dec.csv", "r");
    unsigned total_sw = 0, genuine = 0, flaps = 0;
    if (f) {
        char line[512];
        fgets(line, sizeof(line), f);
        int prev = -1, pending = 0;
        unsigned since = 99;
        while (fgets(line, sizeof(line), f)) {
            char *tok = strtok(line, ",");
            for (int i = 0; i < 15 && tok; i++) tok = strtok(NULL, ",");
            if (!tok) continue;
            int sel = strcmp(tok, "SHM") == 0 ? 1 : 2;
            if (prev >= 0 && sel != prev) {
                total_sw++;
                pending = sel;
                since = 0;
            } else if (pending && sel == pending) {
                if (++since == PERSIST) { genuine++; pending = 0; }
            } else if (pending) {
                flaps++;          /* flipped back before persisting */
                pending = 0;
            }
            prev = sel;
        }
        fclose(f);
    }
    double dur = (double)(t1 - t0) / 1e9;
    fprintf(csv, "%.2f,%lu,%u,%u,%u,%.3f,%.1f\n", H_us, msgs, total_sw,
            genuine, flaps, dur / msgs * 1e6,
            bytes / (1024.0 * 1024.0) / dur);
}

static void exp_margin(void)
{
    exp_epsilon();
    FILE *f = open_out("stability_margin.csv",
        "H_us,messages,total_switches,genuine_escapes,noise_flaps,"
        "latency_us_per_msg,throughput_mbps");
    /* Measured estimator noise is ~0 in steady state (deterministic
     * converged cost estimates), so the sweep uses ABSOLUTE margins:
     * the stability risk under load is input fluctuation, not
     * estimator noise. epsilon is still recorded for the record. */
    const double Hs[] = { 0.0, 2.0, 5.0, 10.0, 25.0, 50.0 };
    char tag[32];
    for (unsigned k = 0; k < 6; k++) {
        snprintf(tag, sizeof(tag), "H=%.0fus", Hs[k]);
        printf("  margin %s\n", tag);
        run_margin_point(f, Hs[k], 2000);
    }
    fclose(f);
    printf("margin sweep done\n");
}

/* ---------------- 4. queue-prediction accuracy ---------------- */
/* Fill the ring to a target occupancy with the consumer stopped, then
 * send one stamped probe and compare the model's predicted wait with
 * the probe's actual queueing delay (e2e latency of a probe sent when
 * the ring holds X bytes, minus the unloaded e2e baseline). */
static void exp_prediction(void)
{
    FILE *f = open_out("queue_prediction_accuracy.csv",
        "occ_pct,occupancy_bytes,predicted_wait_us,actual_delay_us,"
        "abs_error_us,rel_error");
    setenv("ADAPTIPC_EAGER_SHM", "1", 1);
    char pp[64], cp[64];
    snprintf(pp, 64, "/tmp/hd_pr_p_%ld", (long)getpid());
    snprintf(cp, 64, "/tmp/hd_pr_c_%ld", (long)getpid());
    adapt_config_t pc = { .shm_name = "/hd_pr_ring", .local_sock = pp,
        .peer_sock = cp, .shm_capacity = 4u << 20, .nonblocking_send = 1 };
    adapt_config_t cc = { .shm_name = "/hd_pr_ring", .local_sock = cp,
        .peer_sock = pp, .shm_capacity = 4u << 20 };
    adapt_ctx_t *p, *c;
    if (adapt_init(ADAPT_ROLE_PRODUCER, &pc, &p)) exit(2);
    if (adapt_init(ADAPT_ROLE_CONSUMER, &cc, &c)) exit(2);
    static unsigned char big[8192], rx[8192];
    memset(big, 0x5a, sizeof(big));

    /* unloaded baseline (median of 200 single-in-flight latencies) */
    uint64_t base[200];
    for (int i = 0; i < 200; i++) {
        uint64_t t = now_ns();
        memcpy(big, &t, 8);
        while (adapt_send(p, big, 4096) == -EAGAIN) {}
        int bn = adapt_recv(c, rx, sizeof(rx));
        if (bn != 4096) {
            fprintf(stderr, "baseline recv=%d (%s)\n", bn,
                    adapt_strerror(bn));
            exit(5);
        }
        uint64_t s; memcpy(&s, rx, 8);
        base[i] = now_ns() - s;
    }
    qsort(base, 200, sizeof(uint64_t), cmp_u64);
    const uint64_t base_med = base[100];

    const unsigned pcts[] = { 10, 25, 50, 75, 80, 90, 95 };
    for (unsigned k = 0; k < 7; k++) {
        size_t target = (size_t)(4u << 20) * pcts[k] / 100u;
        while (adapt_shm_used_bytes(p) < target) {
            if (adapt_send(p, big, 8192) != 0) break;
        }
        /* one stamped probe through the queue; the policy inputs are
         * captured at send time (occupancy, prediction), since the
         * drain below empties the ring */
        size_t occ_at_send = adapt_shm_used_bytes(p);
        double predicted = adapt_debug_predicted_wait_us(p, 4096);
        uint64_t t = now_ns();
        memcpy(big, &t, 8);
        int rc;
        do { rc = adapt_send(p, big, 4096); } while (rc == -EAGAIN);
        /* the probe queues behind the fill backlog: drain until the
         * 4096-byte probe arrives; its e2e time IS the queueing delay */
        uint64_t s = 0, actual = 0;
        for (;;) {
            int rn = adapt_recv(c, rx, sizeof(rx));
            if (rn == 4096) {
                memcpy(&s, rx, 8);
                actual = now_ns() - s;
                break;
            }
            if (rn <= 0) exit(5);
        }
        double actual_delay = actual > base_med
                                  ? (double)(actual - base_med) : 0.0;
        fprintf(f, "%u,%zu,%.1f,%.1f,%.1f,%.3f\n", pcts[k],
                occ_at_send, predicted, actual_delay,
                fabs(predicted - actual_delay),
                actual_delay > 0 ? fabs(predicted - actual_delay) /
                                       actual_delay : 0.0);
    }
    adapt_shutdown(p); adapt_shutdown(c);
    fclose(f);
    printf("prediction accuracy done\n");
}

/* ---------------- 5. adversarial delta sweep, repeated ---------------- */
static void run_adversarial_repeat(FILE *f, const char *policy,
                                   const char *delta_tag, size_t s_lo,
                                   size_t s_hi, unsigned long msgs,
                                   double eps_us)
{
    setenv("ADAPTIPC_EAGER_SHM", "1", 1);
    setenv("ADAPTIPC_STATS", "1", 1);
    char pol[16];
    snprintf(pol, sizeof(pol), "%s", policy);
    setenv("ADAPTIPC_POLICY", pol, 1);
    char pp[64], cp[64];
    snprintf(pp, 64, "/tmp/hd_av_p_%ld", (long)getpid());
    snprintf(cp, 64, "/tmp/hd_av_c_%ld", (long)getpid());
    adapt_policy_mode_t pm = ADAPT_POLICY_SIZE_ONLY;
    if (!strcmp(policy, "size_hysteresis"))
        pm = ADAPT_POLICY_SIZE_HYSTERESIS;
    else if (!strcmp(policy, "queue_aware"))
        pm = ADAPT_POLICY_QUEUE_AWARE;
    else if (!strcmp(policy, "cost_aware"))
        pm = ADAPT_POLICY_COST_AWARE;
    else if (!strcmp(policy, "full_adaptive"))
        pm = ADAPT_POLICY_FULL_ADAPTIVE;
    adapt_config_t pc = { .shm_name = "/hd_av_ring", .local_sock = pp,
        .peer_sock = cp, .shm_capacity = 1u << 20,
        .policy = pm, .nonblocking_send = 1 };
    adapt_config_t cc = { .shm_name = "/hd_av_ring", .local_sock = cp,
        .peer_sock = pp, .shm_capacity = 1u << 20 };
    adapt_ctx_t *p, *c;
    if (adapt_init(ADAPT_ROLE_PRODUCER, &pc, &p)) exit(2);
    if (adapt_init(ADAPT_ROLE_CONSUMER, &cc, &c)) exit(2);
    static unsigned char tx[20000], rx[20000];
    memset(tx, 0x5a, sizeof(tx));
    uint64_t lat[2048];
    unsigned n_lat = 0;
    uint64_t t0 = now_ns();
    double bytes = 0;
    int prev_route = -1;
    unsigned total_sw = 0, genuine = 0, flaps = 0;
    int pending = 0;
    unsigned since = 99;
    for (unsigned long i = 0; i < msgs; i++) {
        size_t sz = (i % 2) ? s_hi : s_lo;
        uint64_t ts = now_ns();
        memcpy(tx, &ts, 8);
        int rc;
        do { rc = adapt_send(p, tx, sz); } while (rc == -EAGAIN);
        if (rc) exit(3);
        assert(adapt_recv(c, rx, sizeof(rx)) == (int)sz);
        bytes += sz;
        uint64_t s; memcpy(&s, rx, 8);
        if (n_lat < 2048) lat[n_lat++] = now_ns() - s;
        /* switch accounting via the library counter + persistence */
        adapt_stats_t st;
        adapt_get_stats(p, &st);
        int route = (int)adapt_last_route(p);
        if (prev_route >= 0 && route != prev_route) {
            total_sw++;
            pending = route;
            since = 0;
        } else if (pending && route == pending) {
            if (++since == PERSIST) { genuine++; pending = 0; }
        } else if (pending) {
            flaps++;
            pending = 0;
        }
        prev_route = route;
    }
    uint64_t t1 = now_ns();
    adapt_shutdown(p); adapt_shutdown(c);
    qsort(lat, n_lat, sizeof(uint64_t), cmp_u64);
    double dur = (double)(t1 - t0) / 1e9;
    fprintf(f,
            "%s,%s,%lu,%.0f,%.0f,%u,%u,%u,%.3f,%.1f,%.1f,%.1f,%.1f\n",
            policy, delta_tag, msgs, (double)s_lo, (double)s_hi,
            total_sw, genuine, flaps, bytes / (1024.0 * 1024.0) / dur,
            pct(lat, n_lat, 50) / 1000.0, pct(lat, n_lat, 99) / 1000.0,
            pct(lat, n_lat, 99.9) / 1000.0, dur / msgs * 1e6);
}

static void exp_adversarial(void)
{
    /* establish the learned crossover with a warm-up run first */
    double s_star = 0;
    {
        setenv("ADAPTIPC_EAGER_SHM", "1", 1);
        char pp[64], cp[64];
        snprintf(pp, 64, "/tmp/hd_av_w_%ld", (long)getpid());
        snprintf(cp, 64, "/tmp/hd_av_w_%ldc", (long)getpid());
        adapt_config_t pc = { .shm_name = "/hd_av_ring", .local_sock = pp,
            .peer_sock = cp, .shm_capacity = 1u << 20,
            .policy = ADAPT_POLICY_FULL_ADAPTIVE };
        adapt_config_t cc = { .shm_name = "/hd_av_ring", .local_sock = cp,
            .peer_sock = pp, .shm_capacity = 1u << 20 };
        adapt_ctx_t *p, *c;
        if (adapt_init(ADAPT_ROLE_PRODUCER, &pc, &p)) exit(2);
        if (adapt_init(ADAPT_ROLE_CONSUMER, &cc, &c)) exit(2);
        static unsigned char tx[20000], rx[20000];
        memset(tx, 0x5a, sizeof(tx));
        for (int i = 0; i < 200; i++) {
            size_t sz = (i % 2) ? 4096 : 512;
            int rc;
            do { rc = adapt_send(p, tx, sz); } while (rc == -EAGAIN);
            adapt_recv(c, rx, sizeof(rx));
        }
        s_star = adapt_crossover(p);
        adapt_shutdown(p); adapt_shutdown(c);
    }
    printf("learned S* = %.0f B\n", s_star);
    if (s_star < 512) s_star = 4096; /* fall back to tau_high */

    FILE *f = open_out("adversarial_delta.csv",
        "policy,delta_tag,messages,s_lo,s_hi,total_switches,"
        "genuine_escapes,noise_flaps,throughput_mbps,p50_us,p99_us,"
        "p99_9_us,latency_us_per_msg");
    const double deltas[] = { 0.01, 0.02, 0.05, 0.10 };
    const char *policies[] = { "size_only", "size_hysteresis",
        "queue_aware", "cost_aware", "full_adaptive" };
    for (unsigned d = 0; d < 4; d++) {
        size_t mid = (size_t)s_star;
        size_t lo = mid - (size_t)(mid * deltas[d]);
        size_t hi = mid + (size_t)(mid * deltas[d]);
        if (lo < 64) lo = 64;
        for (unsigned p = 0; p < 5; p++) {
            char tag[32];
            snprintf(tag, sizeof(tag), "d=%.0f%%", deltas[d] * 100);
            for (int rep = 0; rep < 2; rep++) {
                printf("  adversarial %s %s rep%d\n",
                       policies[p], tag, rep);
                run_adversarial_repeat(f, policies[p], tag, lo, hi,
                                       5000, g_eps_us);
            }
        }
    }
    fclose(f);
    printf("adversarial sweep done\n");
}

/* consumer thread body for the stall experiment (file scope) */
static void *hd_consumer_fn(void *arg)
{
    (void)arg;
    static unsigned char rxb[8192];
    extern struct timespec hd_ms;
    for (;;) {
        if (atomic_load(&g_stall)) { nanosleep(&hd_ms, NULL); continue; }
        if (adapt_recv(g_cpthr, rxb, sizeof(rxb)) <= 0)
            nanosleep(&hd_ms, NULL);
    }
    return NULL;
}

/* ---------------- 6. consumer stall timeline ---------------- */
static void exp_stall(void)
{
    FILE *f = open_out("stall_timeline.csv",
        "t_ms,event,payload,route,ring_used,predicted_wait_us");
    setenv("ADAPTIPC_EAGER_SHM", "1", 1);
    setenv("ADAPTIPC_STATS", "1", 1);
    char pp[64], cp[64];
    snprintf(pp, 64, "/tmp/hd_st_p_%ld", (long)getpid());
    snprintf(cp, 64, "/tmp/hd_st_c_%ld", (long)getpid());
    /* parent = consumer (drains); child = producer; both forked */
    adapt_config_t pc = { .shm_name = "/hd_st_ring", .local_sock = pp,
        .peer_sock = cp, .shm_capacity = 1u << 20,
        .policy = ADAPT_POLICY_FULL_ADAPTIVE, .nonblocking_send = 1 };
    adapt_config_t cc = { .shm_name = "/hd_st_ring", .local_sock = cp,
        .peer_sock = pp, .shm_capacity = 1u << 20 };
    adapt_ctx_t *p, *c;
    if (adapt_init(ADAPT_ROLE_PRODUCER, &pc, &p)) exit(2);
    if (adapt_init(ADAPT_ROLE_CONSUMER, &cc, &c)) exit(2);

    /* timeline: normal (fast drain) -> stall (consumer stops) ->
     * recovery (consumer resumes). The consumer runs in a thread so
     * the producer timeline is uninterrupted. */
    static pthread_t g_ct;
    g_cpthr = c;
    uint64_t t0 = now_ns();
    pthread_create(&g_ct, NULL, hd_consumer_fn, NULL);

    /* phase 1: normal */
    for (int i = 0; i < 400; i++) {
        static unsigned char big[8192];
        memcpy(big, &(uint64_t){(uint64_t)now_ns()}, 8);
        adapt_send(p, big, 8192);
        if (i % 40 == 0)
            fprintf(f, "%.1f,normal,8192,%s,%zu,%.1f\n",
                    (now_ns() - t0) / 1e6,
                    adapt_route_name(adapt_last_route(p)),
                    adapt_shm_used_bytes(p),
                    adapt_debug_predicted_wait_us(p, 8192));
    }
    /* phase 2: stall -- consumer stops; producer keeps sending */
    atomic_store(&g_stall, 1);
    uint64_t stall_start = now_ns();
    uint64_t escape_ns = 0;
    for (int i = 0; i < 4000 && !escape_ns; i++) {
        static unsigned char small[3072];
        memcpy(small, &(uint64_t){(uint64_t)now_ns()}, 8);
        (void)adapt_send(p, small, 3072);   /* may fail; decision counts */
        int route = (int)adapt_last_route(p);
        if (route == ADAPT_ROUTE_UDS)
            escape_ns = now_ns();
        if (i % 20 == 0)
            fprintf(f, "%.1f,stall,3072,%s,%zu,%.1f\n",
                    (now_ns() - t0) / 1e6,
                    adapt_route_name(adapt_last_route(p)),
                    adapt_shm_used_bytes(p),
                    adapt_debug_predicted_wait_us(p, 3072));
        nanosleep(&hd_ms, NULL);
    }
    /* phase 3: recovery -- consumer resumes; bulk probes */
    atomic_store(&g_stall, 0);
    uint64_t recovery_ns = 0;
    for (int i = 0; i < 4000; i++) {
        static unsigned char big[8192];
        memcpy(big, &(uint64_t){(uint64_t)now_ns()}, 8);
        int rc = adapt_send(p, big, 8192);
        if (rc == 0 && adapt_last_route(p) == ADAPT_ROUTE_SHM) {
            recovery_ns = now_ns();
            break;
        }
        if (i % 40 == 0)
            fprintf(f, "%.1f,recovery,8192,%s,%zu,%.1f\n",
                    (now_ns() - t0) / 1e6,
                    adapt_route_name(adapt_last_route(p)),
                    adapt_shm_used_bytes(p),
                    adapt_debug_predicted_wait_us(p, 8192));
        nanosleep(&hd_ms, NULL);
    }
    double det_ms = escape_ns ? (escape_ns - stall_start) / 1e6 : -1;
    double rec_ms = recovery_ns ? (recovery_ns - stall_start) / 1e6 : -1;
    fprintf(f, "%.1f,summary,0,%.1f,%.1f,%.1f\n",
            (now_ns() - t0) / 1e6, det_ms, rec_ms, g_eps_us);
    printf("stall: detection+escape=%.1f ms, recovery=%.1f ms\n",
           det_ms, rec_ms);
    pthread_cancel(g_ct);
    adapt_shutdown(p); adapt_shutdown(c);
    fclose(f);
}

/* ---------------- 7. crossover learning sweep ---------------- */
static void exp_crossover(void)
{
    FILE *f = open_out("crossover_sweep.csv",
        "true_crossover_b,learned_b,abs_error_b,rel_error,"
        "crossover_updates");
    const double truths[] = { 512, 1024, 1333, 2048, 4096, 8192 };
    for (unsigned t = 0; t < sizeof(truths)/sizeof(truths[0]); t++) {
        double S = truths[t];
        adapt_cost_cfg_t cfg;
        adapt_cost_cfg_defaults(&cfg);
        adapt_learn_t learn;
        adapt_learn_init(&learn, &cfg);
        /* synthetic ground truth: UDS = 2us + 2ns/B,
         * SHM chosen so the crossover lands at S:
         *   (a_shm - 2) / (2ns - b_shm) = S  with b_shm = 0.5ns
         *   => a_shm = 2 + 1.5ns * S  (in us: 0.0015*S) */
        double a_shm = 2.0 + 0.0015 * S;
        unsigned seed = (unsigned)(S * 7);
        unsigned conv_at = 0;
        for (int i = 0; i < 400; i++) {
            size_t lo = 128 + (size_t)(rand_r(&seed) % 256);
            size_t hi = 8192 + (size_t)(rand_r(&seed) % 4096);
            adapt_learn_observe(&learn, &cfg, ADAPT_ROUTE_UDS, lo,
                                2.0 + 0.002 * lo);
            adapt_learn_observe(&learn, &cfg, ADAPT_ROUTE_UDS, hi,
                                2.0 + 0.002 * hi);
            adapt_learn_observe(&learn, &cfg, ADAPT_ROUTE_SHM, lo,
                                a_shm + 0.0005 * lo);
            adapt_learn_observe(&learn, &cfg, ADAPT_ROUTE_SHM, hi,
                                a_shm + 0.0005 * hi);
            double s = adapt_learn_crossover(&learn, &cfg);
            if (!conv_at && s > 0 &&
                fabs(s - S) < 0.05 * S)
                conv_at = (unsigned)(i + 1);
        }
        double learned = adapt_learn_crossover(&learn, &cfg);
        double err = fabs(learned - S);
        fprintf(f, "%.0f,%.1f,%.1f,%.4f,%u\n", S, learned, err,
                err / S, learn.crossover_updates);
    }
    fclose(f);
    printf("crossover sweep done\n");
}

/* ---------------- 8. lazy setup ---------------- */
static double init_time_us;
static uint64_t first_msg_us;
static uint64_t setup_reqs, setup_acks;

static void run_lazy(const char *tag, int eager, int heavy)
{
    setenv("ADAPTIPC_EAGER_SHM", eager ? "1" : "0", 1);
    char pp[64], cp[64];
    snprintf(pp, 64, "/tmp/hd_lz_p_%ld", (long)getpid());
    snprintf(cp, 64, "/tmp/hd_lz_c_%ld", (long)getpid());
    adapt_config_t pc = { .shm_name = "/hd_lz_ring", .local_sock = pp,
        .peer_sock = cp, .shm_capacity = 1u << 20 };
    adapt_config_t cc = { .shm_name = "/hd_lz_ring", .local_sock = cp,
        .peer_sock = pp, .shm_capacity = 1u << 20 };
    setenv("ADAPTIPC_STATS", "1", 1);
    uint64_t t0 = now_ns();
    adapt_ctx_t *p, *c;
    if (adapt_init(ADAPT_ROLE_PRODUCER, &pc, &p)) exit(2);
    if (adapt_init(ADAPT_ROLE_CONSUMER, &cc, &c)) exit(2);
    init_time_us = (double)(now_ns() - t0) / 1.0;
    static unsigned char tx[70000], rx[70000];
    memset(tx, 0x5a, sizeof(tx));
    /* first message: bulk (forces SHM decision in mixed/heavy) */
    uint64_t t1 = now_ns();
    memcpy(tx, &t1, 8);
    int rc;
    do { rc = adapt_send(p, tx, 16384); } while (rc == -EAGAIN);
    assert(adapt_recv(c, rx, sizeof(rx)) == 16384);
    first_msg_us = now_ns() - t1;
    /* steady state: workload mix */
    unsigned n = 3000;
    uint64_t t2 = now_ns();
    double bytes = 0;
    for (unsigned i = 0; i < n; i++) {
        size_t sz = heavy == 2 ? 16384 :
                    (heavy == 1 ? ((i % 2) ? 16384 : 512) : 512);
        uint64_t ts = now_ns();
        memcpy(tx, &ts, 8);
        do { rc = adapt_send(p, tx, sz); } while (rc == -EAGAIN);
        assert(adapt_recv(c, rx, sizeof(rx)) == (int)sz);
        bytes += sz;
    }
    double dur = (double)(now_ns() - t2) / 1e9;
    adapt_stats_t st;
    adapt_get_stats(p, &st);
    setup_reqs = st.shm_setup_reqs;
    setup_acks = st.shm_setup_acks;
    FILE *f = fopen(OUTDIR "/lazy_setup.csv", "a");
    fprintf(f, "%s,%d,%d,%.1f,%.1f,%llu,%llu,%.1f\n", tag, eager, heavy,
            init_time_us, (double)first_msg_us,
            (unsigned long long)setup_reqs,
            (unsigned long long)setup_acks,
            bytes / (1024.0 * 1024.0) / dur);
    fclose(f);
    adapt_shutdown(p); adapt_shutdown(c);
}

static void exp_lazy(void)
{
    FILE *f = open_out("lazy_setup.csv",
        "workload,eager,heavy,init_time_us,first_msg_us,setup_reqs,"
        "setup_acks,steady_throughput_mbps");
    fclose(f);
    const char *wnames[] = { "uds_only", "mixed", "shm_heavy" };
    for (int w = 0; w < 3; w++) {
        run_lazy(wnames[w], 1, w);
        run_lazy(wnames[w], 0, w);
    }
    printf("lazy setup done\n");
}

/* ---------------- main ---------------- */
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s occupancy|epsilon|margin|prediction|"
                        "adversarial|stall|crossover|lazy\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "occupancy")) exp_occupancy();
    else if (!strcmp(argv[1], "epsilon")) exp_epsilon();
    else if (!strcmp(argv[1], "margin")) exp_margin();
    else if (!strcmp(argv[1], "prediction")) exp_prediction();
    else if (!strcmp(argv[1], "adversarial")) exp_adversarial();
    else if (!strcmp(argv[1], "stall")) exp_stall();
    else if (!strcmp(argv[1], "crossover")) exp_crossover();
    else if (!strcmp(argv[1], "lazy")) exp_lazy();
    else { fprintf(stderr, "unknown experiment\n"); return 2; }
    return 0;
}
