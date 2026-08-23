#ifndef ADAPT_IPC_H
#define ADAPT_IPC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * AdaptIPC -- adaptive IPC routing middleware.
 *
 * Every message is classified through an EWMA of its payload size:
 *
 *     ewma_size = 0.2 * current_size + 0.8 * ewma_size
 *
 * Routing decision (with hysteresis):
 *     ewma >= TAU_HIGH (4096)  -> SHM lock-free ring buffer
 *     ewma <= TAU_LOW  (1024)  -> UDS datagram fallback
 *     in between               -> keep last route (sticky hysteresis band)
 *
 * A one-byte transport tag prefixes every message on the wire so that
 * adapt_recv() demultiplexes correctly even when sender and receiver pick
 * different routes for consecutive messages.
 */

#define ADAPT_TAU_LOW    ((double)1024.0)
#define ADAPT_TAU_HIGH   ((double)4096.0)
#define ADAPT_EWMA_ALPHA ((double)0.2)

#define ADAPT_SHM_NAME_DEFAULT "/adaptipc_shm_0"

/* Bounded UDS wait inside adapt_recv() so SHM arrivals are seen promptly
 * while the receiver is nominally blocked on the socket. */
#define ADAPT_UDS_POLL_MS 2

/* Optional instrumentation. Set environment variable ADAPTIPC_STATS=1
 * before adapt_init() to enable per-context accounting (negligible cost
 * when disabled). */
typedef struct adapt_ctx adapt_ctx_t;

typedef struct adapt_stats {
    /* send path */
    uint64_t sends, sends_shm, sends_uds, send_escalations;
    uint64_t router_ns;        /* EWMA update + classify, cumulative   */
    uint64_t send_alloc_ns;    /* malloc in SHM branch                */
    uint64_t send_copy_ns;     /* tag/payload copies, both branches   */
    uint64_t send_copy_bytes;
    /* receive path */
    uint64_t recv_msgs;
    uint64_t rx_shm_first;     /* delivered by initial ring poll      */
    uint64_t rx_shm_later;     /* ring, after >=1 poll timeout        */
    uint64_t rx_uds_prompt;    /* uds, zero timeouts waited           */
    uint64_t rx_uds_later;     /* uds, after >=1 timeout              */
    uint64_t poll_calls, poll_timeouts, poll_wait_ns;
    uint64_t recv_loop_iters; /* total ring-poll iterations          */
} adapt_stats_t;

/* Copy this context's counters (zeros unless ADAPTIPC_STATS=1). */
void adapt_get_stats(adapt_ctx_t *ctx, adapt_stats_t *out);

typedef enum {
    ADAPT_ROLE_PRODUCER = 0,   /* calls adapt_send(); peer calls adapt_recv() */
    ADAPT_ROLE_CONSUMER = 1
} adapt_role_t;

typedef enum {
    ADAPT_ROUTE_NONE = 0,
    ADAPT_ROUTE_SHM  = 1,
    ADAPT_ROUTE_UDS  = 2
} adapt_route_t;

typedef struct adapt_ctx adapt_ctx_t;

typedef struct adapt_config {
    const char *shm_name;      /* NULL -> ADAPT_SHM_NAME_DEFAULT */
    const char *local_sock;    /* this endpoint's UDS path  (required) */
    const char *peer_sock;     /* peer endpoint's UDS path  (required) */
    size_t      shm_capacity;  /* payload capacity bytes; 0 -> default 64 KiB */
} adapt_config_t;

/* Initialize both transports and reset EWMA state to 0. */
int adapt_init(adapt_role_t role, const adapt_config_t *cfg, adapt_ctx_t **out);

/*
 * Classify, route and transmit `size` bytes from `data`.
 * Returns 0 on success, negative errno otherwise. Never blocks on SHM-full:
 * returns -EAGAIN if the ring cannot currently fit the message.
 */
int adapt_send(adapt_ctx_t *ctx, const void *data, size_t size);

/* Receive one message into buf (any transport). Blocks on the UDS socket
 * only after an SHM poll finds nothing. Returns byte count (>0),
 * or -EAGAIN when nonblocking mode is set, negative errno otherwise. */
int adapt_recv(adapt_ctx_t *ctx, void *buf, size_t max_size);

/* Introspection for tests / instrumentation. */
adapt_route_t  adapt_last_route(const adapt_ctx_t *ctx);
double         adapt_ewma(const adapt_ctx_t *ctx);

void adapt_shutdown(adapt_ctx_t *ctx);

const char *adapt_route_name(adapt_route_t r);
const char *adapt_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* ADAPT_IPC_H */
