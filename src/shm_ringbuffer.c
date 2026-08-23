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
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>

#define SHM_HDR_PAD    128u        /* header padding: avoids false sharing */
#define SHM_RING_MAGIC 0x52494752u /* "RIGR" */

typedef struct shm_ring_header {
    _Atomic uint64_t head;        /* bytes enqueued (producer cursor) */
    _Atomic uint64_t tail;        /* bytes dequeued (consumer cursor) */
    uint64_t         capacity;    /* payload capacity in bytes        */
    uint32_t         magic;
    _Atomic uint32_t initialized; /* set with release once header ready */
} shm_ring_header;

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

int shm_ring_create(const char *name, size_t capacity, int create,
                    shm_ring_role_t role, shm_ring_t **out)
{
    if (!name || !out || name[0] != '/') return -EINVAL;

    size_t cap = round_pow2(capacity ? capacity : 65536);
    size_t map_size = sizeof(shm_ring_header) + SHM_HDR_PAD + cap;

    int oflags = O_RDWR | O_CREAT;
    if (create && role == SHM_RING_ROLE_PRODUCER) oflags |= O_TRUNC;

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
        rb->hdr->capacity = cap;
        rb->hdr->magic = SHM_RING_MAGIC;
        atomic_store_explicit(&rb->hdr->initialized, 1, memory_order_release);
    } else {
        while (!atomic_load_explicit(&rb->hdr->initialized,
                                     memory_order_acquire))
            sched_yield(); /* wait for producer-side header init */
    }

    *out = rb;
    return 0;
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
        return (int)len32;
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

