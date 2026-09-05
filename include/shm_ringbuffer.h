#ifndef SHM_RINGBUFFER_H
#define SHM_RINGBUFFER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lock-free SPSC ring buffer over POSIX shared memory.
 *
 * Synchronization uses C11 <stdatomic.h> exclusively:
 *   - producer stores `head` with memory_order_release
 *   - consumer loads  `head` with memory_order_acquire
 *   - consumer stores `tail` with memory_order_release
 *   - producer loads  `tail` with memory_order_acquire
 * No locks are taken on the data path.
 */

/* Opaque handle. One per process; both point at the same shm object. */
typedef struct shm_ring shm_ring_t;

/* Per-record on-wire framing inside the ring (little-endian u32 length). */
#define SHM_RING_MSG_HDR_SIZE 4

/*
 * High/low watermark flow control. The producer blocks once ring usage
 * reaches the high watermark (80% of capacity) and is woken by the
 * consumer once usage drains below the low watermark (20%). The gap is
 * the hysteresis band that prevents wake storms.
 */
#define SHM_HW_PCT 80u
#define SHM_LW_PCT 20u

typedef enum {
    SHM_RING_ROLE_PRODUCER = 0,
    SHM_RING_ROLE_CONSUMER = 1
} shm_ring_role_t;

/*
 * Create (or attach to) a shared-memory ring buffer.
 *
 * name       : POSIX shm object name, e.g. "/adaptipc_shm_0"
 * capacity   : usable payload capacity in bytes (rounded up to power of two)
 * create     : nonzero -> create the shm object and initialize the header;
 *              zero     -> attach to an existing object created by the peer.
 * out        : receives the new handle.
 *
 * Returns 0 on success, -errno-style negative value on failure.
 */
int shm_ring_create(const char *name, size_t capacity, int create,
                    shm_ring_role_t role, shm_ring_t **out);

/*
 * Attach without waiting for the peer's header initialization (used by
 * lazy negotiation, where the ACK must be sent before either side can
 * touch the header). Producer role still initializes the header; consumer
 * role returns immediately -- subsequent shm_ring_pop() calls simply
 * observe an empty ring (head == tail == 0) until the producer's
 * release-published init becomes visible.
 */
int shm_ring_attach(const char *name, size_t capacity,
                    shm_ring_role_t role, shm_ring_t **out);

/* Producer: enqueue one record assembled from scattered segments.
 * Equivalent to pushing the concatenation of segs[0..nseg-1], but without
 * requiring the caller to build a contiguous buffer. Returns 0 on success,
 * -ENOSPC/EAGAIN semantics identical to shm_ring_push(). */
typedef struct shm_segment {
    const void *base;
    size_t      len;
} shm_segment_t;

int shm_ring_push_scatter(shm_ring_t *rb, const shm_segment_t *segs, int nseg);

/* Producer: enqueue one record. Returns 0 on success,
 * -ENOSPC if insufficient space is currently available (never blocks). */
int shm_ring_push(shm_ring_t *rb, const void *data, size_t size);

/* Consumer: dequeue one record into buf. Returns bytes copied (>0) on success,
 * -EAGAIN if empty (never blocks), -EINVAL/-EMSGSIZE on bad args. */
int shm_ring_pop(shm_ring_t *rb, void *buf, size_t max_size);

/* Non-blocking availability queries (approximate; lock-free snapshot). */
size_t shm_ring_used_bytes(const shm_ring_t *rb);
size_t shm_ring_free_bytes(const shm_ring_t *rb);

/*
 * Watermark flow control (producer side). Blocks the calling thread until
 * ring usage drops below the high watermark, using a futex on Linux
 * (FUTEX_WAIT on a shared atomic flag) or a process-shared mutex/condvar
 * elsewhere. The consumer wakes it automatically from shm_ring_pop() once
 * usage falls below the low watermark. Safe against lost wakeups: the
 * flag is re-checked after being set, before sleeping.
 * Returns 0 on success, negative errno otherwise.
 */
int shm_ring_wait_writable(shm_ring_t *rb);

/* Blocking producer push: throttles at the high watermark instead of
 * returning -EAGAIN. Never drops a message; returns 0 or negative errno. */
int shm_ring_push_scatter_blocking(shm_ring_t *rb, const shm_segment_t *segs,
                                   int nseg);

/* Detach this process' mapping. If `unlink_obj` is nonzero, also remove the
 * shm object (call once, by whoever created it, after the peer detached). */
void shm_ring_close(shm_ring_t *rb, int unlink_obj);

const char *shm_ring_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* SHM_RINGBUFFER_H */
