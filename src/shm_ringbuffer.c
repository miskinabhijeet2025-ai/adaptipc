/*
 * shm_ringbuffer.c -- lock-free SPSC ring buffer over POSIX shared memory.
 *
 * Concurrency protocol (single producer / single consumer):
 *   - `head` is written ONLY by the producer, read by the consumer.
 *     Producer publishes with release; consumer observes with acquire.
 *   - `tail` is written ONLY by the consumer, read by the producer.
 *     Consumer publishes with release; producer observes with acquire.
 * Records are framed as [u32 le length][payload]; a length of 0 is reserved
 * as a wrap sentinel and never stored as a real record.
 */
#define _POSIX_C_SOURCE 200809L

#include "shm_ringbuffer.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>

#include "futex_compat.h"

#define SHM_HDR_PAD    128u        /* header padding: avoids false sharing */
#define SHM_RING_MAGIC 0x52494752u /* "RIGR" */

typedef struct shm_ring_header {
    _Atomic uint64_t head;        /* bytes enqueued (producer cursor) */
    _Atomic uint64_t tail;        /* bytes dequeued (consumer cursor) */
    _Atomic uint64_t parks;       /* producer park count (watermark)  */
    uint64_t         capacity;    /* payload capacity in bytes        */
    uint32_t         magic;
    _Atomic uint32_t initialized; /* set with release once header ready */
    /* High/low watermark flow control. On Linux `flow_flag` is a
     * shared-memory futex word; on Apple/other Unix `flow` embeds the
     * same flag plus a process-shared mutex/condvar backing it. */
#if defined(__linux__)
    _Atomic int      flow_flag;
#else
    futex_compat_emu_t flow;
#endif
} shm_ring_header;

#if defined(__linux__)
static inline _Atomic int *flow_word(shm_ring_header *h)
{
    return &h->flow_flag;
}
static inline int flow_wait(shm_ring_header *h, int expected)
{
    long r = futex_compat_wait(flow_word(h), expected);
    if (r == 0) return 0;
    return (errno == EAGAIN) ? -EAGAIN : -errno;
}
static inline int flow_wake(shm_ring_header *h)
{
    if (!atomic_exchange_explicit(flow_word(h), 0, memory_order_acq_rel))
        return 0; /* nobody parked on it */
    futex_compat_wake(flow_word(h), INT_MAX);
    return 0;
}
#else
static inline _Atomic int *flow_word(shm_ring_header *h)
{
    return &h->flow.flag;
}
static inline int flow_wait(shm_ring_header *h, int expected)
{
    return futex_compat_emu_wait(&h->flow, expected);
}
static inline int flow_wake(shm_ring_header *h)
{
    return futex_compat_emu_wake(&h->flow);
}

/*
 * Emulated futex on Apple/other Unix: a process-shared mutex/condvar
 * embedded (in shared memory) next to the flag it backs. The flag check
 * is repeated under the mutex before sleeping, which closes the classic
 * lost-wakeup window between the waker's exchange() and its broadcast.
 */
int futex_compat_emu_init(futex_compat_emu_t *f)
{
    pthread_mutexattr_t ma;
    pthread_condattr_t  ca;
    int rc;
    if ((rc = pthread_mutexattr_init(&ma)) != 0) return -rc;
    if ((rc = pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED)) != 0) {
        pthread_mutexattr_destroy(&ma);
        return -rc;
    }
    rc = pthread_mutex_init(&f->mtx, &ma);
    pthread_mutexattr_destroy(&ma);
    if (rc != 0) return -rc;

    if ((rc = pthread_condattr_init(&ca)) != 0) {
        pthread_mutex_destroy(&f->mtx);
        return -rc;
    }
    if ((rc = pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED)) != 0) {
        pthread_condattr_destroy(&ca);
        pthread_mutex_destroy(&f->mtx);
        return -rc;
    }
    rc = pthread_cond_init(&f->cnd, &ca);
    pthread_condattr_destroy(&ca);
    if (rc != 0) {
        pthread_mutex_destroy(&f->mtx);
        return -rc;
    }
    atomic_store_explicit(&f->flag, 0, memory_order_relaxed);
    atomic_store_explicit(&f->ready, 1, memory_order_release);
    return 0;
}

int futex_compat_emu_wait(futex_compat_emu_t *f, int expected)
{
    if (!atomic_load_explicit(&f->ready, memory_order_acquire))
        return -EINVAL; /* not initialized yet: caller must not park */
    if (pthread_mutex_lock(&f->mtx) != 0) return -EINVAL;
    int rc = 0;
    if (atomic_load_explicit(&f->flag, memory_order_acquire) != expected) {
        rc = -EAGAIN; /* flag changed before we slept: recheck condition */
    } else if (pthread_cond_wait(&f->cnd, &f->mtx) != 0) {
        rc = -EINVAL;
    }
    pthread_mutex_unlock(&f->mtx);
    return rc;
}

int futex_compat_emu_wake(futex_compat_emu_t *f)
{
    if (!atomic_load_explicit(&f->ready, memory_order_acquire))
        return 0; /* producer never initialized the parking structures */
    if (!atomic_exchange_explicit(&f->flag, 0, memory_order_acq_rel))
        return 0; /* nobody parked on it */
    if (pthread_mutex_lock(&f->mtx) != 0) return -EINVAL;
    pthread_cond_broadcast(&f->cnd);
    pthread_mutex_unlock(&f->mtx);
    return 0;
}
#endif

struct shm_ring {
    shm_ring_header *hdr;
    unsigned char   *data;
    size_t           capacity;
    char             name[64];
    int              fd;
};

static inline size_t round_pow2(size_t v)
{
    size_t p = 4096; /* at least one page */
    while (p < v) p <<= 1;
    return p;
}

static int ring_map(const char *name, size_t capacity, int create,
                    shm_ring_role_t role, int wait_init, shm_ring_t **out)
{
    /* `create` is kept for API compatibility: both callers pass
     * create=1 and share one idempotent open (see NOTE above). */
    (void)create;
    if (!name || !out || name[0] != '/') return -EINVAL;

    size_t cap = round_pow2(capacity ? capacity : 65536);
    size_t map_size = sizeof(shm_ring_header) + SHM_HDR_PAD + cap;

    /* NOTE: no O_TRUNC. macOS returns EINVAL for shm_open(O_TRUNC) on
     * an existing object. A fresh logical ring does not need zeroed
     * data: the producer (re)initializes head/tail/initialized with
     * release semantics, and consumers only read bytes below head.
     * Undersized objects are grown by the fstat/ftruncate block below
     * (idempotently, by every attacher). */
    int oflags = O_RDWR | O_CREAT;

    int fd = shm_open(name, oflags, 0600);
    if (fd < 0) return -errno;

    /* Only grow the object if needed: some platforms (e.g. macOS) return
     * EINVAL from ftruncate() when the size already matches. */
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    if ((size_t)st.st_size < map_size &&
        ftruncate(fd, (off_t)map_size) != 0) {
        int e = errno;
        close(fd);
        return -e;
    }

    void *base = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        int e = errno;
        close(fd);
        return -e;
    }

    shm_ring_t *rb = calloc(1, sizeof(*rb));
    if (!rb) {
        munmap(base, map_size);
        close(fd);
        return -ENOMEM;
    }

    rb->hdr      = (shm_ring_header *)base;
    rb->data     = (unsigned char *)base + sizeof(shm_ring_header) + SHM_HDR_PAD;
    rb->capacity = cap;
    rb->fd       = fd;
    snprintf(rb->name, sizeof(rb->name), "%s", name);

    if (role == SHM_RING_ROLE_PRODUCER) {
        atomic_store_explicit(&rb->hdr->head, 0, memory_order_relaxed);
        atomic_store_explicit(&rb->hdr->tail, 0, memory_order_relaxed);
        atomic_store_explicit(&rb->hdr->parks, 0, memory_order_relaxed);
#if defined(__linux__)
        atomic_store_explicit(&rb->hdr->flow_flag, 0, memory_order_relaxed);
#else
        int fr = futex_compat_emu_init(&rb->hdr->flow);
        if (fr != 0) {
            munmap(base, map_size);
            close(fd);
            free(rb);
            return fr;
        }
#endif
        rb->hdr->capacity = cap;
        rb->hdr->magic = SHM_RING_MAGIC;
        atomic_store_explicit(&rb->hdr->initialized, 1, memory_order_release);
    } else {
        if (wait_init)
            while (!atomic_load_explicit(&rb->hdr->initialized,
                                         memory_order_acquire))
                sched_yield(); /* wait for producer-side header init */
    }

    *out = rb;
    return 0;
}

int shm_ring_create(const char *name, size_t capacity, int create,
                    shm_ring_role_t role, shm_ring_t **out)
{
    return ring_map(name, capacity, create, role, 1, out);
}

int shm_ring_attach(const char *name, size_t capacity,
                    shm_ring_role_t role, shm_ring_t **out)
{
    return ring_map(name, capacity, 0, role, 0, out);
}

/*
 * Byte-cursor accounting: head/tail are monotonic 64-bit cursors.
 * used = head - tail (wrapping arithmetic is exact). Push reserves
 * len(4) + payload; a wrap pads with a zero-length sentinel record.
 */
#define WRAP_SENTINEL_LEN 0u

int shm_ring_push(shm_ring_t *rb, const void *data, size_t size)
{
    if (!rb || !data || size == 0) return -EINVAL;
    if (size > rb->capacity - SHM_RING_MSG_HDR_SIZE) return -EMSGSIZE;

    const uint64_t need = (uint64_t)size + SHM_RING_MSG_HDR_SIZE;
    /* Own cursor: relaxed (only this thread writes it).
     * Peer cursor: acquire (pairs with peer's release-store). */
    const uint64_t head = atomic_load_explicit(&rb->hdr->head,
                                               memory_order_relaxed);
    const uint64_t tail = atomic_load_explicit(&rb->hdr->tail,
                                               memory_order_acquire);
    const uint64_t used = head - tail;

    const uint64_t pos        = head & (uint64_t)(rb->capacity - 1);
    const uint64_t contiguous = (uint64_t)rb->capacity - pos;
    const uint32_t len32      = (uint32_t)size;

    /* A wrap costs `contiguous` bytes of padding plus the record itself;
     * a straight write costs just the record. Reject if either doesn't fit
     * between tail and end-of-buffer envelope. */
    const int wrap = contiguous < need;
    const uint64_t required = wrap ? contiguous + need : need;
    if ((uint64_t)rb->capacity - used < required) return -EAGAIN;

    uint64_t new_head;
    if (contiguous < need) {
        /* Pad to end of buffer with a zero-length sentinel record that
         * tells the consumer to jump to offset 0, then write at the start.
         * The padding consumes cursor space too: round head up to the next
         * capacity multiple, then add the record length. */
        uint32_t zero = WRAP_SENTINEL_LEN;
        memcpy(rb->data + pos, &zero, sizeof(zero));
        memcpy(rb->data, &len32, sizeof(len32));
        memcpy(rb->data + SHM_RING_MSG_HDR_SIZE, data, size);
        new_head = (head & ~(uint64_t)(rb->capacity - 1)) +
                   (uint64_t)rb->capacity + need;
    } else {
        memcpy(rb->data + pos, &len32, sizeof(len32));
        memcpy(rb->data + pos + SHM_RING_MSG_HDR_SIZE, data, size);
        new_head = head + need;
    }

    /* Publish: payload writes above become visible before head advances. */
    atomic_store_explicit(&rb->hdr->head, new_head, memory_order_release);
    return 0;
}

/*
 * Scatter-gather push: assembles a record [u32 len][seg0][seg1]... directly
 * in the ring without requiring a contiguous staging buffer. Semantics
 * (space check, wrap sentinel, release/acquire publish) are identical to
 * shm_ring_push(); only the copy strategy differs.
 */
int shm_ring_push_scatter(shm_ring_t *rb, const shm_segment_t *segs,
                          int nseg)
{
    if (!rb || !segs || nseg <= 0) return -EINVAL;

    uint64_t body = 0;
    for (int k = 0; k < nseg; ++k) {
        if (!segs[k].base && segs[k].len) return -EINVAL;
        body += segs[k].len;
    }
    const uint64_t need = (uint64_t)body + SHM_RING_MSG_HDR_SIZE;
    if (body == 0 || need > rb->capacity - SHM_RING_MSG_HDR_SIZE)
        return -EMSGSIZE;

    /* Own cursor relaxed (sole writer); peer cursor acquire. */
    const uint64_t head = atomic_load_explicit(&rb->hdr->head,
                                               memory_order_relaxed);
    const uint64_t tail = atomic_load_explicit(&rb->hdr->tail,
                                               memory_order_acquire);
    const uint64_t used = head - tail;

    const uint64_t pos        = head & (uint64_t)(rb->capacity - 1);
    const uint64_t contiguous = (uint64_t)rb->capacity - pos;

    const int wrap = contiguous < need;
    const uint64_t required = wrap ? contiguous + need : need;
    if ((uint64_t)rb->capacity - used < required) return -EAGAIN;

    const uint32_t len32 = (uint32_t)body;
    size_t p;              /* physical write position */

    if (wrap) {
        /* Sentinel at end of buffer tells the consumer to jump to 0. */
        uint32_t zero = WRAP_SENTINEL_LEN;
        memcpy(rb->data + pos, &zero, sizeof(zero));
        p = 0;
    } else {
        p = (size_t)pos;
    }

    /* Record length header, then each segment, wrapping as needed. */
    memcpy(rb->data + p, &len32, sizeof(len32));
    p = (p + sizeof(len32)) % rb->capacity;

    for (int k = 0; k < nseg; ++k) {
        const unsigned char *s = segs[k].base;
        size_t len = segs[k].len;
        while (len > 0) {
            size_t chunk = rb->capacity - p;
            if (chunk > len) chunk = len;
            memcpy(rb->data + p, s, chunk);
            s += chunk;
            p = (p + chunk) % rb->capacity;
            len -= chunk;
        }
    }

    /* Publish: payload writes become visible before head advances. */
    atomic_store_explicit(&rb->hdr->head,
                          head + (wrap ? required : need),
                          memory_order_release);
    return 0;
}

int shm_ring_pop(shm_ring_t *rb, void *buf, size_t max_size)
{

    for (;;) {
        const uint64_t tail = atomic_load_explicit(&rb->hdr->tail,
                                                   memory_order_relaxed);
        /* Acquire pairs with producer's release-store of `head`: every byte
         * below head is fully written and visible to us. */
        const uint64_t head = atomic_load_explicit(&rb->hdr->head,
                                                   memory_order_acquire);
        if (head == tail) return -EAGAIN; /* empty */ 

        const uint64_t pos = tail & (uint64_t)(rb->capacity - 1);
        uint32_t len32;
        memcpy(&len32, rb->data + pos, sizeof(len32));

        if (len32 == WRAP_SENTINEL_LEN) {
            /* Wrap marker: advance consumer cursor to start of buffer. */
            const uint64_t new_tail =
                (tail & ~(uint64_t)(rb->capacity - 1)) +
                (uint64_t)rb->capacity;
            atomic_store_explicit(&rb->hdr->tail, new_tail,
                                  memory_order_release);
            continue; /* retry from offset 0 */
        }

        if (max_size < len32) return -EMSGSIZE;

        memcpy(buf, rb->data + pos + SHM_RING_MSG_HDR_SIZE, len32);
        atomic_store_explicit(&rb->hdr->tail,
                              tail + (uint64_t)len32 + SHM_RING_MSG_HDR_SIZE,
                              memory_order_release);
        /* Watermark wake: once drained below the low watermark, release
         * a parked producer (if any). exchange(flag,0)==1 means a producer
         * had committed to sleep, so no wakeup can be lost. */
        if (head - (tail + (uint64_t)len32 + SHM_RING_MSG_HDR_SIZE) <
            (uint64_t)rb->capacity * SHM_LW_PCT / 100u)
            flow_wake(rb->hdr);
        return (int)len32;
    }
}

/*
 * Producer-side watermark throttle. Blocks until used < HW_PCT% of
 * capacity. Lost-wakeup-safe: the flow flag is stored with release, the
 * usage condition re-checked with acquire, and the consumer only clears
 * the flag via an exchange before issuing the wake.
 */
int shm_ring_wait_writable(shm_ring_t *rb)
{
    if (!rb) return -EINVAL;

    const uint64_t hw_bytes =
        (uint64_t)rb->capacity * SHM_HW_PCT / 100u;

    for (;;) {
        const uint64_t head = atomic_load_explicit(&rb->hdr->head,
                                                   memory_order_relaxed);
        const uint64_t tail = atomic_load_explicit(&rb->hdr->tail,
                                                   memory_order_acquire);
        if (head - tail < hw_bytes) return 0;

        atomic_store_explicit(flow_word(rb->hdr), 1, memory_order_release);

        /* Re-check after arming the flag: the consumer may have drained
         * between our condition test and the store -- its exchange() would
         * have missed us, so we must not sleep now. */
        const uint64_t tail2 = atomic_load_explicit(&rb->hdr->tail,
                                                    memory_order_acquire);
        if (head - tail2 < hw_bytes) {
            atomic_store_explicit(flow_word(rb->hdr), 0,
                                  memory_order_release);
            return 0;
        }

        atomic_fetch_add_explicit(&rb->hdr->parks, 1,
                                  memory_order_relaxed);
        int rc = flow_wait(rb->hdr, 1);
        if (rc == -EAGAIN) continue; /* flag changed; recheck condition */
        if (rc != 0) return rc;      /* real error */
        /* woken: loop and re-verify the condition */
    }
}

int shm_ring_push_scatter_blocking(shm_ring_t *rb, const shm_segment_t *segs,
                                   int nseg)
{
    if (!rb) return -EINVAL;
    /* Throttle BEFORE attempting the push: parking only after a 100%
     * full-EAGAIN would let usage climb to capacity every cycle and
     * defeat the high-watermark queueing bound. */
    int rc = shm_ring_wait_writable(rb);
    if (rc != 0 && rc != -EINVAL) return rc;
    for (;;) {
        rc = shm_ring_push_scatter(rb, segs, nseg);
        if (rc != -EAGAIN) return rc;
        /* Full anyway (record vs. watermark rounding): park until the
         * consumer drains below HW, then retry. Yield prevents a spin
         * if a single record cannot fit even below HW. */
        rc = shm_ring_wait_writable(rb);
        if (rc != 0) return rc;
        sched_yield();
    }
}

size_t shm_ring_used_bytes(const shm_ring_t *rb)
{
    if (!rb) return 0;
    const uint64_t h = atomic_load_explicit(&rb->hdr->head,
                                            memory_order_acquire);
    const uint64_t t = atomic_load_explicit(&rb->hdr->tail,
                                            memory_order_acquire);
    return (size_t)(h - t);
}

uint64_t shm_ring_park_count(const shm_ring_t *rb)
{
    if (!rb) return 0;
    return atomic_load_explicit(&rb->hdr->parks, memory_order_relaxed);
}

size_t shm_ring_free_bytes(const shm_ring_t *rb)
{
    if (!rb) return 0;
    return rb->capacity - shm_ring_used_bytes(rb);
}

void shm_ring_close(shm_ring_t *rb, int unlink_obj)
{
    if (!rb) return;
    munmap(rb->hdr, sizeof(shm_ring_header) + SHM_HDR_PAD + rb->capacity);
    close(rb->fd);
    if (unlink_obj) shm_unlink(rb->name);
    free(rb);
}

const char *shm_ring_strerror(int rc)
{
    switch (rc) {
    case 0:         return "ok";
    case -EAGAIN:   return "ring empty or full";
    case -EMSGSIZE: return "message too large for ring";
    default:        return strerror(-rc);
    }
}

