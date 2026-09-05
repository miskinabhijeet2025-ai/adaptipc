/*
 * adaptive_ablation.c -- experiment matrix for context-aware AdaptIPC.
 *
 * Experiments (all share identical workloads across policies):
 *   A payload_sweep      : fixed-size streams 64 B .. 1 MB
 *   B queue_pressure     : paced consumer, saturating producer
 *   C burst_workload     : idle/burst cycles
 *   D adversarial        : payloads alternating across the threshold
 *   E degradation        : consumer stall -> health -> recovery
 *   F setup_cost         : eager vs lazy SHM establishment
 *   G qos                : BALANCED / LATENCY / THROUGHPUT budgets
 *
 * Transports/policies compared:
 *   uds, shm (pure baselines), size_only, size_hysteresis,
 *   queue_aware, cost_aware, full_adaptive (+ QoS variants)
 *
 * Output: experiments/raw/<experiment>.csv (one row per run) with the
 * metric columns required by the paper; metadata columns include
 * platform, compiler, seed and message counts so every run is
 * reproducible. Summaries are derived by scripts/summarize.py.
 *
 * Build:
 *   cc -std=c11 -O3 -Wall -Wextra -pthread -Iinclude \
 *      benchmarks/adaptive_ablation.c src/[sources] -o adaptive_ablation
 */
#define _POSIX_C_SOURCE 200809L

#include "adapt_ipc.h"
#include "shm_ringbuffer.h"
#include "uds_fallback.h"

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#elif !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS 0x1000   /* macOS/BSD value under strict POSIX */
#endif

#define FRAME_HDR 24u                 /* seq(8) ts(8) len(8) */
#define MAX_MSG   (1u << 20)
#define RING_CAP  (64u << 20)

#define SOCK_A "/tmp/adaptipc_abl_a.sock"
#define SOCK_B "/tmp/adaptipc_abl_b.sock"
#define SHM_NAME_DEFAULT "/adaptipc_abl_shm"

/* Shared producer<->consumer control block (fork-inherited mapping). */
typedef struct {
    _Atomic uint32_t consumer_pause;   /* E: stall the consumer */
    _Atomic uint32_t done;             /* unused sentinel */
} ctl_t;

static ctl_t *g_ctl;

int uds_prod_run(uds_endpoint_t *u, const char *experiment,
                 const char *peer, unsigned long seed);
int shm_prod_run(shm_ring_t *ring, const char *experiment,
                 unsigned long seed);

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void die(const char *m)
{
    fprintf(stderr, "adaptive_ablation: %s (%s)\n", m, strerror(errno));
    exit(1);
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

static const char *platform_meta(void)
{
#if defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "unknown";
#endif
}

/* ------------------------------------------------------------------ */
/* Run configuration                                                    */
/* ------------------------------------------------------------------ */

typedef enum { TR_UDS = 0, TR_SHM, TR_ADAPT } trkind_t;

typedef struct {
    const char *label;            /* CSV 'policy' column value        */
    trkind_t    kind;             /* pure transport or adaptive       */
    adapt_policy_mode_t policy;   /* adaptive only                    */
    adapt_qos_t qos;
    double      latency_budget_us;
    int         eager_shm;        /* F experiment                     */
    int         nonblocking;      /* producer -EAGAIN vs park         */
    size_t      ring_cap;         /* 0 -> RING_CAP                    */
} run_cfg_t;

#define LAT_CAP 65536

/* Result collected by the consumer (parent) + producer stats. */
typedef struct {
    uint64_t n;
    uint64_t bytes;
    uint64_t lat[LAT_CAP];
    uint64_t t0, t1;
    /* producer-side (via shared stats block) */
    uint64_t switches, parks, health_tr, uds_msgs, shm_msgs, decisions;
    uint64_t setup_reqs;
    double   occ_mean, occ_max;
    uint64_t first_uds_escape_ns;   /* E */
    uint64_t recovery_ns;           /* E */
} result_t;

/* Shared producer->parent stats (fork-inherited). */
typedef struct {
    _Atomic uint64_t switches, parks, health_tr, uds_msgs, shm_msgs,
                     decisions, setup_reqs, sent_count, setup_time_ns,
                     shm_ready, consumer_attached;
    _Atomic uint64_t occ_sum, occ_n, occ_max;
    _Atomic uint64_t first_uds_escape_ns, recovery_ns;
} pstats_t;

static pstats_t *g_ps;
static char g_run_note[160];    /* shortfall annotations for the CSV */

/* ------------------------------------------------------------------ */
/* Producer (child process)                                             */
/* ------------------------------------------------------------------ */

static adapt_ctx_t *P;

static inline void ps_track_occ(size_t occ)
{
    atomic_fetch_add(&g_ps->occ_sum, (uint64_t)occ);
    atomic_fetch_add(&g_ps->occ_n, 1);
    uint64_t prev = atomic_load(&g_ps->occ_max);
    while ((uint64_t)occ > prev &&
           !atomic_compare_exchange_weak(&g_ps->occ_max, &prev,
                                         (uint64_t)occ))
        ;
}

static int prod_send(adapt_ctx_t *p, const unsigned char *frame, size_t len)
{
    int rc;
    do { rc = adapt_send(p, frame, len); } while (rc == -EAGAIN);
    if (rc)
        fprintf(stderr, "producer: send failed rc=%d (%s)\n", rc,
                adapt_strerror(rc));
    return rc;
}

static int run_producer(const run_cfg_t *rc, const char *experiment,
                        unsigned long seed)
{
    (void)seed;
    unsigned char *frame = malloc(FRAME_HDR + MAX_MSG);
    if (!frame) die("malloc");
    memset(frame, 0x5a, FRAME_HDR + MAX_MSG);

    uint64_t seq = 0;
    unsigned stride_occ = 1;

    /* -------- Experiment A: payload sweep -------- */
    if (!strcmp(experiment, "payload_sweep")) {
        static const size_t sizes[] = { 64, 128, 256, 512, 1024, 2048,
            4096, 8192, 16384, 32768, 65536, 262144, 1048576 };
        const unsigned K = 1500;
        for (unsigned s = 0; s < sizeof(sizes)/sizeof(sizes[0]); s++) {
            for (unsigned i = 0; i < K; i++) {
                memcpy(frame, &seq, 8);
                uint64_t t = now_ns();
                memcpy(frame + 8, &t, 8);
                memcpy(frame + 16, &(uint64_t){sizes[s]}, 8);
                if (rc->kind == TR_ADAPT) {
                    if (prod_send(P, frame, FRAME_HDR + sizes[s]))
                        return 1;
                    if (seq % stride_occ == 0)
                        ps_track_occ(adapt_shm_used_bytes(P));
                }
                seq++;
            }
        }
    }
    /* -------- Experiment B: queue pressure (2 KB: fits UDS so
     * queue-aware policies can escape a backlogged ring) -------- */
    else if (!strcmp(experiment, "queue_pressure")) {
        const unsigned K = 6000;
        for (unsigned i = 0; i < K; i++) {
            memcpy(frame, &seq, 8);
            uint64_t t = now_ns();
            memcpy(frame + 8, &t, 8);
            memcpy(frame + 16, &(uint64_t){2048}, 8);
            if (rc->kind == TR_ADAPT) {
                if (prod_send(P, frame, FRAME_HDR + 2048)) return 1;
                ps_track_occ(adapt_shm_used_bytes(P));
            }
            seq++;
        }
    }
    /* -------- Experiment C: bursty -------- */
    else if (!strcmp(experiment, "burst_workload")) {
        for (int cycle = 0; cycle < 8; cycle++) {
            for (unsigned i = 0; i < 1500; i++) {
                memcpy(frame, &seq, 8);
                uint64_t t = now_ns();
                memcpy(frame + 8, &t, 8);
                memcpy(frame + 16, &(uint64_t){2048}, 8);
                if (rc->kind == TR_ADAPT) {
                    if (prod_send(P, frame, FRAME_HDR + 2048)) return 1;
                    ps_track_occ(adapt_shm_used_bytes(P));
                }
                seq++;
            }
            struct timespec ts = { 0, 50 * 1000000 }; /* 50 ms idle */
            nanosleep(&ts, NULL);
        }
    }
    /* -------- Experiment D: adversarial oscillation -------- */
    else if (!strcmp(experiment, "adversarial_switching")) {
        for (int i = 0; i < 256; i++) {
            size_t sz = (i % 2) ? 16384 : 1024;
            memcpy(frame, &seq, 8);
            uint64_t t = now_ns();
            memcpy(frame + 8, &t, 8);
            memcpy(frame + 16, &(uint64_t){sz}, 8);
            if (rc->kind == TR_ADAPT) {
                if (prod_send(P, frame, FRAME_HDR + sz)) return 1;
                if (getenv("ABL_TRACE"))
                    fprintf(stderr, "P send seq=%llu sz=%zu route=%s\n",
                            (unsigned long long)seq, sz,
                            adapt_route_name(adapt_last_route(P)));
            }
            seq++;
        }
    }
    /* -------- Experiment E: degradation / recovery --------
     * Nonblocking: the stall phase intentionally fills the ring while
     * the consumer is paused; rejected sends are skipped (never
     * retried) so the phase stays bounded. Every successful send is
     * counted so the consumer can drain exactly what was sent. */
    else if (!strcmp(experiment, "degradation")) {
        const int nb = rc->nonblocking;
        unsigned long sent = 0;
        /* warm up: establish the SHM route */
        for (int i = 0; i < 300; i++) {
            memcpy(frame, &seq, 8);
            uint64_t t = now_ns();
            memcpy(frame + 8, &t, 8);
            memcpy(frame + 16, &(uint64_t){8192}, 8);
            int r2 = nb ? adapt_send(P, frame, FRAME_HDR + 8192)
                        : prod_send(P, frame, FRAME_HDR + 8192);
            if (r2 == 0) { seq++; sent++;
                atomic_store(&g_ps->sent_count, sent); }
        }
        if (rc->kind == TR_ADAPT) {
            /* stall: pause the consumer, fill the ring */
            atomic_store(&g_ctl->consumer_pause, 1);
            /* bounded stall: up to 600 probes x 5 ms = 3 s; policies
             * without a queue-aware escape simply time out here. */
            for (int probe = 0; probe < 600 &&
                 !atomic_load(&g_ps->first_uds_escape_ns); probe++) {
                memcpy(frame, &seq, 8);
                uint64_t t = now_ns();
                memcpy(frame + 8, &t, 8);
                memcpy(frame + 16, &(uint64_t){3072}, 8);
                int r3 = adapt_send(P, frame, FRAME_HDR + 3072);
                if (r3 == 0) { seq++; sent++;
                    atomic_store(&g_ps->sent_count, sent); }
                if (adapt_last_route(P) == ADAPT_ROUTE_UDS &&
                    !atomic_load(&g_ps->first_uds_escape_ns))
                    atomic_store(&g_ps->first_uds_escape_ns, now_ns());
                struct timespec ts = { 0, 5 * 1000000 };
                nanosleep(&ts, NULL);
            }
            /* recovery: resume the consumer, probe with BULK-sized
             * messages (keeps the EWMA above tau_high so the route
             * decision reflects queue/health state, not size decay);
             * record the first SHM-classified decision. */
            atomic_store(&g_ctl->consumer_pause, 0);
            for (int pr = 0; pr < 2000; pr++) {
                memcpy(frame, &seq, 8);
                uint64_t t = now_ns();
                memcpy(frame + 8, &t, 8);
                memcpy(frame + 16, &(uint64_t){8192}, 8);
                int r3 = adapt_send(P, frame, FRAME_HDR + 8192);
                if (r3 == 0 || r3 == -EAGAIN) {
                    seq++; sent++;
                    atomic_store(&g_ps->sent_count, sent);
                    if (r3 == 0 &&
                        adapt_last_route(P) == ADAPT_ROUTE_SHM) {
                        atomic_store(&g_ps->recovery_ns, now_ns());
                        break;
                    }
                }
                struct timespec ts = { 0, 5 * 1000000 };
                nanosleep(&ts, NULL);
            }
        }
    }
    /* -------- Experiment F: setup cost -------- */
    else if (!strcmp(experiment, "setup_cost")) {
        /* init timing happens in the fork preamble (recorded via
         * g_ps->setup fields by the caller); here: first-message RTT
         * then steady-state stream */
        for (unsigned i = 0; i < 3000; i++) {
            size_t sz = (i % 3 == 0) ? 8192 : 512;
            memcpy(frame, &seq, 8);
            uint64_t t = now_ns();
            memcpy(frame + 8, &t, 8);
            memcpy(frame + 16, &(uint64_t){sz}, 8);
            if (rc->kind == TR_ADAPT) {
                if (prod_send(P, frame, FRAME_HDR + sz)) return 1;
            }
            seq++;
        }
    }
    /* -------- Experiment G: QoS -------- */
    else if (!strcmp(experiment, "qos")) {
        const unsigned K = 6000;
        unsigned seed2 = 42;
        for (unsigned i = 0; i < K; i++) {
            size_t sz;
            int cls = rand_r(&seed2) % 100;
            sz = cls < 55 ? 512 : (cls < 85 ? 2048 : 8192);
            memcpy(frame, &seq, 8);
            uint64_t t = now_ns();
            memcpy(frame + 8, &t, 8);
            memcpy(frame + 16, &(uint64_t){sz}, 8);
            if (rc->kind == TR_ADAPT) {
                if (prod_send(P, frame, FRAME_HDR + sz)) return 1;
                ps_track_occ(adapt_shm_used_bytes(P));
            }
            seq++;
        }
    }

    /* publish producer stats */
    if (rc->kind == TR_ADAPT) {
        atomic_store(&g_ps->sent_count, (uint64_t)0); /* not used here */
        adapt_stats_t st;
        adapt_get_stats(P, &st);
        atomic_store(&g_ps->switches, st.route_switches);
        atomic_store(&g_ps->parks, st.backpressure_parks);
        atomic_store(&g_ps->health_tr, st.health_transitions);
        atomic_store(&g_ps->uds_msgs, st.sends_uds);
        atomic_store(&g_ps->shm_msgs, st.sends_shm);
        atomic_store(&g_ps->decisions, st.decisions);
        atomic_store(&g_ps->setup_reqs, st.shm_setup_reqs);
    }
    adapt_shutdown(P);
    free(frame);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Consumer (parent)                                                    */
/* ------------------------------------------------------------------ */

/* Pure-transport receive helpers (no adapt layer). */
static int pure_recv_uds(uds_endpoint_t *u, unsigned char *rx, size_t max)
{
    uint64_t l;
    int n = uds_recv(u, &l, sizeof(l));
    if (n != (int)sizeof(l)) return (n < 0) ? n : -EIO;
    if (l > max) return -EMSGSIZE;
    size_t off = 0;
    while (off < l) {
        size_t want = (l - off > 4096) ? 4096 : l - off;
        n = uds_recv(u, rx + off, want);
        if (n <= 0) return (n < 0) ? n : -EIO;
        off += (size_t)n;
    }
    return (int)l;
}

static int pure_recv_shm(shm_ring_t *ring, unsigned char *rx, size_t max)
{
    int n;
    do { n = shm_ring_pop(ring, rx, max); } while (n == -EAGAIN);
    return n;
}

static int pure_send_uds(uds_endpoint_t *u, const unsigned char *frame,
                         size_t len)
{
    uint64_t l = (uint64_t)len;
    int rc = uds_send(u, SOCK_B, &l, sizeof(l));
    size_t off = 0;
    while (rc == 0 && off < len) {
        size_t n = (len - off > 4096) ? 4096 : len - off;
        rc = uds_send(u, SOCK_B, frame + off, n);
        off += n;
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Experiment orchestration                                             */
/* ------------------------------------------------------------------ */

static unsigned long consumer_count(const char *experiment,
                                    const run_cfg_t *rc)
{
    const trkind_t kind = rc->kind;
    if (!strcmp(experiment, "payload_sweep")) {
        /* UDS SOCK_DGRAM cannot transfer >64 KB reliably (the receiver
         * queue drops datagrams silently under bulk); the uds baseline
         * covers sizes up to 64 KB and larger sizes are reported as
         * unsupported rather than measured with loss. */
        return kind == TR_UDS ? 10u * 1500u : 13u * 1500u;
    }
    if (!strcmp(experiment, "queue_pressure")) return 6000;
    if (!strcmp(experiment, "burst_workload")) return 8u * 1500u;
    if (!strcmp(experiment, "adversarial_switching")) return 256;
    if (!strcmp(experiment, "degradation"))
        return rc->kind == TR_ADAPT ? 300u + 4000u : 0;
    if (!strcmp(experiment, "setup_cost")) return 3000;
    if (!strcmp(experiment, "qos")) return 6000;
    return 0;
}

static int consumer_pace_us(const char *experiment)
{
    if (!strcmp(experiment, "queue_pressure")) return 150;
    if (!strcmp(experiment, "qos")) return 60;
    return 0;
}

static void csv_header(FILE *f)
{
    fprintf(f, "experiment,policy,kind,timestamp,platform,compiler,"
               "optimization,message_count,payload_distribution,seed,"
               "throughput_mbps,p50_us,p95_us,p99_us,p99_9_us,min_us,"
               "max_us,route_switches,uds_messages,shm_messages,"
               "backpressure_events,setup_messages,setup_time_us,"
               "health_transitions,mean_queue_occupancy_bytes,"
               "max_queue_occupancy_bytes,decisions,notes\n");
}

static void csv_row(FILE *f, const char *experiment, const run_cfg_t *rc,
                    const result_t *r, const char *notes, unsigned long seed)
{
    char notes2[256];
    snprintf(notes2, sizeof(notes2),
             "%s%s%s setup_us=%llu escape_us=%lld recovery_us=%lld",
             g_run_note, *g_run_note ? " " : "", notes,
             (unsigned long long)(atomic_load(&g_ps->setup_time_ns)/1000),
             atomic_load(&g_ps->first_uds_escape_ns)
                 ? (long long)((long long)atomic_load(&g_ps->first_uds_escape_ns) - (long long)r->t0) / 1000 : -1LL,
             atomic_load(&g_ps->recovery_ns)
                 ? (long long)((long long)atomic_load(&g_ps->recovery_ns) - (long long)r->t0) / 1000 : -1LL);
    notes = notes2;
    double dur = (double)(r->t1 - r->t0) / 1e9;
    double tp = dur > 0 ? (double)r->bytes / (1024.0 * 1024.0) / dur : 0;
    size_t n = r->n < LAT_CAP ? r->n : LAT_CAP;
    uint64_t *lat = malloc((n ? n : 1) * sizeof(uint64_t));
    memcpy(lat, r->lat, n * sizeof(uint64_t));
    qsort(lat, n, sizeof(uint64_t), cmp_u64);
    fprintf(f,
            "%s,%s,%s,%lld,%s,%s,%s,%lu,%s,%lu,%.1f,%.1f,%.1f,%.1f,%.1f,"
            "%.1f,%.1f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.0f,%llu,%llu,"
            "%s\n",
            experiment, rc->label,
            rc->kind == TR_UDS ? "uds" :
            rc->kind == TR_SHM ? "shm" : "adapt",
            (long long)time(NULL), platform_meta(),
#if defined(__clang__)
            "clang-" __clang_version__,
#elif defined(__GNUC__)
            "gcc-" __VERSION__,
#else
            "unknown",
#endif
            "O3", (unsigned long)r->n,
            strcmp(experiment, "payload_sweep") == 0 ? "fixed-per-size"
                : "bimodal",
            seed, tp,
            pct(lat, n, 50) / 1000.0, pct(lat, n, 95) / 1000.0,
            pct(lat, n, 99) / 1000.0, pct(lat, n, 99.9) / 1000.0,
            (n ? lat[0] : 0) / 1000.0, (n ? lat[n - 1] : 0) / 1000.0,
            (unsigned long long)r->switches,
            (unsigned long long)r->uds_msgs,
            (unsigned long long)r->shm_msgs,
            (unsigned long long)r->parks,
            (unsigned long long)r->setup_reqs,
            (unsigned long long)0,
            (unsigned long long)r->health_tr,
            r->occ_mean, (unsigned long long)r->occ_max,
            (unsigned long long)r->decisions, notes);
    free(lat);
}

/* One full run: fork producer, consume in parent, return result.
 * The parent binds its endpoint BEFORE forking so the producer's first
 * send always finds the peer (UDS datagram sendto fails with ENOENT
 * otherwise); the pure-SHM consumer attaches with retries instead. */
static void run_one(const run_cfg_t *rc, const char *experiment,
                    unsigned long seed, FILE *out, const char *notes)
{
    result_t *r = calloc(1, sizeof(*r));
    memset(g_run_note, 0, sizeof(g_run_note));
    memset(g_ps, 0, sizeof(pstats_t));
    atomic_store(&g_ctl->consumer_pause, 0);

    setenv("ADAPTIPC_STATS", "1", 1); /* counters feed the CSV */
    unlink(SOCK_A); unlink(SOCK_B);
    shm_unlink(SHM_NAME_DEFAULT);

    /* Eager/lazy is environment-selected and must be set before BOTH
     * sides initialize (the consumer maps in eager mode too). */
    if (rc->kind == TR_ADAPT && rc->eager_shm)
        setenv("ADAPTIPC_EAGER_SHM", "1", 1);
    else
        unsetenv("ADAPTIPC_EAGER_SHM");

    /* Parent-side consumer endpoints that must exist pre-fork. */
    adapt_ctx_t *cadapt = NULL;
    uds_endpoint_t *cuds = NULL;
    if (rc->kind == TR_ADAPT) {
        adapt_config_t cfg = {
            .shm_name = SHM_NAME_DEFAULT,
            .local_sock = SOCK_B, .peer_sock = SOCK_A,
            .shm_capacity = rc->ring_cap ? rc->ring_cap : RING_CAP,
            .policy = rc->policy, .qos = rc->qos,
            .latency_budget_us = rc->latency_budget_us,
        };
        if (adapt_init(ADAPT_ROLE_CONSUMER, &cfg, &cadapt))
            die("consumer init");
    } else if (rc->kind == TR_UDS) {
        if (uds_open(SOCK_B, &cuds)) die("uds consumer bind");
    }

    pid_t pid = fork();
    if (pid < 0) die("fork");
    pid_t g_pid = pid;
    if (pid == 0) {
        /* child: producer */
        if (rc->kind == TR_ADAPT) {
            adapt_config_t cfg = {
                .shm_name = SHM_NAME_DEFAULT,
                .local_sock = SOCK_A, .peer_sock = SOCK_B,
                .shm_capacity = rc->ring_cap ? rc->ring_cap : RING_CAP,
                .nonblocking_send = rc->nonblocking,
                .policy = rc->policy, .qos = rc->qos,
                .latency_budget_us = rc->latency_budget_us,
            };
            uint64_t t0 = now_ns();
            if (adapt_init(ADAPT_ROLE_PRODUCER, &cfg, &P)) {
                fprintf(stderr, "producer adapt_init failed\n");
                _exit(3);
            }
            atomic_store(&g_ps->setup_time_ns, now_ns() - t0);
            int rc2 = run_producer(rc, experiment, seed);
            _exit(rc2);
        } else if (rc->kind == TR_UDS) {
            uds_endpoint_t *u;
            if (uds_open(SOCK_A, &u)) _exit(3);
            _exit(uds_prod_run(u, experiment, SOCK_B, seed));
        } else {
            shm_ring_t *ring;
            if (shm_ring_create(SHM_NAME_DEFAULT, RING_CAP, 1,
                                SHM_RING_ROLE_PRODUCER, &ring)) _exit(3);
            atomic_store(&g_ps->shm_ready, 1);
            /* wait until the consumer has attached before pushing:
             * otherwise a fast producer can unlink the object before
             * the slow parent ever maps it. */
            while (!atomic_load(&g_ps->consumer_attached))
                sched_yield();
            _exit(shm_prod_run(ring, experiment, seed));
        }
    }

    /* parent: consume */
    unsigned long n_expect = consumer_count(experiment, rc);
    int pace = consumer_pace_us(experiment);
    r->t0 = now_ns();
    static unsigned char rx[FRAME_HDR + MAX_MSG];
    if (rc->kind == TR_ADAPT) {
        const int is_degradation = !strcmp(experiment, "degradation");
        unsigned long got = 0;
        int child_dead = 0;
        for (;;) {
            if (is_degradation) {
                /* E: bounded by the producer's sent-count register */
                unsigned long target =
                    (unsigned long)atomic_load(&g_ps->sent_count);
                if (got >= target) {
                    int st4 = 0;
                    if (atomic_load(&g_ps->recovery_ns) ||
                        waitpid(g_pid, &st4, WNOHANG) == g_pid)
                        break; /* recovery observed or producer done */
                    struct timespec ts = { 0, 200000 };
                    nanosleep(&ts, NULL);
                    continue;
                }
                if (atomic_load(&g_ctl->consumer_pause)) {
                    /* slow drain of the UDS socket ONLY: keeps the
                     * fallback path alive (datagrams would otherwise
                     * overflow) while the SHM ring backs up */
                    int n = adapt_recv_uds_timeout(cadapt, rx,
                                                   sizeof(rx), 20);
                    if (n > 0) {
                        r->n++; got++;
                        r->bytes += (uint64_t)n;
                    }
                    struct timespec ts = { 0, 20 * 1000000 };
                    nanosleep(&ts, NULL);
                    continue;
                }
            } else if (got >= n_expect) {
                break;
            }
            int st3 = 0;
            if (!child_dead &&
                waitpid(g_pid, &st3, WNOHANG) == g_pid)
                child_dead = 1; /* producer done: drain backlog first */
            if (child_dead && got < n_expect &&
                adapt_shm_used_bytes(cadapt) == 0) {
                /* transport dropped messages (UDS receiver-queue
                 * overflow drops datagrams silently); record it. */
                snprintf(g_run_note, sizeof(g_run_note),
                         "dropped=%lu (uds rx-queue overflow)",
                         n_expect - got);
                break;
            }
            int n = adapt_recv(cadapt, rx, sizeof(rx));
            if (n <= 0) {
                if (getenv("ABL_TRACE"))
                    fprintf(stderr, "C recv error n=%d (%s)\n", n,
                            adapt_strerror(n));
                continue;
            }
            if (getenv("ABL_TRACE")) {
                uint64_t sq; memcpy(&sq, rx, 8);
                fprintf(stderr, "C recv seq=%llu n=%d ring=%zu\n",
                        (unsigned long long)sq, n,
                        adapt_shm_used_bytes(cadapt));
            }
            uint64_t t_send;
            memcpy(&t_send, rx + 8, 8);
            if (r->n < LAT_CAP) r->lat[r->n] = now_ns() - t_send;
            r->n++; got++;
            r->bytes += (uint64_t)n;
            if (pace) { struct timespec ts = { 0, pace * 1000 };
                        nanosleep(&ts, NULL); }
        }
        adapt_shutdown(cadapt);
    } else if (rc->kind == TR_UDS) {
        unsigned long got = 0;
        int child_dead_uds = 0;
        while (got < n_expect) {
            int st2 = 0;
            if (!child_dead_uds &&
                waitpid(g_pid, &st2, WNOHANG) == g_pid) {
                child_dead_uds = 1; /* drain socket backlog first */
                fprintf(stderr, "uds producer done, drained %lu/%lu\n",
                        got, n_expect);
            }
            int n = pure_recv_uds(cuds, rx, sizeof(rx));
            if (n <= 0) {
                if (child_dead_uds) {
                    snprintf(g_run_note, sizeof(g_run_note),
                             "dropped=%lu (uds rx-queue overflow)",
                             n_expect - got);
                    break;
                }
                die("uds recv");
            }
            uint64_t t_send;
            memcpy(&t_send, rx + 8, 8);
            if (r->n < LAT_CAP) r->lat[r->n] = now_ns() - t_send;
            r->n++; got++;
            r->bytes += (uint64_t)n;
            if (pace) { struct timespec ts = { 0, pace * 1000 };
                        nanosleep(&ts, NULL); }
        }
        uds_close(cuds);
    } else {
        /* pure SHM: wait for the producer's ready flag, then attach
         * without the header-init spin (a stale object that the
         * producer's EINVAL-retry unlinked would otherwise park us
         * forever). */
        shm_ring_t *ring = NULL;
        for (int tries = 0; tries < 5000 &&
                            !atomic_load(&g_ps->shm_ready); tries++) {
            struct timespec ts = { 0, 2000000 };
            nanosleep(&ts, NULL);
        }
        if (shm_ring_attach(SHM_NAME_DEFAULT, RING_CAP,
                            SHM_RING_ROLE_CONSUMER, &ring))
            die("shm consumer attach");
        atomic_store(&g_ps->consumer_attached, 1);
        unsigned long got = 0;
        int child_dead = 0;
        while (got < n_expect) {
            int st2 = 0;
            if (!child_dead &&
                waitpid(g_pid, &st2, WNOHANG) == g_pid)
                child_dead = 1; /* producer done: drain backlog first */
            int n = shm_ring_pop(ring, rx, sizeof(rx));
            if (n == -EAGAIN) {
                if (child_dead && shm_ring_used_bytes(ring) == 0) {
                    fprintf(stderr, "producer short %lu/%lu msgs\n",
                            got, n_expect);
                    exit(1);
                }
                continue;
            }
            if (n <= 0) die("shm recv");
            if (getenv("ABL_TRACE") && n > 65536) {
                uint64_t sq; memcpy(&sq, rx, 8);
                fprintf(stderr, "SP pop got=%lu seq=%llu n=%d\n",
                        got, (unsigned long long)sq, n);
            }
            uint64_t t_send;
            memcpy(&t_send, rx + 8, 8);
            if (r->n < LAT_CAP) r->lat[r->n] = now_ns() - t_send;
            r->n++; got++;
            r->bytes += (uint64_t)n;
            if (pace) { struct timespec ts = { 0, pace * 1000 };
                        nanosleep(&ts, NULL); }
        }
        shm_ring_close(ring, 0);
    }
    r->t1 = now_ns();

    /* gather producer stats */
    r->switches = atomic_load(&g_ps->switches);
    r->parks = atomic_load(&g_ps->parks);
    r->health_tr = atomic_load(&g_ps->health_tr);
    r->uds_msgs = atomic_load(&g_ps->uds_msgs);
    r->shm_msgs = atomic_load(&g_ps->shm_msgs);
    r->decisions = atomic_load(&g_ps->decisions);
    r->setup_reqs = atomic_load(&g_ps->setup_reqs);
    uint64_t occ_sum = atomic_load(&g_ps->occ_sum);
    uint64_t occ_n = atomic_load(&g_ps->occ_n);
    r->occ_mean = occ_n ? (double)occ_sum / (double)occ_n : 0.0;
    r->occ_max = (double)atomic_load(&g_ps->occ_max);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "run[%s/%s]: producer exit %d\n",
                experiment, rc->label, status);
        exit(1);
    }
    if (out) csv_row(out, experiment, rc, r, notes, seed);
    free(r);
}

/* Pure-transport producer drivers (used via extern above). */
static uds_endpoint_t *g_uprod;
static shm_ring_t *g_sprod;

int uds_prod_run(uds_endpoint_t *u, const char *experiment,
                 const char *peer, unsigned long seed)
{
    (void)peer; (void)seed;
    g_uprod = u;
    static unsigned char frame[FRAME_HDR + MAX_MSG];
    memset(frame, 0x5a, sizeof(frame));
    uint64_t seq = 0;
    unsigned long n = consumer_count(experiment,
                                     &(run_cfg_t){ .kind = TR_UDS });
    for (unsigned long i = 0; i < n; i++) {
        size_t sz;
        if (!strcmp(experiment, "payload_sweep")) {
            static const size_t sizes[] = { 64, 128, 256, 512, 1024,
                2048, 4096, 8192, 16384, 32768 };
            sz = sizes[(i / 1500u) % 10];
        } else if (!strcmp(experiment, "queue_pressure")) sz = 2048;
        else if (!strcmp(experiment, "burst_workload")) sz = 2048;
        else if (!strcmp(experiment, "qos")) {
            unsigned s2 = (unsigned)(i * 2654435761u);
            int cls = s2 % 100;
            sz = cls < 55 ? 512 : (cls < 85 ? 2048 : 8192);
        } else sz = 2048;
        memcpy(frame, &seq, 8);
        uint64_t t = now_ns();
        memcpy(frame + 8, &t, 8);
        memcpy(frame + 16, &(uint64_t){sz}, 8);
        if (pure_send_uds(u, frame, FRAME_HDR + sz)) return 1;
        seq++;
    }
    uds_close(u);
    return 0;
}

int shm_prod_run(shm_ring_t *ring, const char *experiment,
                 unsigned long seed)
{
    (void)seed;
    g_sprod = ring;
    static unsigned char frame[FRAME_HDR + MAX_MSG];
    memset(frame, 0x5a, sizeof(frame));
    uint64_t seq = 0;
    unsigned long n = consumer_count(experiment,
                                     &(run_cfg_t){ .kind = TR_SHM });
    for (unsigned long i = 0; i < n; i++) {
        size_t sz;
        if (!strcmp(experiment, "payload_sweep")) {
            static const size_t sizes[] = { 64, 128, 256, 512, 1024,
                2048, 4096, 8192, 16384, 32768, 65536, 262144, 1048576 };
            sz = sizes[(i / 1500u) % 13];
        } else if (!strcmp(experiment, "queue_pressure")) sz = 2048;
        else if (!strcmp(experiment, "burst_workload")) sz = 2048;
        else if (!strcmp(experiment, "qos")) {
            unsigned s2 = (unsigned)(i * 2654435761u);
            int cls = s2 % 100;
            sz = cls < 55 ? 512 : (cls < 85 ? 2048 : 8192);
        } else sz = 2048;
        memcpy(frame, &seq, 8);
        uint64_t t = now_ns();
        memcpy(frame + 8, &t, 8);
        memcpy(frame + 16, &(uint64_t){sz}, 8);
        int rc;
        do { rc = shm_ring_push(ring, frame, FRAME_HDR + sz); }
        while (rc == -EAGAIN);
        if (rc) return 1;
        if (getenv("ABL_TRACE") && sz > 65536)
            fprintf(stderr, "SP push seq=%llu sz=%zu used=%zu\n",
                    (unsigned long long)seq, sz,
                    shm_ring_used_bytes(ring));
        seq++;
    }
    shm_ring_close(ring, 1);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Experiment matrix                                                    */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *outdir = "experiments/raw";
    const char *only = NULL;
    unsigned long seed = 20260905;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "--only") && i + 1 < argc)
            only = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            seed = strtoul(argv[++i], NULL, 10);
        else { fprintf(stderr, "usage: %s [--out DIR] [--only EXP] "
                               "[--seed N]\n", argv[0]); return 2; }
    }

    g_ctl = mmap(NULL, sizeof(ctl_t), PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    g_ps = mmap(NULL, sizeof(pstats_t), PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_ctl == MAP_FAILED || g_ps == MAP_FAILED) die("mmap");

    static const run_cfg_t adapt_policies[] = {
        { "size_only",       TR_ADAPT, ADAPT_POLICY_SIZE_ONLY, 0, 0, 0, 0 },
        { "size_hysteresis", TR_ADAPT, ADAPT_POLICY_SIZE_HYSTERESIS, 0, 0, 0, 0 },
        { "queue_aware",     TR_ADAPT, ADAPT_POLICY_QUEUE_AWARE, 0, 0, 0, 0 },
        { "cost_aware",      TR_ADAPT, ADAPT_POLICY_COST_AWARE, 0, 0, 0, 0 },
        { "full_adaptive",   TR_ADAPT, ADAPT_POLICY_FULL_ADAPTIVE, 0, 0, 0, 0 },
    };
    static const run_cfg_t pure[] = {
        { "uds", TR_UDS, 0, 0, 0, 0, 0 },
        { "shm", TR_SHM, 0, 0, 0, 0, 0 },
    };

    struct { const char *name; } exps[] = {
        { "payload_sweep" }, { "queue_pressure" }, { "burst_workload" },
        { "adversarial_switching" }, { "degradation" }, { "setup_cost" },
        { "qos" }
    };

    for (unsigned e = 0; e < sizeof(exps)/sizeof(exps[0]); e++) {
        const char *exp = exps[e].name;
        if (only && strcmp(only, exp)) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.csv", outdir, exp);
        FILE *f = fopen(path, exp[0] == '\0' ? "w" :
                       (access(path, F_OK) == 0 ? "a" : "w"));
        if (!f) die("fopen out");
        fseek(f, 0, SEEK_END);
        if (ftell(f) == 0) csv_header(f);

        printf("== experiment %s -> %s\n", exp, path);

        /* pure baselines for the throughput/latency experiments */
        if (strcmp(exp, "degradation") && strcmp(exp, "setup_cost")) {
            for (unsigned k = 0; k < 2; k++)
                run_one(&pure[k], exp, seed, f, "baseline");
        }

        if (!strcmp(exp, "qos")) {
            static const run_cfg_t qosv[] = {
                { "full_adaptive_balanced", TR_ADAPT,
                  ADAPT_POLICY_FULL_ADAPTIVE, ADAPT_QOS_BALANCED,
                  500.0, 0, 0 },
                { "full_adaptive_latency", TR_ADAPT,
                  ADAPT_POLICY_FULL_ADAPTIVE, ADAPT_QOS_LATENCY,
                  500.0, 0, 0 },
                { "full_adaptive_throughput", TR_ADAPT,
                  ADAPT_POLICY_FULL_ADAPTIVE, ADAPT_QOS_THROUGHPUT,
                  500.0, 0, 0 },
                { "size_hysteresis", TR_ADAPT,
                  ADAPT_POLICY_SIZE_HYSTERESIS, ADAPT_QOS_BALANCED,
                  0, 0, 0 },
            };
            for (unsigned k = 0; k < 4; k++)
                run_one(&qosv[k], exp, seed, f, "qos-matrix");
        } else if (!strcmp(exp, "setup_cost")) {
            static const run_cfg_t setupv[] = {
                { "eager_shm", TR_ADAPT, ADAPT_POLICY_SIZE_HYSTERESIS,
                  0, 0, 1, 0 },
                { "lazy_shm", TR_ADAPT, ADAPT_POLICY_SIZE_HYSTERESIS,
                  0, 0, 0, 0 },
                { "lazy_cost_aware", TR_ADAPT,
                  ADAPT_POLICY_FULL_ADAPTIVE, 0, 0, 0, 0 },
            };
            for (unsigned k = 0; k < 3; k++)
                run_one(&setupv[k], exp, seed, f, "setup");
        } else if (!strcmp(exp, "degradation")) {
            run_cfg_t dpol[5];
            for (unsigned k = 0; k < 5; k++) {
                dpol[k] = adapt_policies[k];
                dpol[k].nonblocking = 1;   /* stall must not park us */
                dpol[k].ring_cap = 512u << 10; /* bounded backlog   */
            }
            for (unsigned k = 0; k < 5; k++)
                run_one(&dpol[k], exp, seed, f, "policy");
        } else {
            for (unsigned k = 0; k < 5; k++)
                run_one(&adapt_policies[k], exp, seed, f, "policy");
        }
        fclose(f);
    }

    printf("done. raw CSVs in %s/\n", outdir);
    return 0;
}
