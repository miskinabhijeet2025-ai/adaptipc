/*
 * benchmark_suite.c -- multi-process benchmark for AdaptIPC.
 *
 * Topology: fork() -> child = producer, parent = consumer + recorder.
 *
 * Modes:
 *   uds   : pure Unix-datagram baseline (large frames are chunked into
 *           4096-byte datagrams behind an 8-byte length prefix, since
 *           SOCK_DGRAM cannot carry megabyte payloads atomically)
 *   shm   : pure lock-free SPSC ring-buffer baseline
 *   adapt : AdaptIPC engine (EWMA + hysteresis routing)
 *
 * Workloads:
 *   sweep   : payload sizes 64 B .. 16 MB (powers of 2), aggregate rows
 *   bimodal : alternating 128 B control / 2 MB bulk with random bursts,
 *             one CSV row per message (feeds CDF + adaptation figures)
 *   thrash  : rapid oscillation inside the [1024, 4096] deadband;
 *             verifies zero route switches (hysteresis stability)
 *
 * CSV header (exact):
 *   workload,mode,payload_bytes,latency_ns,throughput_mbps,route_taken
 */
#define _POSIX_C_SOURCE 200809L

#include "adapt_ipc.h"
#include "shm_ringbuffer.h"
#include "uds_fallback.h"

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/wait.h>

#define FRAME_HDR     32u                /* sizeof(frame_hdr), packed-safe */
#define MAX_PAYLOAD   (16u << 20)        /* 16 MB */
#define BULK_PAYLOAD  (2u << 20)         /* 2 MB */
#define CONTROL_PAYLD 128u
#define SHM_CAP       (64u << 20)        /* ring payload capacity */
#define CHUNK         4096u              /* UDS chunk size */

#define SOCK_A "/tmp/adaptipc_bench_a.sock"
#define SOCK_B "/tmp/adaptipc_bench_b.sock"
#define SHM_NAME "/adaptipc_bench_shm"

#define CSV_HEADER "workload,mode,payload_bytes,latency_ns," \
                   "throughput_mbps,route_taken"

/* macOS calls it MAP_ANON; Linux provides MAP_ANONYMOUS. The value
 * 0x1000 is MAP_ANON on macOS/BSD when the macro is hidden by strict
 * POSIX feature-test macros. */
#include <sys/mman.h>
#if !defined(MAP_ANONYMOUS)
#if defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#else
#define MAP_ANONYMOUS 0x1000
#endif
#endif

typedef enum { BMODE_UDS = 0, BMODE_SHM = 1, BMODE_ADAPT = 2 } bmode_t;
typedef enum { WL_SWEEP = 0, WL_BIMODAL = 1, WL_THRASH = 2 } workload_t;

static const char *bmode_name(bmode_t m)
{
    static const char *n[] = { "uds", "shm", "adapt" };
    return n[m];
}

static const char *wl_name(workload_t w)
{
    static const char *n[] = { "sweep", "bimodal", "thrash" };
    return n[w];
}

/* Directory of the --out CSV (for breakdown files); "." if none. */
static char g_bench_dir[512] = ".";

static void die(const char *msg)
{
    fprintf(stderr, "benchmark_suite: %s (%s)\n", msg, strerror(errno));
    exit(1);
}

static void xclock(struct timespec *ts) { clock_gettime(CLOCK_MONOTONIC, ts); }

static double now_ns(void)
{
    struct timespec ts;
    xclock(&ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static uint64_t diff_ns(const struct timespec *a, const struct timespec *b)
{
    return (uint64_t)(b->tv_sec - a->tv_sec) * 1000000000ull +
           (uint64_t)(b->tv_nsec - a->tv_nsec);
}

/* ------------------------------------------------------------------ */
/* On-wire frame + transport abstraction                               */
/* ------------------------------------------------------------------ */

typedef struct {                       /* exactly FRAME_HDR bytes */
    uint32_t magic;
    uint32_t route_hint;
    uint64_t seq;
    uint64_t ts_sec;
    uint64_t ts_nsec;
} frame_hdr;

typedef struct {
    bmode_t         mode;
    uds_endpoint_t *uds;
    shm_ring_t     *ring;
    adapt_ctx_t    *adapt;
    const char     *peer_sock;
} bench_ctx;

static int bench_send(bench_ctx *c, const unsigned char *buf, size_t len)
{
    switch (c->mode) {
    case BMODE_SHM: {
        int rc;
        do {
            rc = shm_ring_push(c->ring, buf, len);
            if (rc == -EAGAIN) sched_yield();
        } while (rc == -EAGAIN);
        return rc;
    }
    case BMODE_ADAPT: {
        int rc;
        do {
            rc = adapt_send(c->adapt, buf, len);
            if (rc == -EAGAIN) sched_yield();
        } while (rc == -EAGAIN);
        return rc;
    }
    case BMODE_UDS:
    default: {
        /* 8-byte length-prefix datagram, then 4096-byte payload chunks.
         * AF_UNIX datagrams are ordered and lossless, so reassembly is
         * a simple sequential read loop. */
        uint64_t l = (uint64_t)len;
        int rc = uds_send(c->uds, c->peer_sock, &l, sizeof(l));
        if (rc != 0) return rc;
        size_t off = 0;
        while (off < len) {
            size_t n = (len - off > CHUNK) ? CHUNK : len - off;
            rc = uds_send(c->uds, c->peer_sock, buf + off, n);
            if (rc != 0) return rc;
            off += n;
        }
        return 0;
    }
    }
}

static int bench_recv(bench_ctx *c, unsigned char *buf, size_t max)
{
    switch (c->mode) {
    case BMODE_SHM: {
        int n;
        do { n = shm_ring_pop(c->ring, buf, max); } while (n == -EAGAIN);
        return n;
    }
    case BMODE_ADAPT: {
        int n;
        do { n = adapt_recv(c->adapt, buf, max); }
        while (n == -EAGAIN || n == -ECONNREFUSED);
        return n;
    }
    case BMODE_UDS:
    default: {
        uint64_t l;
        int n = uds_recv(c->uds, &l, sizeof(l));
        if (n != (int)sizeof(l)) return (n < 0) ? n : -EIO;
        if (l > max) return -EMSGSIZE;
        size_t off = 0;
        while (off < l) {
            size_t want = (l - off > CHUNK) ? CHUNK : l - off;
            n = uds_recv(c->uds, buf + off, want);
            if (n <= 0) return (n < 0) ? n : -EIO;
            off += (size_t)n;
        }
        return (int)l;
    }
    }
}


/* ------------------------------------------------------------------ */
/* Workload generators (deterministic; both sides derive the same list) */
/* ------------------------------------------------------------------ */

static unsigned long clampul(unsigned long v, unsigned long lo,
                             unsigned long hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static size_t *build_sizes(workload_t wl, unsigned long bimodal_iters,
                           size_t *count_out)
{
    size_t *sizes = NULL;
    size_t count = 0, cap = 0;

#define APPEND(s) do {                                              \
        if (count == cap) {                                         \
            cap = cap ? cap * 2 : 1024;                             \
            size_t *tmp = realloc(sizes, cap * sizeof(*sizes));     \
            if (!tmp) die("realloc");                               \
            sizes = tmp;                                            \
        }                                                           \
        sizes[count++] = (s);                                       \
    } while (0)

    if (wl == WL_SWEEP) {
        for (unsigned k = 0; k <= 18; ++k) {           /* 64 B .. 16 MB */
            size_t sz = (size_t)64 << k;
            /* enough reps for stable numbers, bounded runtime */
            unsigned long reps =
                clampul((32UL << 20) / (unsigned long)sz, 5UL, 1000UL);
            for (unsigned long r = 0; r < reps; ++r) APPEND(sz);
        }
    } else if (wl == WL_BIMODAL) {
        unsigned seed = 42;
        size_t prev = CONTROL_PAYLD;
        for (unsigned long i = 0; i < bimodal_iters; ++i) {
            size_t base = (i % 2 == 0) ? CONTROL_PAYLD : BULK_PAYLOAD;
            /* ~2% random burst: repeat the previous class instead of
             * switching, exercising hysteresis responsiveness. */
            if (i > 0 && (unsigned)(rand_r(&seed) % 97u) < 2u) {
                APPEND(prev);
            } else {
                APPEND(base);
                prev = base;
            }
        }
    } else { /* WL_THRASH: rapid oscillation inside the deadband */
        static const size_t pat[] = { 1024, 4096, 1024, 2048,
                                      4096, 1536, 3072, 4096 };
        unsigned long n = bimodal_iters < 20000UL ? 20000UL : bimodal_iters;
        for (unsigned long i = 0; i < n; ++i) APPEND(pat[i % 8]);
    }

#undef APPEND
    *count_out = count;
    return sizes;
}

/* Per-message route sharing producer -> consumer via shared anon mmap. */
static inline void publish_route(uint8_t *arr, unsigned long i, int route)
{
    __atomic_store_n(&arr[i], (uint8_t)route, __ATOMIC_RELEASE);
}


/* ------------------------------------------------------------------ */
/* Producer (child process)                                            */
/* ------------------------------------------------------------------ */

static int run_producer(bench_ctx *c, workload_t wl, const size_t *sizes,
                        size_t total, uint8_t *routes)
{
    unsigned char *buf = malloc(FRAME_HDR + MAX_PAYLOAD);
    if (!buf) die("malloc");

    for (size_t i = 0; i < total; ++i) {
        frame_hdr *h = (frame_hdr *)buf;
        struct timespec ts;
        xclock(&ts);
        h->magic    = 0x444D4341u;                 /* "ACMD" */
        h->seq      = (uint64_t)i;
        h->ts_sec   = (uint64_t)ts.tv_sec;
        h->ts_nsec  = (uint64_t)ts.tv_nsec;
        memset(buf + FRAME_HDR, (int)(i & 0xFF), sizes[i]);

        int rc = bench_send(c, buf, FRAME_HDR + sizes[i]);
        if (i % 500 == 0 || (i > 5300 && i < 5700) || rc != 0)
            fprintf(stderr, "P-SEND i=%zu rc=%d len=%zu\n", i, rc,
                    FRAME_HDR + sizes[i]);
        if (rc != 0) {
            fprintf(stderr, "producer: send failed at seq %zu: %s\n",
                    i, strerror(-rc));
            free(buf);
            return 1;
        }
        int route;
        if (c->mode == BMODE_ADAPT)
            route = (int)adapt_last_route(c->adapt);
        else
            route = (c->mode == BMODE_SHM) ? ADAPT_ROUTE_SHM
                                           : ADAPT_ROUTE_UDS;
        publish_route(routes, i, route);
    }

    if (c->mode == BMODE_ADAPT) {
        /* Producer-side breakdown (send path counters live only in this
         * forked child; dump before exit). */
        adapt_stats_t st;
        adapt_get_stats(c->adapt, &st);
        char path[512];
        snprintf(path, sizeof(path), "%s/latency_breakdown_%s_producer.txt",
                 g_bench_dir, wl_name(wl));
        FILE *bf = fopen(path, "w");
        if (bf) {
            fprintf(bf,
                "# latency breakdown (PRODUCER): mode=%s workload=%s\n"
                "workload=%s\n"
                "messages_sent=%zu\n"
                "\n[send-side]\n"
                "sends_total=%llu sends_shm=%llu sends_uds=%llu\n"
                "send_escalations_udsbeyond_dgram=%llu\n"
                "router_ewma_classify_total_ns=%llu\n"
                "router_ewma_classify_mean_ns=%llu\n"
                "send_alloc_total_ns=%llu send_alloc_mean_ns=%llu\n"
                "send_copy_total_ns=%llu send_copy_mean_ns=%llu\n"
                "send_copy_bytes=%llu\n",
                bmode_name(c->mode), wl_name(wl), wl_name(wl), total,
                (unsigned long long)st.sends,
                (unsigned long long)st.sends_shm,
                (unsigned long long)st.sends_uds,
                (unsigned long long)st.send_escalations,
                (unsigned long long)st.router_ns,
                (unsigned long long)(st.sends ? st.router_ns / st.sends : 0),
                (unsigned long long)st.send_alloc_ns,
                (unsigned long long)(st.sends_shm
                                     ? st.send_alloc_ns / st.sends_shm : 0),
                (unsigned long long)st.send_copy_ns,
                (unsigned long long)(st.sends
                                     ? st.send_copy_ns / st.sends : 0),
                (unsigned long long)st.send_copy_bytes);
            fclose(bf);
            fprintf(stderr, "[breakdown] wrote %s\n", path);
        }
    }

    free(buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Consumer (parent): collect samples, aggregate, write CSV            */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t    n;
    double   *latency_ns;
    size_t   *payload;
    int      *route;
    double    t_first_recv, t_last_recv;
    double    tp_mbps;
} samples;

static void samples_init(samples *s, size_t total)
{
    s->n = total;
    s->latency_ns = malloc(total * sizeof(double));
    s->payload    = malloc(total * sizeof(size_t));
    s->route      = malloc(total * sizeof(int));
    if (!s->latency_ns || !s->payload || !s->route) die("malloc");
}

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median(double *v, size_t n)
{
    qsort(v, n, sizeof(double), cmp_dbl);
    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

static inline int peek_route(const uint8_t *arr, unsigned long i)
{
    /* Route values are 1 (SHM) / 2 (UDS); 0 means "not yet published".
     * Guards the tiny window between the producer's send() returning and
     * its release-store of the route landing in the shared mapping. */
    uint8_t v;
    do {
        v = __atomic_load_n(&arr[i], __ATOMIC_ACQUIRE);
        if (v == 0) sched_yield();
    } while (v == 0);
    return v;
}


static void csv_write_rows(FILE *f, const char *wlname, bmode_t mode,
                           const samples *s, double dur_s, int aggregate)
{
    if (aggregate) {
        /* one aggregate row per distinct payload size (median latency;
         * throughput over the whole workload window) */
        for (size_t i = 0; i < s->n;) {
            size_t j = i;
            while (j < s->n && s->payload[j] == s->payload[i]) ++j;

            double *tmp = malloc((j - i) * sizeof(double));
            if (!tmp) die("malloc");
            memcpy(tmp, s->latency_ns + i, (j - i) * sizeof(double));
            double med = median(tmp, j - i);
            free(tmp);

            uint64_t bytes = 0;
            int shm_votes = 0;
            for (size_t k = i; k < j; ++k) {
                bytes += FRAME_HDR + s->payload[k];
                if (s->route[k] == ADAPT_ROUTE_SHM) ++shm_votes;
            }
            double tp = dur_s > 0 ? (double)bytes / dur_s / 1e6 : 0.0;
            fprintf(f, "%s,%s,%zu,%.0f,%.2f,%d\n",
                    wlname, bmode_name(mode), s->payload[i], med, tp,
                    (shm_votes * 2 >= (int)(j - i)) ? ADAPT_ROUTE_SHM
                                                    : ADAPT_ROUTE_UDS);
            i = j;
        }
    } else {
        /* per-message rows; throughput column carries the workload-level
         * average (repeated), latency/route are per message */
        for (size_t i = 0; i < s->n; ++i)
            fprintf(f, "%s,%s,%zu,%.0f,%.2f,%d\n",
                    wlname, bmode_name(mode), s->payload[i],
                    s->latency_ns[i], s->tp_mbps, s->route[i]);
    }
}

static int run_consumer(bench_ctx *c, workload_t wl, size_t total,
                        const uint8_t *routes, FILE *csv,
                        const char *wlname, bmode_t mode)
{
    samples s;
    samples_init(&s, total);
    unsigned char *buf = malloc(FRAME_HDR + MAX_PAYLOAD);
    if (!buf) die("malloc");

    unsigned long switches = 0;
    int prev_route = -1;
    struct timespec t_prev;

    for (size_t i = 0; i < total; ++i) {
        int n = bench_recv(c, buf, FRAME_HDR + MAX_PAYLOAD);
        if (i % 500 == 0 || (i > 5300 && i < 5700))
            fprintf(stderr, "C-RECV i=%zu n=%d\n", i, n);
        xclock(&t_prev);
        if (n < (int)FRAME_HDR) {
            fprintf(stderr, "consumer: recv failed at %zu: %s\n",
                    i, strerror(-n));
            free(buf);
            return 1;
        }
        if (i == 0) {
            struct timespec tmp; xclock(&tmp);
            s.t_first_recv = (double)tmp.tv_sec * 1e9 + (double)tmp.tv_nsec;
        }

        const frame_hdr *h = (const frame_hdr *)buf;
        struct timespec tsent = { (time_t)h->ts_sec, (long)h->ts_nsec };
        s.latency_ns[i] = (double)diff_ns(&tsent, &t_prev);
        s.payload[i]    = (size_t)n - FRAME_HDR;
        s.route[i]      = peek_route(routes, i);
        if (getenv("BS_TRACE")) fprintf(stderr, "C-ROUTE i=%zu r=%d\n", i, s.route[i]);
        if (prev_route != -1 && s.route[i] != prev_route) ++switches;
        prev_route = s.route[i];
    }
    {
        struct timespec tmp; xclock(&tmp);
        s.t_last_recv = (double)tmp.tv_sec * 1e9 + (double)tmp.tv_nsec;
    }
    free(buf);

    double dur_s = (s.t_last_recv - s.t_first_recv) / 1e9;
    uint64_t wire_total = 0;
    for (size_t i = 0; i < s.n; ++i) wire_total += FRAME_HDR + s.payload[i];
    s.tp_mbps = dur_s > 0 ? (double)wire_total / dur_s / 1e6 : 0.0;

    csv_write_rows(csv, wlname, mode, &s, dur_s, wl != WL_BIMODAL);
    fflush(csv);

    if (mode == BMODE_ADAPT) {
        /* Latency-component breakdown -> latency_breakdown_<wl>.txt */
        adapt_stats_t st;
        adapt_get_stats(c->adapt, &st);
        char path[512];
        snprintf(path, sizeof(path), "%s/latency_breakdown_%s.txt",
                 g_bench_dir, wlname);
        FILE *bf = fopen(path, "w");
        if (bf) {
            fprintf(bf,
                "# latency breakdown: mode=%s workload=%s\n"
                "# generated from ADAPTIPC_STATS=1 in-process counters\n"
                "workload=%s\n"
                "messages_received=%zu\n"
                "\n[receive-side multiplexer]\n"
                "delivered_shm_first_poll=%llu\n"
                "delivered_shm_after_timeout=%llu\n"
                "delivered_uds_prompt=%llu\n"
                "delivered_uds_after_timeout=%llu\n"
                "poll_calls=%llu\n"
                "poll_timeouts_2ms=%llu\n"
                "poll_wait_total_ns=%llu\n"
                "poll_wait_mean_ns=%llu\n"
                "recv_loop_iters=%llu\n"
                "\n[send-side]\n"
                "sends_total=%llu sends_shm=%llu sends_uds=%llu\n"
                "send_escalations_udsbeyond_dgram=%llu\n"
                "router_ewma_classify_total_ns=%llu\n"
                "router_ewma_classify_mean_ns=%llu\n"
                "send_alloc_total_ns=%llu send_alloc_mean_ns=%llu\n"
                "send_copy_total_ns=%llu send_copy_bytes=%llu\n",
                bmode_name(mode), wlname, wlname, s.n,
                (unsigned long long)st.rx_shm_first,
                (unsigned long long)st.rx_shm_later,
                (unsigned long long)st.rx_uds_prompt,
                (unsigned long long)st.rx_uds_later,
                (unsigned long long)st.poll_calls,
                (unsigned long long)st.poll_timeouts,
                (unsigned long long)st.poll_wait_ns,
                (unsigned long long)(st.poll_calls
                                     ? st.poll_wait_ns / st.poll_calls : 0),
                (unsigned long long)st.recv_loop_iters,
                (unsigned long long)st.sends,
                (unsigned long long)st.sends_shm,
                (unsigned long long)st.sends_uds,
                (unsigned long long)st.send_escalations,
                (unsigned long long)st.router_ns,
                (unsigned long long)(st.sends ? st.router_ns / st.sends : 0),
                (unsigned long long)st.send_alloc_ns,
                (unsigned long long)(st.sends_shm
                                     ? st.send_alloc_ns / st.sends_shm : 0),
                (unsigned long long)st.send_copy_ns,
                (unsigned long long)st.send_copy_bytes);
            fclose(bf);
            fprintf(stderr, "[breakdown] wrote %s\n", path);
        }
    }

    {
        double *tmp = malloc(s.n * sizeof(double));
        if (!tmp) die("malloc");
        memcpy(tmp, s.latency_ns, s.n * sizeof(double));
        double med = median(tmp, s.n);
        free(tmp);
        printf("[%s/%s] msgs=%zu wire=%.1f MB dur=%.3fs avg_tp=%.1f MB/s "
               "median_lat=%.0f ns route_switches=%lu%s\n",
               bmode_name(mode), wlname, s.n,
               (double)(wire_total) / 1e6, dur_s, s.tp_mbps, med, switches,
               (wl == WL_THRASH)
                   ? (switches == 0 ? " [HYSTERESIS OK]"
                                    : " [!! FALSE SWITCHES]")
                   : "");
    }
    return 0;
}


/* ------------------------------------------------------------------ */
/* Transport setup / teardown                                          */
/* ------------------------------------------------------------------ */

static int bench_setup(bench_ctx *c, bmode_t mode, adapt_role_t role)
{
    memset(c, 0, sizeof(*c));
    c->mode = mode;
    c->peer_sock = (role == ADAPT_ROLE_PRODUCER) ? SOCK_B : SOCK_A;

    switch (mode) {
    case BMODE_UDS: {
        const char *local = (role == ADAPT_ROLE_PRODUCER) ? SOCK_A : SOCK_B;
        return uds_open(local, &c->uds);
    }
    case BMODE_SHM:
        return shm_ring_create(SHM_NAME, SHM_CAP, 1,
                               role == ADAPT_ROLE_PRODUCER
                                   ? SHM_RING_ROLE_PRODUCER
                                   : SHM_RING_ROLE_CONSUMER,
                               &c->ring);
    case BMODE_ADAPT:
    default: {
        adapt_config_t cfg = {
            .shm_name     = SHM_NAME,
            .local_sock   = (role == ADAPT_ROLE_PRODUCER) ? SOCK_A : SOCK_B,
            .peer_sock    = c->peer_sock,
            .shm_capacity = SHM_CAP,
        };
        return adapt_init(role, &cfg, &c->adapt);
    }
    }
}

static void bench_teardown(bench_ctx *c, adapt_role_t role)
{
    if (c->uds) uds_close(c->uds);
    if (c->ring) shm_ring_close(c->ring, role == ADAPT_ROLE_PRODUCER);
    if (c->adapt) adapt_shutdown(c->adapt);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s --mode uds|shm|adapt "
            "--workload sweep|bimodal|thrash --out FILE.csv "
            "[--iters N]\n"
            "  --iters N : iterations for bimodal/thrash (default %lu; "
            "paper value 100000)\n",
            argv0, 100000UL);
}

int main(int argc, char **argv)
{
    bmode_t mode = BMODE_ADAPT;
    workload_t wl = WL_SWEEP;
    const char *out_path = NULL;
    unsigned long iters = 100000UL;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            ++i;
            if (!strcmp(argv[i], "uds"))        mode = BMODE_UDS;
            else if (!strcmp(argv[i], "shm"))   mode = BMODE_SHM;
            else if (!strcmp(argv[i], "adapt")) mode = BMODE_ADAPT;
            else { usage(argv[0]); return 2; }
        } else if (!strcmp(argv[i], "--workload") && i + 1 < argc) {
            ++i;
            if (!strcmp(argv[i], "sweep"))         wl = WL_SWEEP;
            else if (!strcmp(argv[i], "bimodal"))  wl = WL_BIMODAL;
            else if (!strcmp(argv[i], "thrash"))   wl = WL_THRASH;
            else { usage(argv[0]); return 2; }
        } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            out_path = argv[++i];
        } else if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
            iters = strtoul(argv[++i], NULL, 10);
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!out_path) { usage(argv[0]); return 2; }

    size_t total = 0;
    size_t *sizes = build_sizes(wl, iters, &total);

    /* Shared route array + handshake pipes survive fork(). */
    uint8_t *routes = mmap(NULL, total, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (routes == MAP_FAILED) die("mmap");
    memset(routes, 0, total);

    int ready_pipe[2], go_pipe[2];             /* child->parent, parent->child */
    if (pipe(ready_pipe) != 0 || pipe(go_pipe) != 0) die("pipe");

    /* Pre-clean stale IPC objects from previous runs. */
    shm_unlink(SHM_NAME);
    unlink(SOCK_A);
    unlink(SOCK_B);

    /* Extract output directory for breakdown files. */
    {
        const char *slash = strrchr(out_path, '/');
        if (slash) {
            size_t d = (size_t)(slash - out_path);
            if (d >= sizeof(g_bench_dir)) d = sizeof(g_bench_dir) - 1;
            memcpy(g_bench_dir, out_path, d);
            g_bench_dir[d] = '\0';
        } else {
            snprintf(g_bench_dir, sizeof(g_bench_dir), ".");
        }
    }

    FILE *csv = fopen(out_path, "a");
    if (!csv) die(out_path);
    /* write header only for a fresh file */
    {
        if (fseek(csv, 0, SEEK_END) != 0) die("fseek");
        if (ftell(csv) == 0) fprintf(csv, "%s\n", CSV_HEADER);
    }

    pid_t pid = fork();
    if (pid < 0) die("fork");

    if (pid == 0) {
        /* ---------------- child: producer ---------------- */
        close(ready_pipe[0]);
        close(go_pipe[1]);

        bench_ctx ctx;
        if (bench_setup(&ctx, mode, ADAPT_ROLE_PRODUCER) != 0)
            _exit(3);
        if (write(ready_pipe[1], "r", 1) != 1) _exit(4);
        char go;
        if (read(go_pipe[0], &go, 1) != 1) _exit(5);

        int rc = run_producer(&ctx, wl, sizes, total, routes);
        bench_teardown(&ctx, ADAPT_ROLE_PRODUCER);
        _exit(rc == 0 ? 0 : 1);
    }

    /* ---------------- parent: consumer + recorder ---------------- */
    close(ready_pipe[1]);
    close(go_pipe[0]);

    char rbuf;
    if (read(ready_pipe[0], &rbuf, 1) != 1) die("producer did not start");

    bench_ctx ctx;
    if (bench_setup(&ctx, mode, ADAPT_ROLE_CONSUMER) != 0)
        die("consumer transport setup");
    if (write(go_pipe[1], "g", 1) != 1) die("write go");

    double wall0 = now_ns();
    int rc = run_consumer(&ctx, wl, total, routes, csv, wl_name(wl), mode);
    double wall = now_ns() - wall0;
    bench_teardown(&ctx, ADAPT_ROLE_CONSUMER);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) rc = 1;

    fclose(csv);
    munmap(routes, total);
    free(sizes);

    /* Remove IPC objects we own (last one out cleans up). */
    shm_unlink(SHM_NAME);
    unlink(SOCK_A);
    unlink(SOCK_B);

    fprintf(stderr, "[timing] wall=%.3fs\n", wall / 1e9);
    return rc == 0 ? 0 : 1;
}

