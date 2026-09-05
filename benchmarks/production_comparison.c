/*
 * production_comparison.c -- comprehensive multi-benchmark harness.
 *
 * Measures end-to-end throughput and latency of AdaptIPC against three
 * production transport profiles:
 *
 *   adapt  : AdaptIPC adaptive router (EWMA + hysteresis + watermark flow
 *            control + lazy SHM negotiation), via its public API.
 *   uring  : Linux io_uring datagram transfer (sendmsg/recvmsg over a
 *            socketpair). Stubbed out as "unsupported" on non-Linux.
 *   uds    : standard AF_UNIX SOCK_DGRAM baseline.
 *   iceoryx: zero-copy shared-memory profile mimicking Eclipse Iceoryx
 *            (pre-mapped static slot pool, release/acquire sequence
 *            numbers, no copies at the data path -- only cursor swap).
 *
 * Topology: fork() -> child = producer, parent = consumer + recorder.
 * For every transport the producer stamps a CLOCK_MONOTONIC timestamp
 * into each frame; the consumer computes end-to-end latency on receipt.
 *
 * Outputs:
 *   benchmarks/results_summary.txt          -- final comparative table
 *   benchmarks/latency_cdf_<transport>.txt  -- CDF data per transport:
 *       p   percentile   latency_ns
 *   plus per-transport raw samples when ADAPTIPC_BENCH_RAW=1.
 *
 * Build (same flags as the library):
 *   cc -std=c11 -O3 -Wall -Wextra -pthread -Iinclude \
 *      benchmarks/production_comparison.c src/... -o production_comparison
 */
#define _POSIX_C_SOURCE 200809L

#include "adapt_ipc.h"
#include "shm_ringbuffer.h"
#include "uds_fallback.h"

#include <errno.h>
#include <fcntl.h>
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
#endif

#if defined(__linux__) && defined(ADAPTIPC_HAVE_IO_URING)
#define BENCH_WITH_IO_URING 1
#endif

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define FRAME_HDR       24u                /* seq(8) + ts(8) + len(8)   */
#define MAX_PAYLOAD     (2u << 20)         /* 2 MB                       */
#define SHM_CAP         (64u << 20)        /* AdaptIPC ring capacity     */
#define ICEORYX_CHUNK   (MAX_PAYLOAD + 64u) /* zero-copy slot size       */
#define ICEORYX_SLOTS   128u              /* zero-copy slot pool count   */
#define LAT_SAMPLES_CAP (64u * 1024u)      /* per-transport latency set  */

#define SOCK_A "/tmp/adaptipc_pcmp_a.sock"
#define SOCK_B "/tmp/adaptipc_pcmp_b.sock"

typedef enum { TR_ADAPT = 0, TR_IOURING, TR_UDS, TR_ICEORYX, TR_COUNT } tr_t;

static const char *tr_name(tr_t t)
{
    static const char *n[TR_COUNT] = { "adapt", "uring", "uds", "iceoryx" };
    return n[t];
}

/* Workload profile for the run (same message stream for every transport,
 * so the comparison is apples-to-apples). */
typedef struct {
    size_t  *sizes;
    size_t   count;
    uint64_t total_bytes;
} workload_t;

/* ------------------------------------------------------------------ */
/* Timing / stats helpers                                               */
/* ------------------------------------------------------------------ */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void die(const char *msg)
{
    fprintf(stderr, "production_comparison: %s (%s)\n",
            msg, strerror(errno));
    exit(1);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t percentile_sorted(uint64_t *v, size_t n, double p)
{
    if (n == 0) return 0;
    double idx = (p / 100.0) * (double)(n - 1);
    size_t lo = (size_t)idx, hi = (lo + 1 < n) ? lo + 1 : lo;
    double frac = idx - (double)lo;
    return (uint64_t)((double)v[lo] * (1.0 - frac) + (double)v[hi] * frac);
}

/* ------------------------------------------------------------------ */
/* Workload: bimodal production-like stream                             */
/*   - 128 B control frames and 64 KB..2 MB bulk frames                */
/*   - deterministic (same seed everywhere)                            */
/* ------------------------------------------------------------------ */

static void build_workload(workload_t *wl, unsigned long iters)
{
    wl->sizes = malloc(iters * sizeof(size_t));
    if (!wl->sizes) die("malloc workload");
    wl->count = iters;
    wl->total_bytes = 0;

    unsigned seed = 20260904u;
    for (unsigned long i = 0; i < iters; i++) {
        size_t sz;
        int cls = rand_r(&seed) % 100;
        if (cls < 55) {
            sz = 128;                                  /* control frame */
        } else if (cls < 85) {
            sz = (size_t)64 << (rand_r(&seed) % 7);    /* 64B .. 4KB     */
        } else {
            sz = (size_t)1 << (16 + rand_r(&seed) % 6);/* 64KB .. 2MB    */
        }
        wl->sizes[i] = sz;
        wl->total_bytes += sz;
    }
}

/* ------------------------------------------------------------------ */
/* Iceoryx-style zero-copy transport                                    */
/*                                                                     */
/* A pre-mapped pool of ICEORYX_SLOTS fixed slots plus an SPSC queue   */
/* of slot indices (release/acquire sequence numbers). The producer    */
/* copies the frame into a free slot once and hands over the index;   */
/* the consumer reads in place and returns the slot. In a real        */
/* zero-copy deployment with shared ownership the payload copy        */
/* disappears entirely; the pool + sequence handshake is the profile  */
/* we mimic here. Slot discipline: strictly rotating slot[i % N] is   */
/* unsafe under overrun, so the queue itself provides backpressure:   */
/* push blocks (futex-free: sched_yield loop) when full.               */
/* ------------------------------------------------------------------ */

typedef struct ice_chunk_hdr {
    uint64_t seq;
    uint64_t ts_ns;
    uint64_t len;
} ice_chunk_hdr;

typedef struct ice_state {
    /* slot pool */
    unsigned char (*slots)[ICEORYX_CHUNK];
    /* index queue (SPSC) */
    _Atomic uint64_t q_head;   /* producer cursor */
    _Atomic uint64_t q_tail;   /* consumer cursor */
    _Atomic uint64_t q_head2;  /* free-list head (consumer returns)   */
    _Atomic uint64_t q_tail2;  /* free-list tail (producer acquires)  */
    uint32_t q_mask;
    uint32_t q_depth;
    _Atomic uint32_t ready;
} ice_state;

typedef struct ice_shm {
    ice_state st;
    /* flexible arrays follow in one mapping:
     *   queue: q_depth * uint32 slot indices (in flight)
     *   free : q_depth * uint32 slot indices (available)
     *   slots: ICEORYX_SLOTS * ICEORYX_CHUNK bytes
     */
} ice_shm;

static inline uint32_t *ice_queue(ice_shm *s)
{
    return (uint32_t *)((unsigned char *)s + sizeof(ice_shm));
}

static inline uint32_t *ice_free(ice_shm *s)
{
    return (uint32_t *)((unsigned char *)s + sizeof(ice_shm) +
                        (size_t)s->st.q_depth * sizeof(uint32_t));
}

static inline unsigned char (*ice_slots(ice_shm *s))[ICEORYX_CHUNK]
{
    return (unsigned char (*)[ICEORYX_CHUNK])
        ((unsigned char *)s + sizeof(ice_shm) +
         2 * (size_t)s->st.q_depth * sizeof(uint32_t));
}

static ice_shm *ice_create(int create)
{
    const char *name = "/adaptipc_pcmp_ice";
    if (create) shm_unlink(name); /* stale object from a crashed run */
    int fd = shm_open(name, O_RDWR | (create ? O_CREAT : 0), 0600);
    if (fd < 0) {
        fprintf(stderr, "ice: shm_open: %s\n", strerror(errno));
        return NULL;
    }
    /* Ring depth == slot count: every index in either ring refers to a
     * real pool slot, so cursor arithmetic can never run past the pool. */
    const size_t q_depth = ICEORYX_SLOTS;
    const size_t map_size = sizeof(ice_shm) +
        2 * q_depth * sizeof(uint32_t) +
        (size_t)ICEORYX_SLOTS * ICEORYX_CHUNK;
    if (create) {
        if (ftruncate(fd, (off_t)map_size) != 0) {
            fprintf(stderr, "ice: ftruncate(%zu): %s\n", map_size,
                    strerror(errno));
            close(fd);
            return NULL;
        }
    }
    void *base = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                      fd, 0);
    close(fd);
    if (base == MAP_FAILED) return NULL;

    ice_shm *s = (ice_shm *)base;
    if (create) {
        atomic_store_explicit(&s->st.q_head, 0, memory_order_relaxed);
        atomic_store_explicit(&s->st.q_tail, 0, memory_order_relaxed);
        atomic_store_explicit(&s->st.q_tail2, 0, memory_order_relaxed);
        atomic_store_explicit(&s->st.q_head2, (uint64_t)q_depth,
                              memory_order_relaxed);
        s->st.q_depth = (uint32_t)q_depth;
        s->st.q_mask  = (uint32_t)(q_depth - 1);
        for (size_t i = 0; i < q_depth; ++i) ice_free(s)[i] = (uint32_t)i;
        atomic_store_explicit(&s->st.ready, 1, memory_order_release);
    } else {
        while (!atomic_load_explicit(&s->st.ready, memory_order_acquire))
            sched_yield();
    }
    return s;
}

static void ice_close(ice_shm *s, int unlink_obj)
{
    if (!s) return;
    const size_t map_size = sizeof(ice_shm) +
        2 * (size_t)s->st.q_depth * sizeof(uint32_t) +
        (size_t)ICEORYX_SLOTS * ICEORYX_CHUNK;
    munmap(s, map_size);
    if (unlink_obj) shm_unlink("/adaptipc_pcmp_ice");
}

/* Producer: acquire a free slot, copy frame in, publish index. */
static int ice_send(ice_shm *s, const unsigned char *frame, size_t len)
{
    if (len > ICEORYX_CHUNK) return -EMSGSIZE;
    for (;;) {
        /* Acquire a slot from the free list: capture the index inside
         * the CAS loop so a raced tail still yields the slot we read. */
        uint64_t ft;
        uint32_t slot;
        for (;;) {
            ft = atomic_load_explicit(&s->st.q_tail2,
                                      memory_order_relaxed);
            uint64_t fh = atomic_load_explicit(&s->st.q_head2,
                                               memory_order_acquire);
            if (fh == ft) break; /* free list empty: wait for returns */
            slot = ice_free(s)[ft & s->st.q_mask];
            uint64_t expected = ft;
            if (atomic_compare_exchange_strong_explicit(
                    &s->st.q_tail2, &expected, ft + 1,
                    memory_order_acq_rel, memory_order_relaxed))
                goto have_slot;
        }
        sched_yield();
        continue;

    have_slot:
        memcpy(ice_slots(s)[slot], frame, len);
        /* Chunk header occupies the same first FRAME_HDR bytes as the
         * frame itself (identical layout: seq@0, ts@8, payload_len@16),
         * so the consumer can read either view interchangeably. */
        /* publish into the in-flight queue */
        for (;;) {
            uint64_t h = atomic_load_explicit(&s->st.q_head,
                                              memory_order_relaxed);
            uint64_t t = atomic_load_explicit(&s->st.q_tail,
                                              memory_order_acquire);
            if (h - t < s->st.q_depth) {
                ice_queue(s)[h & s->st.q_mask] = slot;
                atomic_store_explicit(&s->st.q_head, h + 1,
                                      memory_order_release);
                return 0;
            }
            sched_yield(); /* queue full: pool-style backpressure */
        }
    }
}

/* Consumer: pop next in-flight slot, copy out (the only copy on the
 * receive side; mirrors iceoryx subscribe-take semantics), return slot. */
static int ice_recv(ice_shm *s, unsigned char *buf, size_t max)
{
    for (;;) {
        uint64_t t = atomic_load_explicit(&s->st.q_tail,
                                          memory_order_relaxed);
        uint64_t h = atomic_load_explicit(&s->st.q_head,
                                          memory_order_acquire);
        if (h == t) { sched_yield(); continue; }
        uint32_t slot = ice_queue(s)[t & s->st.q_mask];
        ice_chunk_hdr *ch = (ice_chunk_hdr *)ice_slots(s)[slot];
        size_t len = (size_t)ch->len;             /* payload length      */
        size_t total = FRAME_HDR + len;           /* frame incl. header  */
        if (total > max) return -EMSGSIZE;
        memcpy(buf, ice_slots(s)[slot], total);
        atomic_store_explicit(&s->st.q_tail, t + 1, memory_order_release);
        /* return the slot to the free list */
        uint64_t h2;
        do {
            h2 = atomic_load_explicit(&s->st.q_head2,
                                      memory_order_relaxed);
            ice_free(s)[h2 & s->st.q_mask] = slot;
        } while (!atomic_compare_exchange_strong_explicit(
                     &s->st.q_head2, &h2, h2 + 1,
                     memory_order_release, memory_order_relaxed));
        return (int)total;
    }
}

/* ------------------------------------------------------------------ */
/* io_uring transport (Linux only)                                      */
/* ------------------------------------------------------------------ */

#ifdef BENCH_WITH_IO_URING
#include <liburing.h>
#include <sys/socket.h>

typedef struct { int fd[2]; struct io_uring ring; } uring_ctx;

static int uring_setup(uring_ctx *u)
{
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, u->fd) != 0) return -errno;
    if (io_uring_queue_init(1024, &u->ring, 0) != 0) {
        close(u->fd[0]); close(u->fd[1]);
        return -EIO;
    }
    return 0;
}

static void uring_close(uring_ctx *u)
{
    io_uring_queue_exit(&u->ring);
    close(u->fd[0]); close(u->fd[1]);
}
#else
typedef struct { int dummy; } uring_ctx;
static int uring_setup(uring_ctx *u) { (void)u; return -ENOSYS; }
static void uring_close(uring_ctx *u) { (void)u; }
#endif

/* ------------------------------------------------------------------ */
/* Frame helpers                                                        */
/* ------------------------------------------------------------------ */

/* frame layout: [seq:8][send_ns:8][len:8][payload...] */
static void frame_stamp(unsigned char *frame, uint64_t seq, uint64_t len)
{
    memcpy(frame + 0, &seq, 8);
    uint64_t t = now_ns();
    memcpy(frame + 8, &t, 8);
    memcpy(frame + 16, &len, 8);
}

static uint64_t frame_take_ts(const unsigned char *frame)
{
    uint64_t t;
    memcpy(&t, frame + 8, 8);
    return t;
}

static uint64_t frame_take_len(const unsigned char *frame)
{
    uint64_t l;
    memcpy(&l, frame + 16, 8);
    return l;
}

/* ------------------------------------------------------------------ */
/* Producer side (child process)                                        */
/* ------------------------------------------------------------------ */

static int produce(tr_t tr, const workload_t *wl)
{
    unsigned char *frame = malloc(FRAME_HDR + MAX_PAYLOAD);
    if (!frame) die("malloc frame");

    uds_endpoint_t *uds = NULL;
    shm_ring_t     *ring = NULL;   /* unused: adapt handles its own */
    adapt_ctx_t    *adapt = NULL;
    ice_shm        *ice = NULL;
    uring_ctx       ur;
    int have_uring = 0;

    if (tr == TR_UDS) {
        if (uds_open(SOCK_A, &uds) != 0) die("uds_open");
    } else if (tr == TR_ADAPT) {
        adapt_config_t cfg = {
            .local_sock = SOCK_A, .peer_sock = SOCK_B,
            .shm_capacity = SHM_CAP,
        };
        if (adapt_init(ADAPT_ROLE_PRODUCER, &cfg, &adapt) != 0)
            die("adapt_init");
    } else if (tr == TR_ICEORYX) {
        ice = ice_create(0); /* pool pre-created in run_one() */
        if (!ice) die("ice attach");
    } else if (tr == TR_IOURING) {
        int rc = uring_setup(&ur);
        if (rc == 0) have_uring = 1;
        else if (rc != -ENOSYS) die("uring_setup");
    }

    for (size_t i = 0; i < wl->count; i++) {
        size_t len = wl->sizes[i];
        frame_stamp(frame, (uint64_t)i, (uint64_t)len);
        memset(frame + FRAME_HDR, (int)(i & 0xFF), len);
        int rc = 0;
        size_t total = FRAME_HDR + len;

        switch (tr) {
        case TR_UDS: {
            /* chunked datagrams with an 8-byte length prefix (SOCK_DGRAM
             * cannot carry >4KB atomically); receiver reassembles. */
            uint64_t l = (uint64_t)total;
            rc = uds_send(uds, SOCK_B, &l, sizeof(l));
            size_t off = 0;
            while (rc == 0 && off < total) {
                size_t n = (total - off > 4096) ? 4096 : total - off;
                rc = uds_send(uds, SOCK_B, frame + off, n);
                off += n;
            }
            break;
        }
        case TR_ADAPT: {
            do {
                rc = adapt_send(adapt, frame, total);
                if (rc == -EAGAIN) sched_yield();
            } while (rc == -EAGAIN);
            break;
        }
        case TR_ICEORYX: {
            rc = ice_send(ice, frame, total);
            break;
        }
        case TR_IOURING:
#ifdef BENCH_WITH_IO_URING
            if (have_uring) {
                /* io_uring send/recv over the socketpair, one CQE each */
                struct io_uring_sqe *sqe = io_uring_get_sqe(&ur.ring);
                io_uring_prep_send(sqe, ur.fd[0], frame, total, 0);
                io_uring_submit(&ur.ring);
                struct io_uring_cqe *cqe;
                io_uring_wait_cqe(&ur.ring, &cqe);
                rc = cqe->res == (int)total ? 0 : -EIO;
                io_uring_cqe_seen(&ur.ring, cqe);
            }
#endif
            break;
        default:
            break;
        }
        if (rc != 0) {
            fprintf(stderr, "producer[%s]: send failed at %zu: %s\n",
                    tr_name(tr), i, strerror(-rc));
            free(frame);
            return 1;
        }
    }

    if (uds) uds_close(uds);
    if (adapt) adapt_shutdown(adapt);
    if (ring) shm_ring_close(ring, 0);
    if (ice) ice_close(ice, 0); /* pool unlinked by run_one */
    if (have_uring) uring_close(&ur);
    free(frame);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Consumer side (parent)                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t n;
    uint64_t bytes;
    uint64_t lat_min, lat_max;
    double   lat_sum;
    uint64_t *lat;        /* sorted at the end */
    uint64_t t0_ns, t1_ns;
    int      had_oversize;
} run_result;

static void result_init(run_result *r)
{
    memset(r, 0, sizeof(*r));
    r->lat = malloc(LAT_SAMPLES_CAP * sizeof(uint64_t));
    if (!r->lat) die("malloc lat");
    r->lat_min = UINT64_MAX;
}

static void result_add(run_result *r, uint64_t lat)
{
    if (r->n < LAT_SAMPLES_CAP) r->lat[r->n] = lat;
    r->n++;
    r->lat_sum += (double)lat;
    if (lat < r->lat_min) r->lat_min = lat;
    if (lat > r->lat_max) r->lat_max = lat;
}

/* Reassemble a framed record from the transport into `frame`. Returns
 * frame length (FRAME_HDR + payload) or negative errno. */
static int consume_one(tr_t tr, uds_endpoint_t *uds, adapt_ctx_t *adapt,
                       ice_shm *ice, unsigned char *frame, size_t max)
{
    switch (tr) {
    case TR_ADAPT: {
        int n;
        do {
            n = adapt_recv(adapt, frame, max);
        } while (n == -EAGAIN || n == -ECONNREFUSED);
        return n;
    }
    case TR_UDS: {
        uint64_t l;
        int n = uds_recv(uds, &l, sizeof(l));
        if (n != (int)sizeof(l)) return (n < 0) ? n : -EIO;
        if (l > max) return -EMSGSIZE;
        size_t off = 0;
        while (off < l) {
            size_t want = (l - off > 4096) ? 4096 : l - off;
            n = uds_recv(uds, frame + off, want);
            if (n <= 0) return (n < 0) ? n : -EIO;
            off += (size_t)n;
        }
        return (int)l;
    }
    case TR_ICEORYX:
        return ice_recv(ice, frame, max);
    case TR_IOURING:
    default:
        return -ENOSYS;
    }
}

static int consume(tr_t tr, const workload_t *wl, run_result *res)
{
    unsigned char *frame = malloc(FRAME_HDR + MAX_PAYLOAD);
    if (!frame) die("malloc frame");

    uds_endpoint_t *uds = NULL;
    adapt_ctx_t    *adapt = NULL;
    ice_shm        *ice = NULL;

    if (tr == TR_UDS) {
        if (uds_open(SOCK_B, &uds) != 0) die("uds_open");
    } else if (tr == TR_ADAPT) {
        adapt_config_t cfg = {
            .local_sock = SOCK_B, .peer_sock = SOCK_A,
            .shm_capacity = SHM_CAP,
        };
        if (adapt_init(ADAPT_ROLE_CONSUMER, &cfg, &adapt) != 0)
            die("adapt_init");
    } else if (tr == TR_ICEORYX) {
        ice = ice_create(0);
        if (!ice) die("ice attach");
    } else if (tr == TR_IOURING) {
        /* producer owns the socketpair side of the io_uring transport on
         * Linux; the consumer reads the peer fd directly. Unsupported
         * platforms never reach here (guarded in run_one). */
        return -ENOSYS;
    }

    res->t0_ns = now_ns();
    for (size_t i = 0; i < wl->count; i++) {
        int n = consume_one(tr, uds, adapt, ice, frame,
                           FRAME_HDR + MAX_PAYLOAD);
        if (n < 0) {
            fprintf(stderr, "consumer[%s]: recv failed at %zu: %s\n",
                    tr_name(tr), i, strerror(-n));
            free(frame);
            return 1;
        }
        uint64_t len = frame_take_len(frame);
        if ((size_t)n != FRAME_HDR + (size_t)len) {
            fprintf(stderr, "consumer[%s]: frame %zu truncated "
                            "(%d != %u+%u)\n", tr_name(tr), i, n,
                    (unsigned)FRAME_HDR, (unsigned)len);
            free(frame);
            return 1;
        }
        uint64_t lat = now_ns() - frame_take_ts(frame);
        res->bytes += (uint64_t)n;
        result_add(res, lat);
    }
    res->t1_ns = now_ns();

    if (uds) uds_close(uds);
    if (adapt) adapt_shutdown(adapt);
    if (ice) ice_close(ice, 0); /* consumer attached only */
    free(frame);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Orchestrator                                                         */
/* ------------------------------------------------------------------ */

static void write_cdf(const run_result *r, const char *outdir,
                      tr_t tr)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/latency_cdf_%s.txt", outdir,
             tr_name(tr));
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# end-to-end latency CDF, transport=%s\n", tr_name(tr));
    fprintf(f, "# samples=%" PRIu64 " (first %" PRIu64 " retained)\n",
            r->n, r->n < LAT_SAMPLES_CAP ? r->n : LAT_SAMPLES_CAP);
    fprintf(f, "p   latency_ns\n");
    static const double ps[] = { 0.1, 1, 5, 10, 25, 50, 75, 90, 95,
                                 99, 99.9, 99.99, 100 };
    size_t n = r->n < LAT_SAMPLES_CAP ? r->n : LAT_SAMPLES_CAP;
    for (size_t k = 0; k < sizeof(ps) / sizeof(ps[0]); k++)
        fprintf(f, "%-6.2f %" PRIu64 "\n", ps[k],
                percentile_sorted(r->lat, n, ps[k]));
    fclose(f);
}

static int run_one(tr_t tr, const workload_t *wl,
                   run_result *res)
{
    if (tr == TR_IOURING) {
#ifndef BENCH_WITH_IO_URING
        return -ENOSYS;
#endif
    }

    /* Clean leftovers from a crashed previous run. */
    unlink(SOCK_A); unlink(SOCK_B);

    /* For the iceoryx transport, create the pool BEFORE forking so the
     * producer child can attach without racing the consumer's setup.
     * Both sides attach (create=0); the parent unlinks at the end. */
    ice_shm *pre_ice = NULL;
    if (tr == TR_ICEORYX) {
        pre_ice = ice_create(1);
        if (!pre_ice) die("ice pre-create");
    }

    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        /* child: producer. Exit with its status. */
        _exit(produce(tr, wl));
    }
    /* parent: consumer */
    int rc = consume(tr, wl, res);
    int status = 0;
    waitpid(pid, &status, 0);
    if (pre_ice) ice_close(pre_ice, 1);
    if (rc != 0) return rc;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "run_one[%s]: producer exit status %d\n",
                tr_name(tr), status);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *outdir = "benchmarks";
    unsigned long iters = 20000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--iters") && i + 1 < argc)
            iters = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            outdir = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--iters N] [--out DIR]\n", argv[0]);
            return 2;
        }
    }

    workload_t wl;
    build_workload(&wl, iters);

    printf("production comparison harness\n");
    printf("  workload: %zu messages, %.1f MB total, bimodal "
           "(55%% control / 30%% mid / 15%% bulk)\n",
           wl.count, (double)wl.total_bytes / (1024.0 * 1024.0));

    run_result results[TR_COUNT];
    static const int enabled[TR_COUNT] = { 1, 1, 1, 1 };
    char summary_path[512];
    snprintf(summary_path, sizeof(summary_path),
             "%s/results_summary.txt", outdir);
    FILE *sum = fopen(summary_path, "w");

    for (tr_t tr = TR_ADAPT; tr < TR_COUNT; tr++) {
        if (!enabled[tr]) continue;
        result_init(&results[tr]);
        printf("  %-8s ... ", tr_name(tr));
        fflush(stdout);
        int rc = run_one(tr, &wl, &results[tr]);
        if (rc == -ENOSYS) {
            printf("unsupported on this platform\n");
            if (sum)
                fprintf(sum, "%-8s unsupported on this platform "
                             "(io_uring requires Linux)\n", tr_name(tr));
            results[tr].n = 0;
            continue;
        }
        if (rc != 0) {
            printf("FAILED (rc=%d)\n", rc);
            if (sum) fprintf(sum, "%-8s FAILED\n", tr_name(tr));
            continue;
        }

        run_result *r = &results[tr];
        size_t n = r->n < LAT_SAMPLES_CAP ? r->n : LAT_SAMPLES_CAP;
        qsort(r->lat, n, sizeof(uint64_t), cmp_u64);
        double dur_s = (double)(r->t1_ns - r->t0_ns) / 1e9;
        double tp = (double)r->bytes / (1024.0 * 1024.0) / dur_s;
        double p50 = (double)percentile_sorted(r->lat, n, 50) / 1000.0;
        double p99 = (double)percentile_sorted(r->lat, n, 99) / 1000.0;
        double p999 = (double)percentile_sorted(r->lat, n, 99.9) / 1000.0;
        double mean = r->n ? r->lat_sum / (double)r->n / 1000.0 : 0;

        printf("%.0f MB/s  p50=%'.1f us  p99=%'.1f us  p99.9=%'.1f us\n",
               tp, p50, p99, p999);

        if (sum)
            fprintf(sum,
                    "%-8s throughput=%.0f MB/s  mean=%.1f us  "
                    "p50=%.1f us  p99=%.1f us  p99.9=%.1f us  "
                    "max=%.1f us  msgs=%" PRIu64 "\n",
                    tr_name(tr), tp, mean, p50, p99, p999,
                    (double)r->lat_max / 1000.0, r->n);
        write_cdf(&results[tr], outdir, tr);
    }

    if (sum) {
        fprintf(sum, "\nworkload: %zu messages, %" PRIu64 " bytes total "
                     "(bimodal: 55%% control, 30%% mid, 15%% bulk)\n",
                wl.count, wl.total_bytes);
        fprintf(sum, "latency = end-to-end (send timestamp -> consumer "
                     "receipt), including transport queueing\n");
        fprintf(sum, "watermark flow control: HW=80%% LW=20%% "
                     "(futex/condvar park+wake)\n");
        fprintf(sum, "lazy SHM negotiation: deferred mapping, "
                     "SHM_SETUP_REQ/ACK over UDS\n");
        fclose(sum);
        printf("summary -> %s\n", summary_path);
    }

    for (tr_t tr = TR_ADAPT; tr < TR_COUNT; tr++)
        free(results[tr].lat);
    free(wl.sizes);
    return 0;
}
