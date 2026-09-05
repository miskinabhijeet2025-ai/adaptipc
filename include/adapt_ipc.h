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
    /* lazy endpoint negotiation */
    uint64_t shm_setup_reqs, shm_setup_acks;
    uint64_t negot_wait_ns;   /* time producer spent waiting for ACK */
    /* context-aware policy (all policies; zeros when not applicable) */
    uint64_t route_switches;        /* transport changes by the policy  */
    uint64_t backpressure_parks;    /* producer parks at the HW         */
    uint64_t health_transitions;    /* SHM health state changes         */
    uint64_t decisions;             /* decisions evaluated              */
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

/*
 * Routing policy modes (ablation ladder; see cost_model.h for the
 * cost model behind modes >= QUEUE_AWARE).
 */
typedef enum {
    ADAPT_POLICY_DEFAULT = 0,        /* == SIZE_HYSTERESIS (compat)      */
    ADAPT_POLICY_SIZE_ONLY = 1,      /* raw EWMA threshold, no deadband  */
    ADAPT_POLICY_SIZE_HYSTERESIS = 2,/* original validated policy        */
    ADAPT_POLICY_QUEUE_AWARE = 3,    /* + queue-wait penalty on SHM      */
    ADAPT_POLICY_COST_AWARE = 4,     /* + measured costs + switch margin */
    ADAPT_POLICY_FULL_ADAPTIVE = 5   /* + health + learned crossover+QoS */
} adapt_policy_mode_t;

typedef enum {
    ADAPT_QOS_BALANCED = 0,          /* combined cost (default)          */
    ADAPT_QOS_LATENCY = 1,           /* double weight on latency terms   */
    ADAPT_QOS_THROUGHPUT = 2         /* ignore latency budget            */
} adapt_qos_t;

typedef enum {
    ADAPT_HEALTH_UNAVAILABLE = 0,    /* not mapped / not negotiated      */
    ADAPT_HEALTH_HEALTHY = 1,
    ADAPT_HEALTH_DEGRADED = 2,       /* high occupancy or slow drain     */
    ADAPT_HEALTH_BLOCKED = 3,        /* ~full ring, producer parked      */
    ADAPT_HEALTH_RECOVERING = 4      /* drained, probation before HEALTHY */
} adapt_health_state_t;

const char *adapt_policy_name(adapt_policy_mode_t p);
const char *adapt_qos_name(adapt_qos_t q);
const char *adapt_health_name(adapt_health_state_t h);

typedef struct adapt_ctx adapt_ctx_t;

typedef struct adapt_config {
    const char *shm_name;      /* NULL -> ADAPT_SHM_NAME_DEFAULT (or a
                                * generated name under lazy negotiation) */
    const char *local_sock;    /* this endpoint's UDS path  (required) */
    const char *peer_sock;     /* peer endpoint's UDS path  (required) */
    size_t      shm_capacity;  /* payload capacity bytes; 0 -> default 64 KiB */
    /* When 0 (default), the producer blocks at the ring's high watermark
     * (80% capacity, futex-parked) instead of returning -EAGAIN. Set to 1
     * for the legacy never-blocks behavior. */
    int         nonblocking_send;
    /* Context-aware policy selection. ADAPT_POLICY_DEFAULT (=0) keeps
     * the validated size+EWMA+hysteresis behavior; higher modes add
     * queue-awareness, cost-awareness, health and QoS (see
     * cost_model.h). Env override: ADAPTIPC_POLICY. */
    adapt_policy_mode_t policy;
    /* QoS posture and optional end-to-end latency budget in us
     * (0 = unlimited). Env: ADAPTIPC_QOS, ADAPTIPC_LATENCY_BUDGET_US. */
    adapt_qos_t qos;
    double      latency_budget_us;
} adapt_config_t;

/*
 * Initialize the endpoint and reset EWMA state to 0. The UDS fallback is
 * bound immediately; the SHM ring is NOT mapped here. Under lazy
 * negotiation (default) the ring is created on demand the first time the
 * EWMA classifier routes to SHM, via a SHM_SETUP_REQ/ACK handshake over
 * the UDS control channel. Pass ADAPTIPC_EAGER_SHM=1 to restore eager
 * mapping.
 */
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

/* Context-aware policy introspection. */
adapt_policy_mode_t adapt_policy(const adapt_ctx_t *ctx);
double              adapt_crossover(const adapt_ctx_t *ctx); /* -1 unknown */
adapt_health_state_t adapt_shm_health(const adapt_ctx_t *ctx);

/* Bytes currently queued in the SHM ring (0 when no ring is mapped).
 * Approximate lock-free snapshot; used by tests to observe backpressure
 * and drain state without touching the ring directly. */
size_t adapt_shm_used_bytes(const adapt_ctx_t *ctx);

/* Payload capacity of the mapped ring (0 when no ring is mapped). */
size_t adapt_shm_capacity(const adapt_ctx_t *ctx);

void adapt_shutdown(adapt_ctx_t *ctx);

const char *adapt_route_name(adapt_route_t r);
const char *adapt_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* ADAPT_IPC_H */
